/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


/*
 * DMA mappings of UVM pages for a GPU.  Without an IOMMU remapping for the
 * GPU, it reaches system memory at its physical address.  Otherwise each
 * mapping is a DMA handle bound to the pages, looked up by bus address when
 * UVM unmaps it.
 *
 * Binds pass a shadow page list rather than a bare kernel address, because
 * the Intel IOMMU code reads an uninitialized address space pointer for the
 * latter, and never wait, because the AMD IOMMU code can sleep for IOVA space
 * while holding the lock that frees it.  A bind that needed a copy buffer is
 * refused: UVM never calls ddi_dma_sync().
 */

#include "uvm_illumos.h"
#include "uvm_illumos_mem.h"

#include <sys/avl.h>
#include <sys/buf.h>
#include <sys/ddi_impldefs.h>
#include <vm/hat.h>
#include <vm/seg_kpm.h>

extern caddr_t hat_kpm_pfn2va(pfn_t);

/* The largest allocation UVM maps, a 2MB CPU chunk. */
#define UVM_DMA_MAX_SIZE        (2UL << 20)
#define UVM_DMA_DEVS            32

/* Remapping state per GPU, found by probing it. */
static kmutex_t uvm_dma_lock;
static struct {
    dev_info_t *dip;
    boolean_t   remap;
} uvm_dma_devs[UVM_DMA_DEVS];

typedef struct uvm_dma_map {
    avl_node_t          udm_link;
    dev_info_t         *udm_dip;
    dma_addr_t          udm_addr;
    uint64_t            udm_seq;        /* identity binds may repeat addresses */
    size_t              udm_size;
    ddi_dma_handle_t    udm_handle;
    page_t            **udm_shadow;
} uvm_dma_map_t;

static kmutex_t     uvm_dma_map_lock;
static avl_tree_t   uvm_dma_maps;
static uint64_t     uvm_dma_seq;

/*
 * Pages of mappings that could not be unbound.  The device may still reach
 * them, so their allocation is never freed.
 */
typedef struct uvm_dma_bad {
    avl_node_t          udb_link;
    page_t             *udb_page;
} uvm_dma_bad_t;

static avl_tree_t       uvm_dma_bad_pages;
static volatile ulong_t uvm_dma_bad_count;

static int
uvm_dma_map_compare(const void *a, const void *b)
{
    const uvm_dma_map_t *x = a, *y = b;

    if (x->udm_dip != y->udm_dip)
        return ((uintptr_t)x->udm_dip < (uintptr_t)y->udm_dip ? -1 : 1);
    if (x->udm_addr != y->udm_addr)
        return (x->udm_addr < y->udm_addr ? -1 : 1);
    if (x->udm_seq != y->udm_seq)
        return (x->udm_seq < y->udm_seq ? -1 : 1);
    return (0);
}

static int
uvm_dma_bad_compare(const void *a, const void *b)
{
    const uvm_dma_bad_t *x = a, *y = b;

    if (x->udb_page == y->udb_page)
        return (0);
    return ((uintptr_t)x->udb_page < (uintptr_t)y->udb_page ? -1 : 1);
}

static void
uvm_dma_quarantine(page_t **pages, pgcnt_t npages)
{
    pgcnt_t i;

    mutex_enter(&uvm_dma_map_lock);
    for (i = 0; i < npages; i++) {
        uvm_dma_bad_t key, *b;

        key.udb_page = pages[i];
        if (avl_find(&uvm_dma_bad_pages, &key, NULL) != NULL)
            continue;
        b = kmem_zalloc(sizeof (*b), KM_SLEEP);
        b->udb_page = pages[i];
        avl_add(&uvm_dma_bad_pages, b);
        uvm_dma_bad_count++;
    }
    mutex_exit(&uvm_dma_map_lock);
}

/* May the allocation of npages from pp be freed? */
boolean_t
uvm_dma_may_free(page_t *pp, pgcnt_t npages)
{
    boolean_t ok = B_TRUE;
    pgcnt_t i;

    if (uvm_dma_bad_count == 0)
        return (B_TRUE);

    mutex_enter(&uvm_dma_map_lock);
    for (i = 0; i < npages && ok; i++) {
        uvm_dma_bad_t key;

        key.udb_page = pp + i;
        ok = (avl_find(&uvm_dma_bad_pages, &key, NULL) == NULL);
    }
    mutex_exit(&uvm_dma_map_lock);

    return (ok);
}

