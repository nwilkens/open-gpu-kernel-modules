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
 * Pages for nvidia-uvm.  struct page is page_t: CPU memory comes from
 * page_create_va() and page_create_io() on vnodes private to this module,
 * stays share-locked with p_lckcnt set so that pageout and relocation leave
 * it alone, and is reached from the kernel through segkpm.
 */

#include "uvm_illumos.h"
#include "uvm_illumos_mem.h"

#include <sys/avl.h>
#include <sys/vmem.h>
#include <sys/vnode.h>
#include <sys/vfs_opreg.h>
#include <vm/as.h>
#include <vm/seg.h>
#include <vm/seg_kmem.h>
#include <vm/seg_kpm.h>
#include <vm/hat.h>

/* i86pc VM routines without a header that a driver can include. */
extern caddr_t hat_kpm_pfn2va(pfn_t);
extern page_t *page_create_io(vnode_t *, u_offset_t, uint_t, uint_t,
    struct as *, caddr_t, ddi_dma_attr_t *);

/* The largest CPU chunk UVM allocates is 2MB. */
#define UVM_PAGE_MAX_ORDER      9
#define UVM_PAGE_CONTIG_TRIES   8

/*
 * One vnode per order, so put_page() can tell how much to free, and a
 * second set for allocations charged to a project.
 */
static vnode_t         *uvm_page_vp[2][UVM_PAGE_MAX_ORDER + 1];
static vnodeops_t      *uvm_page_vnodeops;
static vmem_t          *uvm_page_off_arena;
static volatile ulong_t uvm_page_count;

static const fs_operation_def_t uvm_page_vnodeops_template[] = {
    { NULL, { NULL } }
};

static ddi_dma_attr_t uvm_page_contig_attr = {
    .dma_attr_version       = DMA_ATTR_V0,
    .dma_attr_addr_lo       = 0,
    .dma_attr_addr_hi       = UINT64_MAX,
    .dma_attr_count_max     = UINT64_MAX,
    .dma_attr_align         = PAGE_SIZE,
    .dma_attr_burstsizes    = 1,
    .dma_attr_minxfer       = 1,
    .dma_attr_maxxfer       = UINT64_MAX,
    .dma_attr_seg           = UINT64_MAX,
    .dma_attr_sgllen        = 1,
    .dma_attr_granular      = 1,
    .dma_attr_flags         = 0,
};

/* Undo page_create_*(): destroy one page and drop our lock on it. */
static void
uvm_page_destroy(page_t *pp)
{
    vnode_t *vp = pp->p_vnode;
    u_offset_t off = pp->p_offset;

    if (!page_tryupgrade(pp)) {
        page_unlock(pp);
        pp = page_lookup(vp, off, SE_EXCL);
        VERIFY(pp != NULL);
    }

    /* page_unresv() accounts for availrmem, as in segkmem_xfree(). */
    pp->p_lckcnt = 0;
    page_destroy(pp, 0);
}

/* Leave a newly created page share-locked, like segkmem pages on x86. */
static void
uvm_page_settle(page_t *pp, boolean_t zero)
{
    ASSERT(PAGE_EXCL(pp));
    if (page_iolock_assert(pp))
        page_io_unlock(pp);
    pp->p_lckcnt = 1;
    page_downgrade(pp);

    if (zero)
        bzero(hat_kpm_pfn2va(pp->p_pagenum), PAGESIZE);
}

/*
 * UVM indexes the page_t array of a multi-page allocation, so the pages
 * must be physically contiguous and their page_ts adjacent.  Returns the
 * page with the lowest frame number, or NULL.
 */
static page_t *
uvm_page_contig_head(page_t *plist, pgcnt_t npages)
{
    page_t *pp = plist, *head = plist;
    pgcnt_t i;

    do {
        if (pp->p_pagenum < head->p_pagenum)
            head = pp;
        pp = pp->p_next;
    } while (pp != plist);

    for (i = 0; i < npages; i++) {
        if (page_numtopp_nolock(head->p_pagenum + i) != head + i)
            return (NULL);
    }

    return (head);
}

/*
 * Wait for memory, as the page_xresv() callback and between page creation
 * attempts.  A signal ends the wait, so a process cannot wedge a thread
 * that holds UVM locks.  Kernel threads take no signals and keep waiting,
 * as Linux GFP_KERNEL does.
 */
