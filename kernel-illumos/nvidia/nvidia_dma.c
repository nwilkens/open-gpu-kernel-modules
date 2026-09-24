/*
 * SPDX-FileCopyrightText: Copyright (c) 1999-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * DMA mappings of system memory, MMIO and peer BARs.
 */

#include "nv-illumos.h"
#include "nv-reg.h"
#include "nvidia_mem.h"

#include <sys/vmem.h>
#include <sys/vmsystm.h>
#include <sys/gfx_private.h>
#include <sys/ddi_impldefs.h>
#include <sys/pci_impl.h>
#include <vm/hat.h>
#include <vm/as.h>
#include <vm/seg.h>
#include <vm/seg_kmem.h>
#include <vm/page.h>

/*
 * DMA mapping.  Without an IOMMU translating for the GPU, bus addresses are
 * CPU physical addresses and RM uses the page table directly.  Otherwise the
 * pages are bound through a kernel mapping, which gives the rootnex the page
 * frames it needs to set up the IOMMU.
 */
typedef struct nv_dma_map_s {
    ddi_dma_handle_t    handle;
    caddr_t             kva;
    NvU64               page_count;
    NvBool              own_kva;
} nv_dma_map_t;

/*
 * Detect DMA remapping by binding a page and comparing the cookie with the
 * page's physical address.
 */
NvBool
nv_dma_detect_remap(dev_info_t *dip)
{
    ddi_dma_attr_t attr;
    ddi_dma_handle_t h;
    ddi_acc_handle_t acc;
    ddi_dma_cookie_t cookie;
    caddr_t kva;
    size_t len;
    uint_t ccount;
    NvBool remap = NV_TRUE;

    nv_dma_attr_init(&attr, UINT64_MAX, MMU_PAGESIZE, 1);

    if (ddi_dma_alloc_handle(dip, &attr, DDI_DMA_SLEEP, NULL, &h) != DDI_SUCCESS)
        return NV_TRUE;

    if (ddi_dma_mem_alloc(h, MMU_PAGESIZE, &nv_sysmem_acc_attr,
            DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL, &kva, &len, &acc) != DDI_SUCCESS)
    {
        ddi_dma_free_handle(&h);
        return NV_TRUE;
    }

    if (ddi_dma_addr_bind_handle(h, NULL, kva, MMU_PAGESIZE,
            DDI_DMA_RDWR | DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL,
            &cookie, &ccount) == DDI_DMA_MAPPED)
    {
        remap = (cookie.dmac_laddress != nv_kva_to_phys(kva));
        (void) ddi_dma_unbind_handle(h);
    }

    ddi_dma_mem_free(&acc);
    ddi_dma_free_handle(&h);
    return remap;
}

static NV_STATUS
nv_dma_bind(nv_dma_device_t *dma_dev, caddr_t kva, NvU64 page_count,
    NvBool contig, NvBool read_only, NvU64 *va_array, ddi_dma_handle_t *hp)
{
    ddi_dma_attr_t attr;
    ddi_dma_cookie_t cookie;
    uint_t ccount, c;
    NvU64 page = 0;
    uint_t dir = read_only ? DDI_DMA_WRITE : DDI_DMA_RDWR;

    nv_dma_attr_init(&attr, dma_dev->addressable_range.limit, MMU_PAGESIZE,
        contig ? 1 : (int)MIN(page_count, (NvU64)INT32_MAX));

    if (ddi_dma_alloc_handle(dma_dev->dip, &attr, DDI_DMA_SLEEP, NULL, hp) !=
        DDI_SUCCESS)
        return NV_ERR_NO_MEMORY;

    if (ddi_dma_addr_bind_handle(*hp, NULL, kva, (size_t)mmu_ptob(page_count),
            dir | DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL, &cookie,
            &ccount) != DDI_DMA_MAPPED)
    {
        ddi_dma_free_handle(hp);
        return NV_ERR_OPERATING_SYSTEM;
    }

    for (c = 0; c < ccount; c++)
    {
        NvU64 off;

        for (off = 0; off < cookie.dmac_size && page < page_count;
             off += MMU_PAGESIZE, page++)
        {
            if (!contig)
                va_array[page] = cookie.dmac_laddress + off;
            else if (page == 0)
                va_array[0] = cookie.dmac_laddress;
        }

        if (c + 1 < ccount)
            ddi_dma_nextcookie(*hp, &cookie);
    }

    if (page != page_count)
    {
        (void) ddi_dma_unbind_handle(*hp);
        ddi_dma_free_handle(hp);
        return NV_ERR_OPERATING_SYSTEM;
    }

    return NV_OK;
}