static void
uvm_dma_attr(ddi_dma_attr_t *attr, uint64_t lo, uint64_t hi, size_t align)
{
    bzero(attr, sizeof (*attr));
    attr->dma_attr_version = DMA_ATTR_V0;
    attr->dma_attr_addr_lo = lo;
    attr->dma_attr_addr_hi = hi;
    attr->dma_attr_count_max = UINT64_MAX;
    attr->dma_attr_align = align;
    attr->dma_attr_burstsizes = 0xfff;
    attr->dma_attr_minxfer = 1;
    attr->dma_attr_maxxfer = UINT64_MAX;
    attr->dma_attr_seg = UINT64_MAX;
    attr->dma_attr_sgllen = 1;
    attr->dma_attr_granular = 1;
}

/*
 * Bind npages of physically contiguous pages, listed in shadow, as one bus
 * range.  Returns the cookie address or DMA_MAPPING_ERROR; on failure
 * nothing stays bound unless *stuck is set.
 */
static dma_addr_t
uvm_dma_bind(dev_info_t *dip, const ddi_dma_attr_t *attr, page_t **shadow,
    pgcnt_t npages, ddi_dma_handle_t *hp, boolean_t *stuck)
{
    size_t size = ptob(npages);
    ddi_dma_cookie_t cookie;
    ddi_dma_impl_t *impl;
    struct buf bp;
    uint_t ccount;

    *stuck = B_FALSE;

    if (ddi_dma_alloc_handle(dip, (ddi_dma_attr_t *)attr, DDI_DMA_DONTWAIT,
        NULL, hp) != DDI_SUCCESS)
        return (DMA_MAPPING_ERROR);

    bzero(&bp, sizeof (bp));
    bp.b_flags = B_BUSY | B_SHADOW;
    bp.b_shadow = shadow;
    bp.b_un.b_addr = hat_kpm_pfn2va(shadow[0]->p_pagenum);
    bp.b_bcount = size;

    if (ddi_dma_buf_bind_handle(*hp, &bp, DDI_DMA_RDWR | DDI_DMA_CONSISTENT,
        DDI_DMA_DONTWAIT, NULL, &cookie, &ccount) != DDI_DMA_MAPPED) {
        ddi_dma_free_handle(hp);
        return (DMA_MAPPING_ERROR);
    }

    /* The root nexus clears DMP_NOSYNC when it sets up a copy buffer. */
    impl = (ddi_dma_impl_t *)*hp;
    if (ccount == 1 && cookie.dmac_size == size &&
        (impl->dmai_rflags & DMP_NOSYNC) == DMP_NOSYNC &&
        (cookie.dmac_laddress & (attr->dma_attr_align - 1)) == 0 &&
        cookie.dmac_laddress >= attr->dma_attr_addr_lo &&
        cookie.dmac_laddress + size - 1 >= cookie.dmac_laddress &&
        cookie.dmac_laddress + size - 1 <= attr->dma_attr_addr_hi)
        return (cookie.dmac_laddress);

    if (ddi_dma_unbind_handle(*hp) != DDI_SUCCESS) {
        *stuck = B_TRUE;
        return (DMA_MAPPING_ERROR);
    }
    ddi_dma_free_handle(hp);
    return (DMA_MAPPING_ERROR);
}

/*
 * Does the GPU see system memory through a remapping IOMMU?  Bind two pages
 * and compare bus with physical addresses; one equal pair could be chance.
 * A probe that cannot finish counts as remapped, without being cached.
 */