static int
uvm_page_resv_wait(void)
{
    if (curproc == &p0) {
        delay(hz >> 2);
        return (1);
    }

    return (delay_sig(hz >> 2) == 0);
}

static page_t *
uvm_page_alloc(vnode_t **vps, gfp_t gfp, unsigned int order)
{
    struct seg kseg = { .s_as = &kas };
    boolean_t wait = (gfp & (__GFP_NOSLEEP | __GFP_NORETRY)) == 0;
    pgcnt_t npages, i;
    size_t size;
    void *off;
    page_t *plist, *head, *pp;
    uint_t tries;

    npages = 1UL << order;
    size = ptob(npages);

    if (page_xresv(npages, wait ? KM_SLEEP : KM_NOSLEEP,
        uvm_page_resv_wait) == 0)
        return (NULL);

    off = vmem_alloc(uvm_page_off_arena, size,
        (wait ? VM_SLEEP : VM_NOSLEEP) | VM_NEXTFIT);
    if (off == NULL) {
        page_unresv(npages);
        return (NULL);
    }

    /*
     * PG_WAIT would sleep without regard to signals, so waiting is done
     * here.  A physically contiguous run may be unavailable however long
     * one waits, so multi-page requests give up after a few tries.
     */
    for (tries = 0; ; tries++) {
        if (order == 0) {
            plist = page_create_va(vps[0],
                (u_offset_t)(uintptr_t)off, PAGESIZE, PG_EXCL | PG_NORELOC,
                &kseg, (caddr_t)off);
        } else {
            ddi_dma_attr_t attr = uvm_page_contig_attr;

            attr.dma_attr_align = size;
            plist = page_create_io(vps[order],
                (u_offset_t)(uintptr_t)off, (uint_t)size,
                PG_EXCL | PG_PHYSCONTIG, &kas, (caddr_t)off, &attr);
        }

        if (plist != NULL || !wait ||
            (order > 0 && tries >= UVM_PAGE_CONTIG_TRIES) ||
            uvm_page_resv_wait() == 0)
            break;
    }

    if (plist == NULL) {
        vmem_free(uvm_page_off_arena, off, size);
        page_unresv(npages);
        return (NULL);
    }

    head = (order == 0) ? plist : uvm_page_contig_head(plist, npages);

    for (i = 0; i < npages; i++) {
        pp = plist;
        page_sub(&plist, pp);
        uvm_page_settle(pp, head != NULL && (gfp & __GFP_ZERO) != 0);
        if (head == NULL)
            uvm_page_destroy(pp);
    }

    if (head == NULL) {
        vmem_free(uvm_page_off_arena, off, size);
        page_unresv(npages);
        return (NULL);
    }

    atomic_add_long(&uvm_page_count, npages);
    return (head);
}

static page_t *
uvm_page_alloc_charged(uvm_charge_t *charge, gfp_t gfp, unsigned int order)
{
    page_t *head = uvm_page_alloc(uvm_page_vp[charge != NULL], gfp, order);

    if (charge != NULL) {
        if (head != NULL)
            uvm_charge_page_add(charge, head);
        else
            uvm_charge_rele(charge);
    }

    return (head);
}

/*
 * __GFP_ACCOUNT allocations are charged to the process whose mm UVM made
 * the active memcg.  Managed CPU chunks (__GFP_HIGHMEM) are left out: their
 * range was charged when it was mapped.  With va_space_mm disabled the mm
 * is always the caller's own, so anything else is refused.
 */
struct page *
linux_alloc_pages(gfp_t gfp, unsigned int order)
{
    uvm_charge_t *charge = NULL;
    void *memcg;
    int err;

    if (order > UVM_PAGE_MAX_ORDER)
        return (NULL);

    if ((gfp & (__GFP_ACCOUNT | __GFP_HIGHMEM)) == __GFP_ACCOUNT &&
        (memcg = uvm_memcg_active()) != NULL) {
        if (memcg != (void *)curproc->p_as || (gfp & __GFP_NOSLEEP) != 0)
            return (NULL);
        charge = uvm_charge_take(ptob(1UL << order), &err);
        if (charge == NULL)
            return (NULL);
    }

    return (uvm_page_alloc_charged(charge, gfp, order));
}