NV_STATUS NV_API_CALL nv_dma_map_alloc(
    nv_dma_device_t *dma_dev,
    NvU64            page_count,
    NvU64           *va_array,
    NvBool           contig,
    NvBool           bReadOnlyDeviceMap,
    void           **priv
)
{
    nv_illumos_alloc_t *at = *priv;
    nv_dma_map_t *map;
    NV_STATUS status;

    if (dma_dev == NULL || dma_dev->nvis == NULL || page_count == 0)
        return NV_ERR_INVALID_ARGUMENT;

    map = kmem_zalloc(sizeof (*map), KM_SLEEP);
    map->page_count = page_count;

    if (!dma_dev->nvis->dma_remap)
    {
        /* Identity: va_array already holds bus addresses. */
        *priv = map;
        return NV_OK;
    }

    if (at != NULL && at->num_pages == page_count && nv_alloc_kva(at) != NULL)
    {
        map->kva = at->kva;
    }
    else
    {
        map->kva = nv_kmap_phys(va_array, page_count, contig,
            (at != NULL) ? at->cache_type : NV_MEMORY_CACHED);
        if (map->kva == NULL)
        {
            kmem_free(map, sizeof (*map));
            return NV_ERR_NO_MEMORY;
        }
        map->own_kva = NV_TRUE;
    }

    status = nv_dma_bind(dma_dev, map->kva, page_count, contig,
        bReadOnlyDeviceMap, va_array, &map->handle);
    if (status != NV_OK)
    {
        if (map->own_kva)
            nv_kunmap_phys(map->kva, page_count);
        kmem_free(map, sizeof (*map));
        return status;
    }

    *priv = map;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_dma_unmap_alloc(
    nv_dma_device_t *dma_dev,
    NvU64            page_count,
    NvU64           *va_array,
    void           **priv
)
{
    nv_dma_map_t *map;

    if (priv == NULL || *priv == NULL)
        return NV_ERR_NOT_SUPPORTED;

    map = *priv;

    if (map->handle != NULL)
    {
        (void) ddi_dma_unbind_handle(map->handle);
        ddi_dma_free_handle(&map->handle);
    }

    if (map->own_kva)
        nv_kunmap_phys(map->kva, map->page_count);

    kmem_free(map, sizeof (*map));
    *priv = NULL;
    return NV_OK;
}

/*
 * MMIO mappings for peer-to-peer.  With remapping enabled the range is bound
 * through an uncached kernel mapping of the BAR pages; the binding is kept in
 * a small list so the unmap can release it.
 */
typedef struct nv_mmio_map_s {
    list_node_t         link;
    nv_dma_device_t    *dma_dev;
    NvU64               dma_addr;
    NvU64               page_count;
    caddr_t             kva;
    ddi_dma_handle_t    handle;
} nv_mmio_map_t;

static list_t   nv_mmio_maps;
static kmutex_t nv_mmio_maps_lock;

void
nv_dma_init(void)
{
    mutex_init(&nv_mmio_maps_lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&nv_mmio_maps, sizeof (nv_mmio_map_t),
        offsetof(nv_mmio_map_t, link));
}

void
nv_dma_fini(void)
{
    list_destroy(&nv_mmio_maps);
    mutex_destroy(&nv_mmio_maps_lock);
}

NV_STATUS NV_API_CALL nv_dma_map_mmio(
    nv_dma_device_t *dma_dev,
    NvU64            page_count,
    NvU64           *va
)
{
    nv_mmio_map_t *m;
    NV_STATUS status;

    if (dma_dev == NULL || dma_dev->nvis == NULL || va == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (!dma_dev->nvis->dma_remap || !nv_dma_remap_peer_mmio)
    {
        *va = *va + dma_dev->addressable_range.start;
        return NV_OK;
    }

    m = kmem_zalloc(sizeof (*m), KM_SLEEP);
    m->dma_dev = dma_dev;
    m->page_count = page_count;
    m->kva = nv_kmap_phys(va, page_count, NV_TRUE, NV_MEMORY_UNCACHED);
    if (m->kva == NULL)
    {
        kmem_free(m, sizeof (*m));
        return NV_ERR_NO_MEMORY;
    }

    status = nv_dma_bind(dma_dev, m->kva, page_count, NV_TRUE, NV_FALSE, va,
        &m->handle);
    if (status != NV_OK)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, NV_STATE_PTR(dma_dev->nvis),
            "Failed to DMA map MMIO range\n");
        nv_kunmap_phys(m->kva, page_count);
        kmem_free(m, sizeof (*m));
        return NV_ERR_OPERATING_SYSTEM;
    }

    m->dma_addr = *va;

    mutex_enter(&nv_mmio_maps_lock);
    list_insert_tail(&nv_mmio_maps, m);
    mutex_exit(&nv_mmio_maps_lock);

    return NV_OK;
}