static boolean_t
uvm_dma_remapped(dev_info_t *dip)
{
    ddi_dma_attr_t attr;
    ddi_dma_handle_t h;
    page_t *pp[2] = { NULL, NULL };
    boolean_t remap = B_FALSE, known = B_TRUE, stuck;
    uint_t i;

    mutex_enter(&uvm_dma_lock);
    for (i = 0; i < UVM_DMA_DEVS && uvm_dma_devs[i].dip != NULL; i++) {
        if (uvm_dma_devs[i].dip == dip) {
            remap = uvm_dma_devs[i].remap;
            mutex_exit(&uvm_dma_lock);
            return (remap);
        }
    }

    uvm_dma_attr(&attr, 0, UINT64_MAX, PAGESIZE);
    for (i = 0; i < 2 && known; i++) {
        dma_addr_t addr;

        pp[i] = linux_alloc_pages(0, 0);
        if (pp[i] == NULL) {
            known = B_FALSE;
            break;
        }
        addr = uvm_dma_bind(dip, &attr, &pp[i], 1, &h, &stuck);
        if (addr == DMA_MAPPING_ERROR) {
            known = B_FALSE;
            if (stuck)
                uvm_dma_quarantine(&pp[i], 1);
            break;
        }
        if (addr != ptob((uint64_t)pp[i]->p_pagenum))
            remap = B_TRUE;
        if (ddi_dma_unbind_handle(h) != DDI_SUCCESS) {
            uvm_dma_quarantine(&pp[i], 1);
            known = B_FALSE;
            break;
        }
        ddi_dma_free_handle(&h);
    }

    /* Quarantined pages are kept, and charged, by linux_free_pages(). */
    for (i = 0; i < 2; i++) {
        if (pp[i] != NULL)
            linux_free_pages(pp[i], 0);
    }

    if (!known) {
        mutex_exit(&uvm_dma_lock);
        return (B_TRUE);
    }

    for (i = 0; i < UVM_DMA_DEVS; i++) {
        if (uvm_dma_devs[i].dip == NULL) {
            uvm_dma_devs[i].dip = dip;
            uvm_dma_devs[i].remap = remap;
            break;
        }
    }
    mutex_exit(&uvm_dma_lock);

    return (remap);
}

/*
 * UVM maps whole allocations of ours: physically contiguous pages with
 * adjacent page_ts.  Anything else would hand the GPU memory we do not own.
 */
static boolean_t
uvm_dma_pages_ok(page_t *pp, pgcnt_t npages)
{
    pgcnt_t i;

    for (i = 0; i < npages; i++) {
        if (page_numtopp_nolock(pp->p_pagenum + i) != pp + i ||
            !uvm_page_owned(pp + i))
            return (B_FALSE);
    }
    return (B_TRUE);
}

static dma_addr_t
uvm_dma_map_bound(struct device *dev, page_t *pp, size_t size)
{
    pgcnt_t npages = btop(size), i;
    ddi_dma_attr_t attr;
    uvm_dma_map_t *m;
    boolean_t stuck;

    /* UVM maps chunks with GPU pages as large as their alignment allows. */
    if (dev->dma_limit <= dev->dma_start)
        return (DMA_MAPPING_ERROR);
    uvm_dma_attr(&attr, dev->dma_start, dev->dma_limit,
        ISP2(size) ? size : PAGESIZE);

    m = kmem_zalloc(sizeof (*m), KM_NOSLEEP);
    if (m == NULL)
        return (DMA_MAPPING_ERROR);
    m->udm_shadow = kmem_alloc(npages * sizeof (page_t *), KM_NOSLEEP);
    if (m->udm_shadow == NULL) {
        kmem_free(m, sizeof (*m));
        return (DMA_MAPPING_ERROR);
    }
    for (i = 0; i < npages; i++)
        m->udm_shadow[i] = pp + i;

    m->udm_addr = uvm_dma_bind(dev->dip, &attr, m->udm_shadow, npages,
        &m->udm_handle, &stuck);

    /* UVM takes a GPU address of zero as unmapped. */
    if (m->udm_addr == dev->dma_start) {
        if (ddi_dma_unbind_handle(m->udm_handle) != DDI_SUCCESS)
            stuck = B_TRUE;
        else
            ddi_dma_free_handle(&m->udm_handle);
        m->udm_addr = DMA_MAPPING_ERROR;
    }

    if (m->udm_addr == DMA_MAPPING_ERROR) {
        if (stuck)
            uvm_dma_quarantine(m->udm_shadow, npages);
        kmem_free(m->udm_shadow, npages * sizeof (page_t *));
        kmem_free(m, sizeof (*m));
        return (DMA_MAPPING_ERROR);
    }

    m->udm_dip = dev->dip;
    m->udm_size = size;

    mutex_enter(&uvm_dma_map_lock);
    m->udm_seq = uvm_dma_seq++;
    avl_add(&uvm_dma_maps, m);
    mutex_exit(&uvm_dma_map_lock);

    return (m->udm_addr);
}