/*
 * User GPU page tables, from uvm_mmu.c.  UVM allocates them on any thread,
 * even after the owner has exited, so they are charged to the owner of the
 * VA space whose mapping m is.
 */
struct page *
uvm_illumos_alloc_pages_owned(struct address_space *m, gfp_t gfp,
    unsigned int order)
{
    uvm_charge_t *charge;
    int err;

    if (order > UVM_PAGE_MAX_ORDER || (gfp & __GFP_NOSLEEP) != 0 ||
        m->am_owner == NULL)
        return (NULL);

    charge = uvm_charge_take_owner(m->am_owner, ptob(1UL << order), &err);
    if (charge == NULL)
        return (NULL);

    return (uvm_page_alloc_charged(charge, gfp, order));
}

/* The order of an allocation from its vnode, or -1 if it is not ours. */
static int
uvm_page_order(const page_t *pp, boolean_t *charged)
{
    unsigned int c, order;

    for (c = 0; c < 2; c++) {
        for (order = 0; order <= UVM_PAGE_MAX_ORDER; order++) {
            if (pp->p_vnode == uvm_page_vp[c][order]) {
                *charged = (c != 0);
                return ((int)order);
            }
        }
    }
    return (-1);
}

void
linux_free_pages(struct page *head, unsigned int order)
{
    uvm_charge_t *charge = NULL;
    boolean_t charged = B_FALSE;
    pgcnt_t npages, i;
    u_offset_t base;

    VERIFY(order <= UVM_PAGE_MAX_ORDER);
    VERIFY3S(uvm_page_order(head, &charged), ==, (int)order);

    npages = 1UL << order;
    if (charged)
        charge = uvm_charge_page_remove(head);

    if (!uvm_dma_may_free(head, npages)) {
        cmn_err(CE_WARN, "nvidia_uvm: keeping %lu pages a device may still "
            "reach", npages);
        uvm_acct_quarantine(ptob(npages));
        if (charge != NULL)
            uvm_charge_rele(charge);
        return;
    }

    base = head->p_offset;
    for (i = 1; i < npages; i++)
        base = MIN(base, head[i].p_offset);

    for (i = 0; i < npages; i++)
        uvm_page_destroy(head + i);

    vmem_free(uvm_page_off_arena, (void *)(uintptr_t)base, ptob(npages));
    page_unresv(npages);
    atomic_add_long(&uvm_page_count, -(long)npages);

    if (charge != NULL)
        uvm_charge_rele(charge);
}

/* UVM calls put_page() only to free the pages it allocated. */
void
linux_put_page(struct page *pp)
{
    boolean_t charged = B_FALSE;
    int order = uvm_page_order(pp, &charged);

    if (order < 0) {
        cmn_err(CE_WARN, "nvidia_uvm: put_page of a foreign page %p",
            (void *)pp);
        return;
    }
    linux_free_pages(pp, (unsigned int)order);
}

/* Is this one of our pages?  Cheap enough for every kmap(). */
boolean_t
uvm_page_owned(const page_t *pp)
{
    return (pp->p_vnode != NULL && pp->p_vnode->v_op == uvm_page_vnodeops);
}

/*
 * Pinned user pages are mapped so that revoking the pin can redirect them.
 * They are looked for first: the frame of a revoked pin may since have
 * become one of ours.
 */
void *
linux_page_address(struct page *pp)
{
    void *va = uvm_pin_kmap(pp);

    if (va != NULL || !uvm_page_owned(pp))
        return (va);
    return (hat_kpm_pfn2va(pp->p_pagenum));
}

unsigned long
linux_page_to_pfn(struct page *pp)
{
    return ((unsigned long)pp->p_pagenum);
}

struct page *
linux_pfn_to_page(unsigned long pfn)
{
    return (page_numtopp_nolock((pfn_t)pfn));
}

/*
 * UVM marks only pinned user pages dirty, and those get the modified bit
 * when they are unlocked for write.  A revoked pin may no longer own the
 * page by now, so it is not touched here.
 */
void
linux_set_page_dirty(struct page *pp)
{
}

/*
 * vmap: a kernel mapping of an array of pages.  The size is kept in an
 * AVL tree keyed by address for vunmap().
 */
typedef struct uvm_vmap {
    avl_node_t  uv_link;
    caddr_t     uv_va;
    size_t      uv_size;
} uvm_vmap_t;

