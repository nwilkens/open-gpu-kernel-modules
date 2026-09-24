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
 * Pinned user memory and externally registered pages.
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
 * User memory pinning.  umem_lockmemory(DDI_UMEMLOCK_LONGTERM) keeps the
 * pages locked beyond the ioctl and enforces locked-memory resource
 * controls.  Physical addresses come from the pagelock's shadow list, or from
 * the process HAT when the segment softlocked the range instead.
 */
typedef struct nv_user_pages_s {
    ddi_umem_cookie_t   cookie;
    caddr_t             addr;
    size_t              len;
    NvU64               page_count;
    NvU64              *phys;
} nv_user_pages_t;

/*
 * munmap(2) or exit of a range RM still has pinned.  The pin cannot be
 * dropped here: the GPU may still DMA to these pages, and RM has no way to
 * revoke that.  The unmap waits until RM unpins, when the memory object is
 * freed.
 */
static void
nv_umem_lock_cleanup(ddi_umem_cookie_t *cookie)
{
}

static struct umem_callback_ops nv_umem_callbacks = {
    .cbo_umem_callback_version = UMEM_CALLBACK_VERSION,
    .cbo_umem_lock_cleanup = nv_umem_lock_cleanup,
};

NV_STATUS NV_API_CALL os_lock_user_pages(
    void   *address,
    NvU64   page_count,
    void  **page_array,
    NvU32   flags
)
{
    nv_user_pages_t *up;
    caddr_t addr = (caddr_t)((uintptr_t)address & MMU_PAGEMASK);
    int lflags = DDI_UMEMLOCK_READ | DDI_UMEMLOCK_LONGTERM;
    page_t **pparray;
    NvU64 i;
    int err;

    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s(): invalid context!\n", __func__);
        return NV_ERR_NOT_SUPPORTED;
    }

    if (page_count == 0 || page_count > (SIZE_MAX >> MMU_PAGESHIFT) ||
        (uintptr_t)addr + mmu_ptob(page_count) < (uintptr_t)addr)
        return NV_ERR_INVALID_ARGUMENT;

    if (DRF_VAL(_LOCK_USER_PAGES, _FLAGS, _WRITE, flags))
        lflags |= DDI_UMEMLOCK_WRITE;

    up = kmem_zalloc(sizeof (*up), KM_SLEEP);
    up->addr = addr;
    up->len = mmu_ptob(page_count);
    up->page_count = page_count;
    up->phys = kmem_alloc(page_count * sizeof (NvU64), KM_NOSLEEP | KM_NORMALPRI);
    if (up->phys == NULL)
    {
        kmem_free(up, sizeof (*up));
        return NV_ERR_NO_MEMORY;
    }

    err = umem_lockmemory(addr, up->len, lflags, &up->cookie,
        &nv_umem_callbacks, curproc);
    if (err != 0)
    {
        kmem_free(up->phys, page_count * sizeof (NvU64));
        kmem_free(up, sizeof (*up));
        return NV_ERR_INVALID_ADDRESS;
    }

    pparray = ((struct ddi_umem_cookie *)up->cookie)->pparray;

    for (i = 0; i < page_count; i++)
    {
        pfn_t pfn = (pparray != NULL) ? page_pptonum(pparray[i]) :
            hat_getpfnum(curproc->p_as->a_hat, addr + mmu_ptob(i));

        if (pfn == PFN_INVALID)
        {
            ddi_umem_unlock(up->cookie);
            kmem_free(up->phys, page_count * sizeof (NvU64));
            kmem_free(up, sizeof (*up));
            return NV_ERR_INVALID_ADDRESS;
        }
        up->phys[i] = mmu_ptob((NvU64)pfn);
    }

    *page_array = up;
    return NV_OK;
}

NV_STATUS NV_API_CALL os_unlock_user_pages(
    NvU64  page_count,
    void  *page_array,
    NvU32  flags
)
{
    nv_user_pages_t *up = page_array;

    if (up == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    ddi_umem_unlock(up->cookie);
    kmem_free(up->phys, up->page_count * sizeof (NvU64));
    kmem_free(up, sizeof (*up));
    return NV_OK;
}

/*
 * Resolve a user range that maps device memory (another driver's mmap) to
 * physical addresses.  The range must lie in one device segment and be
 * physically contiguous, as on Linux.
 */
NV_STATUS NV_API_CALL os_lookup_user_io_memory(
    void   *address,
    NvU64   page_count,
    NvU64 **pte_array
)
{
    struct as *as = curproc->p_as;
    caddr_t start = (caddr_t)address;
    extern struct seg_ops segdev_ops;
    struct seg *seg;
    NvU64 *ptes;
    NV_STATUS status = NV_OK;
    NvU64 i;

    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s(): invalid context!\n", __func__);
        return NV_ERR_NOT_SUPPORTED;
    }

    if (page_count == 0 || page_count > (SIZE_MAX >> MMU_PAGESHIFT) ||
        ((uintptr_t)start & MMU_PAGEOFFSET) != 0)
        return NV_ERR_INVALID_ADDRESS;

    if (os_alloc_mem((void **)&ptes, page_count * sizeof (NvU64)) != NV_OK)
        return NV_ERR_NO_MEMORY;

    AS_LOCK_ENTER(as, RW_READER);

    seg = as_segat(as, start);
    if (seg == NULL || seg->s_ops != &segdev_ops ||
        start + mmu_ptob(page_count) > seg->s_base + seg->s_size ||
        start + mmu_ptob(page_count) < start)
    {
        nv_printf(NV_DBG_ERRORS,
            "Cannot map memory with base addr 0x%p and size of 0x%llx pages\n",
            (void *)start, page_count);
        status = NV_ERR_INVALID_ADDRESS;
        goto done;
    }

    for (i = 0; i < page_count; i++)
    {
        pfn_t pfn = hat_getpfnum(as->a_hat, start + mmu_ptob(i));

        if (pfn == PFN_INVALID || pf_is_memory(pfn))
        {
            status = NV_ERR_INVALID_ADDRESS;
            goto done;
        }

        ptes[i] = mmu_ptob((NvU64)pfn);
        if (i > 0 && ptes[i] != ptes[i - 1] + MMU_PAGESIZE)
        {
            status = NV_ERR_INVALID_ADDRESS;
            goto done;
        }
    }

done:
    AS_LOCK_EXIT(as);

    if (status != NV_OK)
        os_free_mem(ptes);
    else
        *pte_array = ptes;

    return status;
}