void NV_API_CALL nv_dma_unmap_mmio(
    nv_dma_device_t *dma_dev,
    NvU64            page_count,
    NvU64            va
)
{
    nv_mmio_map_t *m;

    if (dma_dev == NULL || dma_dev->nvis == NULL ||
        !dma_dev->nvis->dma_remap || !nv_dma_remap_peer_mmio)
        return;

    mutex_enter(&nv_mmio_maps_lock);
    for (m = list_head(&nv_mmio_maps); m != NULL;
         m = list_next(&nv_mmio_maps, m))
    {
        if (m->dma_dev == dma_dev && m->dma_addr == va &&
            m->page_count == page_count)
        {
            list_remove(&nv_mmio_maps, m);
            break;
        }
    }
    mutex_exit(&nv_mmio_maps_lock);

    if (m == NULL)
        return;

    (void) ddi_dma_unbind_handle(m->handle);
    ddi_dma_free_handle(&m->handle);
    nv_kunmap_phys(m->kva, m->page_count);
    kmem_free(m, sizeof (*m));
}

NV_STATUS NV_API_CALL nv_dma_map_peer(
    nv_dma_device_t *dma_dev,
    nv_dma_device_t *peer_dma_dev,
    NvU8             bar_index,
    NvU64            page_count,
    NvU64           *va
)
{
    nv_state_t *peer;
    nv_aperture_t *bar;

    if (peer_dma_dev == NULL || peer_dma_dev->nvis == NULL ||
        bar_index >= NV_GPU_NUM_BARS)
        return NV_ERR_INVALID_REQUEST;

    peer = NV_STATE_PTR(peer_dma_dev->nvis);
    bar = &peer->bars[bar_index];

    if (bar->cpu_address == 0 || *va < bar->cpu_address ||
        *va + mmu_ptob(page_count) > bar->cpu_address + bar->size)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, peer,
            "Mapping requested (start = 0x%llx, page_count = 0x%llx)"
            " outside of BAR %u\n", *va, page_count, bar_index);
        return NV_ERR_INVALID_REQUEST;
    }

    /* x86 PCI bus addresses equal CPU physical addresses. */
    return nv_dma_map_mmio(dma_dev, page_count, va);
}

void NV_API_CALL nv_dma_unmap_peer(
    nv_dma_device_t *dma_dev,
    NvU64            page_count,
    NvU64            va
)
{
    nv_dma_unmap_mmio(dma_dev, page_count, va);
}

/* x86 DMA is cache coherent. */
void NV_API_CALL nv_dma_cache_invalidate(nv_dma_device_t *dma_dev, void *priv)
{
}

/* There is no device pagemap (ZONE_DEVICE) equivalent on illumos. */
void* NV_API_CALL nv_dma_get_dev_pagemap(NvU64 pfn)
{
    return NULL;
}