static kmutex_t uvm_vmap_lock;
static avl_tree_t uvm_vmap_tree;

static int
uvm_vmap_compare(const void *a, const void *b)
{
    const uvm_vmap_t *x = a, *y = b;

    if (x->uv_va < y->uv_va)
        return (-1);
    return (x->uv_va > y->uv_va);
}

void *
linux_vmap(struct page **pages, unsigned int count)
{
    uvm_vmap_t *vm;
    void *va;
    unsigned int i;

    if (count == 0)
        return (NULL);

    if (uvm_pin_vmap(pages, count, &va))
        return (va);

    /* Anything else must be our own memory. */
    for (i = 0; i < count; i++) {
        if (!uvm_page_owned(pages[i]))
            return (NULL);
    }

    vm = kmem_zalloc(sizeof (*vm), KM_SLEEP);
    vm->uv_size = ptob((size_t)count);
    vm->uv_va = vmem_alloc(heap_arena, vm->uv_size, VM_SLEEP);

    for (i = 0; i < count; i++) {
        hat_devload(kas.a_hat, vm->uv_va + ptob(i), PAGESIZE,
            pages[i]->p_pagenum, PROT_READ | PROT_WRITE | HAT_NOSYNC,
            HAT_LOAD_LOCK);
    }

    mutex_enter(&uvm_vmap_lock);
    avl_add(&uvm_vmap_tree, vm);
    mutex_exit(&uvm_vmap_lock);

    return (vm->uv_va);
}

void
linux_vunmap(const void *va)
{
    uvm_vmap_t key, *vm;

    if (uvm_pin_vunmap(va))
        return;

    key.uv_va = (caddr_t)va;
    mutex_enter(&uvm_vmap_lock);
    vm = avl_find(&uvm_vmap_tree, &key, NULL);
    VERIFY(vm != NULL);
    avl_remove(&uvm_vmap_tree, vm);
    mutex_exit(&uvm_vmap_lock);

    hat_unload(kas.a_hat, vm->uv_va, vm->uv_size, HAT_UNLOAD_UNLOCK);
    vmem_free(heap_arena, vm->uv_va, vm->uv_size);
    kmem_free(vm, sizeof (*vm));
}

int
uvm_page_init(void)
{
    unsigned int c, order;
    int rc;

    if (!kpm_enable)
        return (ENOTSUP);

    rc = vn_make_ops(UVM_ILLUMOS_NAME, uvm_page_vnodeops_template,
        &uvm_page_vnodeops);
    if (rc != 0)
        return (rc);

    for (c = 0; c < 2; c++) {
        for (order = 0; order <= UVM_PAGE_MAX_ORDER; order++) {
            uvm_page_vp[c][order] = vn_alloc(KM_SLEEP);
            vn_setops(uvm_page_vp[c][order], uvm_page_vnodeops);
        }
    }

    /* Page offsets only; the arena maps nothing. */
    uvm_page_off_arena = vmem_create("nvidia_uvm_page_off",
        (void *)PAGESIZE, 1ULL << 46, PAGESIZE, NULL, NULL, NULL, 0,
        VM_SLEEP | VMC_IDENTIFIER);

    mutex_init(&uvm_vmap_lock, NULL, MUTEX_DRIVER, NULL);
    avl_create(&uvm_vmap_tree, uvm_vmap_compare, sizeof (uvm_vmap_t),
        offsetof(uvm_vmap_t, uv_link));
    uvm_dma_init();
    uvm_acct_init();
    uvm_pin_init();

    return (0);
}

void
uvm_page_fini(void)
{
    unsigned int c, order;

    uvm_pin_fini();
    uvm_acct_fini();
    uvm_dma_fini();

    avl_destroy(&uvm_vmap_tree);
    mutex_destroy(&uvm_vmap_lock);

    /* Leaked pages still name the vnodes, so keep those around. */
    if (uvm_page_count != 0) {
        cmn_err(CE_WARN, "nvidia_uvm: %lu pages leaked", uvm_page_count);
        return;
    }

    vmem_destroy(uvm_page_off_arena);
    for (c = 0; c < 2; c++) {
        for (order = 0; order <= UVM_PAGE_MAX_ORDER; order++)
            vn_free(uvm_page_vp[c][order]);
    }
    vn_freevnodeops(uvm_page_vnodeops);
}