dma_addr_t
linux_dma_map_page(struct device *dev, struct page *pp, size_t off,
    size_t size)
{
    if (dev == NULL || dev->dip == NULL || pp == NULL || off != 0 ||
        size == 0 || (size & PAGEOFFSET) != 0 || size > UVM_DMA_MAX_SIZE ||
        !uvm_dma_pages_ok(pp, btop(size)))
        return (DMA_MAPPING_ERROR);

    if (!uvm_dma_remapped(dev->dip))
        return ((dma_addr_t)ptob((uint64_t)pp->p_pagenum));

    return (uvm_dma_map_bound(dev, pp, size));
}

void
linux_dma_unmap_page(struct device *dev, dma_addr_t addr, size_t size)
{
    uvm_dma_map_t key, *m;
    avl_index_t where;

    if (dev == NULL || dev->dip == NULL)
        return;

    /*
     * The mapping table, not a fresh probe, says whether addr was bound:
     * the probe can answer differently than it did at map time.
     */
    key.udm_dip = dev->dip;
    key.udm_addr = addr;
    key.udm_seq = 0;

    mutex_enter(&uvm_dma_map_lock);
    m = avl_find(&uvm_dma_maps, &key, &where);
    if (m == NULL)
        m = avl_nearest(&uvm_dma_maps, where, AVL_AFTER);
    if (m == NULL || m->udm_dip != dev->dip || m->udm_addr != addr ||
        m->udm_size != size) {
        mutex_exit(&uvm_dma_map_lock);
        if (uvm_dma_remapped(dev->dip)) {
            dev_err(dev->dip, CE_WARN, "nvidia_uvm: unmap of an unknown "
                "DMA mapping 0x%llx size 0x%lx", (u_longlong_t)addr, size);
        }
        return;
    }
    avl_remove(&uvm_dma_maps, m);
    mutex_exit(&uvm_dma_map_lock);

    if (ddi_dma_unbind_handle(m->udm_handle) != DDI_SUCCESS) {
        dev_err(dev->dip, CE_WARN, "nvidia_uvm: DMA unbind failed; "
            "keeping its pages");
        uvm_dma_quarantine(m->udm_shadow, btop(m->udm_size));
        return;
    }
    ddi_dma_free_handle(&m->udm_handle);
    kmem_free(m->udm_shadow, btop(m->udm_size) * sizeof (page_t *));
    kmem_free(m, sizeof (*m));
}

/* Coherent DMA memory is used only with Confidential Computing. */
void *
linux_dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle,
    gfp_t gfp)
{
    return (NULL);
}

void
linux_dma_free_coherent(struct device *dev, size_t size, void *va,
    dma_addr_t handle)
{
}

void
uvm_dma_init(void)
{
    mutex_init(&uvm_dma_lock, NULL, MUTEX_DRIVER, NULL);
    bzero(uvm_dma_devs, sizeof (uvm_dma_devs));
    mutex_init(&uvm_dma_map_lock, NULL, MUTEX_DRIVER, NULL);
    avl_create(&uvm_dma_maps, uvm_dma_map_compare, sizeof (uvm_dma_map_t),
        offsetof(uvm_dma_map_t, udm_link));
    avl_create(&uvm_dma_bad_pages, uvm_dma_bad_compare,
        sizeof (uvm_dma_bad_t), offsetof(uvm_dma_bad_t, udb_link));
    uvm_dma_bad_count = 0;
}

/* Leaked mappings and quarantined pages keep their state for good. */
void
uvm_dma_fini(void)
{
    if (avl_numnodes(&uvm_dma_maps) != 0 || uvm_dma_bad_count != 0) {
        cmn_err(CE_WARN, "nvidia_uvm: %lu DMA mappings and %lu pages leaked",
            avl_numnodes(&uvm_dma_maps), uvm_dma_bad_count);
        return;
    }

    avl_destroy(&uvm_dma_bad_pages);
    avl_destroy(&uvm_dma_maps);
    mutex_destroy(&uvm_dma_map_lock);
    mutex_destroy(&uvm_dma_lock);
}