void NV_API_CALL nv_dma_put_dev_pagemap(void *pgmap)
{
}

/* The sysmem window is only used on cache-coherent NVLink-C2C platforms. */
NV_STATUS NV_API_CALL nv_dma_init_sysmem_window_for_fabric_access(
    nv_state_t *nv, NvU64 base, NvU64 *start, NvU64 *limit, NvBool *remap)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_dma_destroy_sysmem_window_for_fabric_access(
    nv_state_t *nv, NvU64 start, NvU64 limit)
{
}

/* dma-buf does not exist on illumos. */
NV_STATUS NV_API_CALL nv_dma_import_sgt(nv_dma_device_t *dma_dev,
    struct sg_table *sgt, struct drm_gem_object *gem)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_dma_release_sgt(struct sg_table *sgt,
    struct drm_gem_object *gem)
{
}

NV_STATUS NV_API_CALL nv_dma_import_dma_buf(nv_dma_device_t *dma_dev,
    struct dma_buf *dma_buf, NvBool is_ro_device_map, NvU32 *size,
    struct sg_table **sgt, nv_dma_buf_t **import_priv)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_dma_import_from_fd(nv_dma_device_t *dma_dev,
    NvS32 fd, NvBool is_ro_device_map, NvU32 *size, struct sg_table **sgt,
    nv_dma_buf_t **import_priv)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_dma_release_dma_buf(nv_dma_buf_t *import_priv)
{
}

/*
 * GPUDirect RDMA peers are allowed unless the chipset is known to break
 * peer-to-peer, in which case both devices must share a PCIe switch.
 */
NvBool NV_API_CALL nv_grdma_pci_topology_supported(nv_state_t *nv,
    nv_dma_device_t *dma_peer)
{
    dev_info_t *a, *b;

    if (nv->coherent || (nv->flags & NV_FLAG_PASSTHRU) != 0)
        return NV_TRUE;

    if ((nv->flags & NV_FLAG_PCI_P2P_UNSUPPORTED_CHIPSET) == 0)
        return NV_TRUE;

    if (dma_peer == NULL || dma_peer->dip == NULL)
        return NV_FALSE;

    /* A common ancestor below the root complex is a switch port. */
    for (a = ddi_get_parent(NV_GET_NVIS(nv)->dip); a != NULL;
         a = ddi_get_parent(a))
    {
        for (b = ddi_get_parent(dma_peer->dip); b != NULL;
             b = ddi_get_parent(b))
        {
            if (a == b)
            {
                return ddi_prop_exists(DDI_DEV_T_ANY, a, DDI_PROP_DONTPASS,
                    "vendor-id") ? NV_TRUE : NV_FALSE;
            }
        }
    }

    return NV_FALSE;
}

void NV_API_CALL nv_set_dma_address_size(nv_state_t *nv, NvU32 phys_addr_bits)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    NvU64 mask = (phys_addr_bits >= 64) ? UINT64_MAX :
        ((1ULL << phys_addr_bits) - 1);

    nvis->dma_dev.addressable_range.limit = mask;
    nvis->niso_dma_dev.addressable_range.limit = mask;
    nv->dma_mask = mask;
}

NvBool NV_API_CALL nv_requires_dma_remap(nv_state_t *nv)
{
    return NV_GET_NVIS(nv)->dma_remap;
}

/* x86 keeps CPU caches coherent with device accesses. */
void NV_API_CALL nv_flush_coherent_cpu_cache_range(nv_state_t *nv,
    NvU64 cpu_virtual, NvU64 size)
{
}

/* Device memory as system memory exists only on coherent (C2C) platforms. */
NV_STATUS NV_API_CALL nv_get_device_memory_config(nv_state_t *nv,
    NvU64 *compr_addr_sys_phys, NvU64 *addr_guest_phys, NvU64 *rsvd_phys,
    NvU64 *size, NvU32 *addr_width, NvS32 *node_id)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_egm_info(nv_state_t *nv, NvU64 *phys_addr,
    NvU64 *size, NvS32 *egm_node_id)
{
    return NV_ERR_NOT_SUPPORTED;
}