NV_STATUS NV_API_CALL nv_register_user_pages(
    nv_state_t *nv,
    NvU64       page_count,
    NvU64      *phys_addr,
    void       *import_priv,
    void      **priv_data,
    NvBool      unencrypted
)
{
    nv_user_pages_t *up = *priv_data;
    nv_illumos_alloc_t *at;
    NvU64 i;

    if (up == NULL || page_count > up->page_count)
        return NV_ERR_INVALID_ARGUMENT;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    /* Anonymous memory must be write-back cacheable. */
    at->cache_type = NV_MEMORY_CACHED;
    at->flags.user = NV_TRUE;

    for (i = 0; i < page_count; i++)
        at->page_table[i].phys_addr = phys_addr[i] = up->phys[i];

    at->user_pages = up;
    at->import_priv = import_priv;
    *priv_data = at;

    return NV_OK;
}

void NV_API_CALL nv_unregister_user_pages(
    nv_state_t *nv,
    NvU64       page_count,
    void      **import_priv,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at = *priv_data;

    *priv_data = at->user_pages;
    if (import_priv != NULL)
        *import_priv = at->import_priv;

    at->user_pages = NULL;
    nv_alloc_rele(at);
}

NV_STATUS NV_API_CALL nv_register_peer_io_mem(
    nv_state_t *nv,
    NvU64      *phys_addr,
    NvU64       page_count,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at;
    NvU64 i;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    at->cache_type = NV_MEMORY_UNCACHED;
    at->flags.contig = NV_TRUE;
    at->flags.peer_io = NV_TRUE;

    for (i = 0; i < page_count; i++)
        at->page_table[i].phys_addr = phys_addr[0] + mmu_ptob(i);

    *priv_data = at;
    return NV_OK;
}

void NV_API_CALL nv_unregister_peer_io_mem(nv_state_t *nv, void *priv_data)
{
    nv_alloc_rele(priv_data);
}

NV_STATUS NV_API_CALL nv_register_phys_pages(
    nv_state_t *nv,
    NvU64      *phys_addr,
    NvU64       page_count,
    NvU32       cache_type,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at;
    NvU64 i;

    if (!nv_cache_type_valid(cache_type))
        return NV_ERR_NOT_SUPPORTED;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    at->cache_type = cache_type;
    at->flags.physical = NV_TRUE;

    for (i = 0; i < page_count; i++)
        at->page_table[i].phys_addr = phys_addr[i];

    *priv_data = at;
    return NV_OK;
}

void NV_API_CALL nv_unregister_phys_pages(nv_state_t *nv, void *priv_data)
{
    nv_alloc_rele(priv_data);
}

/* Scatter-gather tables only come from dma-buf imports, which illumos lacks. */
NV_STATUS NV_API_CALL nv_register_sgt(
    nv_state_t *nv,
    NvU64      *phys_addr,
    NvU64       page_count,
    NvU32       cache_type,
    void      **priv_data,
    struct sg_table *import_sgt,
    void       *import_priv,
    NvBool      is_peer_mmio
)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_unregister_sgt(
    nv_state_t *nv,
    struct sg_table **import_sgt,
    void **import_priv,
    void *priv_data
)
{
}

NV_STATUS NV_API_CALL nv_get_num_phys_pages(void *pAllocPrivate, NvU32 *pNumPages)
{
    nv_illumos_alloc_t *at = pAllocPrivate;

    if (pNumPages == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    *pNumPages = at->num_pages;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_get_phys_pages(void *pAllocPrivate, void *pPages,
    NvU32 *pNumPages)
{
    nv_illumos_alloc_t *at = pAllocPrivate;
    page_t **pages = pPages;
    NvU32 i, count;

    if (pNumPages == NULL || pPages == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    count = MIN(*pNumPages, at->num_pages);
    for (i = 0; i < count; i++)
        pages[i] = page_numtopp_nolock(mmu_btop(at->page_table[i].phys_addr));

    *pNumPages = count;
    return NV_OK;
}
