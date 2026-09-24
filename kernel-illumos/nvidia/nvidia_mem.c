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
 * System memory and its kernel mappings.
 *
 * System memory comes from ddi_dma_mem_alloc(), which honours the device's
 * DMA address limit and the requested cache attribute.  The page table keeps
 * CPU physical addresses; device addresses are produced separately by
 * nv_dma_map_alloc(), because with an IOMMU the two differ.
 *
 * Every allocation has one contiguous kernel virtual mapping (at->kva).  User
 * mappings of system memory are devmap umem mappings of that range; user
 * mappings of GPU BARs are devmap devmem mappings of the BAR register sets.
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

NvU32 nv_dma_remap_peer_mmio = NV_DMA_REMAP_PEER_MMIO_ENABLE;

const ddi_device_acc_attr_t nv_sysmem_acc_attr = {
    DDI_DEVICE_ATTR_V0,
    DDI_NEVERSWAP_ACC,
    DDI_STRICTORDER_ACC
};

uint_t
nv_cache_to_iomem(NvU32 cache_type)
{
    switch (cache_type)
    {
        case NV_MEMORY_CACHED:
            return IOMEM_DATA_CACHED;
        case NV_MEMORY_WRITECOMBINED:
            return IOMEM_DATA_UC_WR_COMBINE;
        case NV_MEMORY_UNCACHED:
        case NV_MEMORY_UNCACHED_WEAK:
        case NV_MEMORY_DEFAULT:
        default:
            return IOMEM_DATA_UNCACHED;
    }
}

static uint32_t
nv_cache_to_gfxp(NvU32 cache_type)
{
    switch (cache_type)
    {
        case NV_MEMORY_CACHED:
            return GFXP_MEMORY_CACHED;
        case NV_MEMORY_WRITECOMBINED:
            return GFXP_MEMORY_WRITECOMBINED;
        default:
            return GFXP_MEMORY_UNCACHED;
    }
}

NvBool
nv_cache_type_valid(NvU32 cache_type)
{
    switch (cache_type)
    {
        case NV_MEMORY_CACHED:
        case NV_MEMORY_UNCACHED:
        case NV_MEMORY_UNCACHED_WEAK:
        case NV_MEMORY_WRITECOMBINED:
        case NV_MEMORY_DEFAULT:
            return NV_TRUE;
        default:
            return NV_FALSE;
    }
}

static NvU64
nv_highest_pow2_le(NvU64 v)
{
    NvU64 p = 1;

    while ((p << 1) != 0 && (p << 1) <= v)
        p <<= 1;
    return p;
}

void
nv_dma_attr_init(ddi_dma_attr_t *attr, NvU64 addr_hi, NvU64 align,
    int sgllen)
{
    bzero(attr, sizeof (*attr));
    attr->dma_attr_version = DMA_ATTR_V0;
    attr->dma_attr_addr_lo = 0;
    attr->dma_attr_addr_hi = addr_hi;
    attr->dma_attr_count_max = UINT64_MAX;
    attr->dma_attr_align = align;
    attr->dma_attr_burstsizes = 0xfff;
    attr->dma_attr_minxfer = 1;
    attr->dma_attr_maxxfer = UINT64_MAX;
    attr->dma_attr_seg = UINT64_MAX;
    attr->dma_attr_sgllen = sgllen;
    attr->dma_attr_granular = 1;
    attr->dma_attr_flags = 0;
}

dev_info_t *
nv_dma_dip(nv_state_t *nv)
{
    nv_illumos_state_t *nvis;
    dev_info_t *dip = NULL;

    if (nv != NULL && !(nv->flags & NV_FLAG_CONTROL))
        return NV_GET_NVIS(nv)->dip;

    /* Allocations not tied to a GPU are made through any attached GPU. */
    rw_enter(&nv_illumos_devices_lock, RW_READER);
    nvis = nv_illumos_devices;
    if (nvis != NULL)
        dip = nvis->dip;
    rw_exit(&nv_illumos_devices_lock);

    return dip;
}

static NvU64
nv_dma_limit(nv_state_t *nv)
{
    if (nv == NULL || (nv->flags & NV_FLAG_CONTROL))
        return 0xffffffffffffffffULL;

    if (nv->force_dma32_alloc)
        return 0xffffffffULL;

    return NV_GET_NVIS(nv)->dma_dev.addressable_range.limit;
}

/*
 * Map an arbitrary list of physical pages at a new kernel address with the
 * given cache type.  The attribute must match every other mapping of the
 * pages to avoid conflicting memory types.
 */
caddr_t
nv_kmap_phys(const NvU64 *phys, NvU64 count, NvBool contig, NvU32 cache_type)
{
    uint32_t mode = nv_cache_to_gfxp(cache_type);
    caddr_t va;
    NvU64 i;

    va = gfxp_alloc_kernel_space(mmu_ptob(count));
    if (va == NULL)
        return NULL;

    if (contig)
    {
        gfxp_load_kernel_space(phys[0], mmu_ptob(count), mode, va);
        return va;
    }

    for (i = 0; i < count; i++)
        gfxp_load_kernel_space(phys[i], MMU_PAGESIZE, mode, va + mmu_ptob(i));

    return va;
}

void
nv_kunmap_phys(caddr_t va, NvU64 count)
{
    gfxp_unload_kernel_space(va, mmu_ptob(count));
    gfxp_free_kernel_space(va, mmu_ptob(count));
}

nv_illumos_alloc_t *
nvos_create_alloc(dev_info_t *dip, NvU64 num_pages)
{
    nv_illumos_alloc_t *at;

    if (num_pages == 0 || num_pages > (NvU64)(UINT32_MAX) ||
        num_pages > (SIZE_MAX / sizeof (nvidia_pte_t)))
        return NULL;

    at = kmem_zalloc(sizeof (*at), KM_NOSLEEP);
    if (at == NULL)
        return NULL;

    at->page_table = kmem_zalloc(num_pages * sizeof (nvidia_pte_t),
        KM_NOSLEEP | KM_NORMALPRI);
    if (at->page_table == NULL)
    {
        kmem_free(at, sizeof (*at));
        return NULL;
    }

    at->dip = dip;
    at->num_pages = (NvU32)num_pages;
    at->size = mmu_ptob(num_pages);
    at->refcnt = 1;
    at->pid = curproc->p_pid;
    at->node_id = -1;

    return at;
}

static void
nvos_free_alloc(nv_illumos_alloc_t *at)
{
    kmem_free(at->page_table, at->num_pages * sizeof (nvidia_pte_t));
    kmem_free(at, sizeof (*at));
}

static void
nv_free_dma_chunks(nv_illumos_alloc_t *at)
{
    NvU32 i;

    if (at->kva != NULL && at->num_chunks > 1)
        nv_kunmap_phys(at->kva, at->num_pages);
    at->kva = NULL;

    for (i = 0; i < at->num_chunks; i++)
    {
        nv_dma_chunk_t *c = &at->chunks[i];

        if (c->acc_handle != NULL)
            ddi_dma_mem_free(&c->acc_handle);
        if (c->dma_handle != NULL)
            ddi_dma_free_handle(&c->dma_handle);
    }

    if (at->chunks != NULL)
        kmem_free(at->chunks, at->num_chunks * sizeof (nv_dma_chunk_t));
    at->chunks = NULL;
    at->num_chunks = 0;
}

/*
 * Allocate backing memory in chunks of chunk_size bytes, each physically
 * contiguous and naturally aligned (like Linux page orders), except that a
 * 4K-granular non-contiguous allocation is done in one piece since each page
 * is independently placed anyway.
 */
static NV_STATUS
nv_alloc_dma_chunks(nv_state_t *nv, nv_illumos_alloc_t *at, NvU64 chunk_size)
{
    int (*waitfp)(caddr_t) = nv_may_sleep() ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT;
    uint_t flags = DDI_DMA_CONSISTENT | nv_cache_to_iomem(at->cache_type);
    NvU64 total = at->size;
    NvU64 limit = nv_dma_limit(nv);
    NvU64 page = 0;
    NvU32 nchunks, i;
    NvBool single;

    if (at->dip == NULL)
        return NV_ERR_INVALID_STATE;

    single = at->flags.contig || chunk_size <= MMU_PAGESIZE;
    nchunks = single ? 1 : (NvU32)howmany(total, chunk_size);

    at->chunks = kmem_zalloc(nchunks * sizeof (nv_dma_chunk_t), KM_NOSLEEP);
    if (at->chunks == NULL)
        return NV_ERR_NO_MEMORY;
    at->num_chunks = nchunks;

    for (i = 0; i < nchunks; i++)
    {
        nv_dma_chunk_t *c = &at->chunks[i];
        ddi_dma_attr_t attr;
        NvU64 len, align;
        int sgllen;
        size_t real_len;
        NvU64 off;

        if (single)
        {
            len = total;
            if (at->flags.contig)
            {
                align = MIN(nv_highest_pow2_le(len), (NvU64)MMU_PAGESIZE << 9);
                align = MAX(align, (NvU64)MMU_PAGESIZE);
                sgllen = 1;
            }
            else
            {
                align = MMU_PAGESIZE;
                sgllen = (int)MIN(at->num_pages, (NvU64)INT32_MAX);
            }
        }
        else
        {
            len = MIN(chunk_size, total - mmu_ptob(page));
            align = chunk_size;
            sgllen = 1;
        }

        nv_dma_attr_init(&attr, limit, align, sgllen);

        if (ddi_dma_alloc_handle(at->dip, &attr, waitfp, NULL,
                &c->dma_handle) != DDI_SUCCESS)
            goto fail;

        if (ddi_dma_mem_alloc(c->dma_handle, (size_t)len, &nv_sysmem_acc_attr,
                flags, waitfp, NULL, &c->kva, &real_len,
                &c->acc_handle) != DDI_SUCCESS)
            goto fail;

        c->len = (size_t)len;

        if (at->flags.zeroed)
            bzero(c->kva, c->len);

        for (off = 0; off < len; off += MMU_PAGESIZE, page++)
        {
            at->page_table[page].virt_addr = c->kva + off;
            at->page_table[page].phys_addr = nv_kva_to_phys(c->kva + off);
        }
    }

    if (nchunks == 1)
    {
        at->kva = at->chunks[0].kva;
    }
    else
    {
        NvU64 *phys;
        NvU64 p;

        phys = kmem_alloc(at->num_pages * sizeof (NvU64), KM_NOSLEEP);
        if (phys == NULL)
            goto fail;
        for (p = 0; p < at->num_pages; p++)
            phys[p] = at->page_table[p].phys_addr;
        at->kva = nv_kmap_phys(phys, at->num_pages, NV_FALSE, at->cache_type);
        kmem_free(phys, at->num_pages * sizeof (NvU64));
        if (at->kva == NULL)
            goto fail;
    }

    return NV_OK;

fail:
    nv_free_dma_chunks(at);
    return NV_ERR_NO_MEMORY;
}

static void
nv_alloc_destroy(nv_illumos_alloc_t *at)
{
    if (at->umem_cookie != NULL)
        gfxp_umem_cookie_destroy(at->umem_cookie);

    if (at->num_chunks != 0)
        nv_free_dma_chunks(at);
    else if (at->kva != NULL)
        nv_kunmap_phys(at->kva, at->num_pages);

    nvos_free_alloc(at);
}

void
nv_alloc_hold(nv_illumos_alloc_t *at)
{
    atomic_inc_64(&at->refcnt);
}

void
nv_alloc_rele(nv_illumos_alloc_t *at)
{
    if (atomic_dec_64_nv(&at->refcnt) == 0)
        nv_alloc_destroy(at);
}

NV_STATUS NV_API_CALL nv_alloc_pages(
    nv_state_t *nv,
    NvU32       page_count,
    NvU64       page_size,
    NvBool      contiguous,
    NvU32       cache_type,
    NvBool      zeroed,
    NvBool      unencrypted,
    NvS32       node_id,
    NvU64      *pte_array,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at;
    NV_STATUS status;
    NvU32 i;

    nv_printf(NV_DBG_MEMINFO, "NVRM: VM: nv_alloc_pages: %d pages, nodeid %d\n",
        page_count, node_id);

    if (!nv_cache_type_valid(cache_type))
        return NV_ERR_NOT_SUPPORTED;

    /* Memory encryption is only meaningful on confidential-compute guests. */
    if (unencrypted)
        return NV_ERR_NOT_SUPPORTED;

    if (!contiguous && (page_size == 0 || (page_size & (page_size - 1)) != 0))
        return NV_ERR_INVALID_ARGUMENT;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    at->cache_type = cache_type;
    at->flags.contig = contiguous;
    at->flags.zeroed = zeroed;
    if (node_id >= 0)
    {
        at->flags.node = NV_TRUE;
        at->node_id = node_id;
    }

    status = nv_alloc_dma_chunks(nv, at, contiguous ? at->size : page_size);
    if (status != NV_OK)
    {
        nvos_free_alloc(at);
        return status;
    }

    for (i = 0; i < (contiguous ? 1 : page_count); i++)
        pte_array[i] = at->page_table[i].phys_addr;

    *priv_data = at;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_free_pages(
    nv_state_t *nv,
    NvU32 page_count,
    NvBool contiguous,
    NvU32 cache_type,
    void *priv_data
)
{
    nv_illumos_alloc_t *at = priv_data;

    nv_printf(NV_DBG_MEMINFO, "NVRM: VM: nv_free_pages: 0x%x\n", page_count);

    /* User mappings may still reference the pages; the last one frees them. */
    nv_alloc_rele(at);
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_alias_pages(
    nv_state_t *nv,
    NvU32       page_cnt,
    NvU64       page_size,
    NvU32       contiguous,
    NvU32       cache_type,
    NvU64       guest_id,
    NvU64      *pte_array,
    NvBool      carveout,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at;
    NvU32 i;

    if (!nv_cache_type_valid(cache_type))
        return NV_ERR_NOT_SUPPORTED;

    at = nvos_create_alloc(nv_dma_dip(nv), page_cnt);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    at->cache_type = cache_type;
    at->flags.contig = contiguous;
    at->flags.physical = NV_TRUE;

    for (i = 0; i < at->num_pages; i++)
    {
        at->page_table[i].phys_addr = contiguous ?
            pte_array[0] + mmu_ptob(i) : pte_array[i];
    }

    *priv_data = at;
    return NV_OK;
}

/*
 * Returns the contiguous kernel mapping of an allocation, creating it on
 * first use for allocations that only carry physical addresses.
 */
caddr_t
nv_alloc_kva(nv_illumos_alloc_t *at)
{
    if (at->kva == NULL)
    {
        NvU64 *phys;
        NvU32 i;
        caddr_t va;

        phys = kmem_alloc(at->num_pages * sizeof (NvU64), KM_NOSLEEP);
        if (phys == NULL)
            return NULL;
        for (i = 0; i < at->num_pages; i++)
            phys[i] = at->page_table[i].phys_addr;

        va = nv_kmap_phys(phys, at->num_pages, NV_FALSE, at->cache_type);
        kmem_free(phys, at->num_pages * sizeof (NvU64));
        if (va == NULL)
            return NULL;

        if (atomic_cas_ptr(&at->kva, NULL, va) != NULL)
            nv_kunmap_phys(va, at->num_pages);
    }

    return at->kva;
}

void* NV_API_CALL nv_alloc_kernel_mapping(
    nv_state_t *nv,
    void       *pAllocPrivate,
    NvU64       pageIndex,
    NvU32       pageOffset,
    NvU64       size,
    void      **pPrivate
)
{
    nv_illumos_alloc_t *at = pAllocPrivate;
    caddr_t kva;

    *pPrivate = NULL;

    if (pageIndex >= at->num_pages ||
        mmu_ptob(pageIndex) + pageOffset + size > at->size)
        return NULL;

    kva = nv_alloc_kva(at);
    if (kva == NULL)
        return NULL;

    return kva + mmu_ptob(pageIndex) + pageOffset;
}

void NV_API_CALL nv_free_kernel_mapping(
    nv_state_t *nv,
    void       *pAllocPrivate,
    void       *address,
    void       *pPrivate
)
{
    /* The allocation-wide mapping is torn down with the allocation. */
}

NV_STATUS NV_API_CALL nv_alloc_user_mapping(
    nv_state_t *nv,
    void       *pAllocPrivate,
    NvU64       pageIndex,
    NvU32       pageOffset,
    NvU64       size,
    NvU32       protect,
    NvU64      *pUserAddress,
    void      **ppPrivate
)
{
    nv_illumos_alloc_t *at = pAllocPrivate;

    if (at->flags.contig)
        *pUserAddress = at->page_table[0].phys_addr + mmu_ptob(pageIndex) + pageOffset;
    else
        *pUserAddress = at->page_table[pageIndex].phys_addr + pageOffset;

    return NV_OK;
}

void NV_API_CALL nv_free_user_mapping(
    nv_state_t *nv,
    void       *pAllocPrivate,
    NvU64       userAddress,
    void       *pPrivate
)
{
}

NV_STATUS NV_API_CALL os_match_mmap_offset(
    void  *pAllocPrivate,
    NvU64  offset,
    NvU64 *pPageIndex
)
{
    nv_illumos_alloc_t *at = pAllocPrivate;
    NvU64 i;

    for (i = 0; i < at->num_pages; i++)
    {
        NvU64 pa = at->flags.contig ?
            at->page_table[0].phys_addr + mmu_ptob(i) :
            at->page_table[i].phys_addr;

        if (offset == pa)
        {
            *pPageIndex = i;
            return NV_OK;
        }
    }

    return NV_ERR_OBJECT_NOT_FOUND;
}

/*
 * Records what the next mmap() of fd maps.  NVIDIA userspace opens a new fd
 * for every mapping, so only one context per open instance is accepted.
 */
NV_STATUS NV_API_CALL nv_add_mapping_context_to_file(
    nv_state_t *nv,
    nv_usermap_access_params_t *nvuap,
    NvU32       prot,
    void       *pAllocPriv,
    NvU64       pageIndex,
    NvU32       fd
)
{
    nv_illumos_file_private_t *nvifp;
    nv_alloc_mapping_list_node_t **plist;
    nv_alloc_mapping_list_node_t *node;
    nv_alloc_mapping_context_t *nvamc;
    nv_file_private_t *nvfp;
    NV_STATUS status = NV_OK;
    void *priv = NULL;

    nvfp = nv_get_file_private(fd, NV_IS_CTL_DEVICE(nv), &priv);
    if (nvfp == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    nvifp = nv_get_nvifp_from_nvfp(nvfp);

    node = kmem_zalloc(sizeof (*node), KM_SLEEP);
    nvamc = &node->context;

    if (NV_IS_CTL_DEVICE(nv))
    {
        nvamc->alloc = pAllocPriv;
        nvamc->page_index = pageIndex;
    }
    else
    {
        if (nvifp->nvis == NULL || NV_STATE_PTR(nvifp->nvis) != nv ||
            nvuap->memArea.numRanges == 0 ||
            nvuap->memArea.numRanges > (SIZE_MAX / sizeof (MemoryRange)))
        {
            status = NV_ERR_INVALID_ARGUMENT;
            goto fail;
        }

        status = os_alloc_mem((void **)&nvamc->memArea.pRanges,
            sizeof (MemoryRange) * nvuap->memArea.numRanges);
        if (status != NV_OK)
            goto fail;

        nvamc->memArea.numRanges = nvuap->memArea.numRanges;
        bcopy(nvuap->memArea.pRanges, nvamc->memArea.pRanges,
            sizeof (MemoryRange) * nvuap->memArea.numRanges);
        nvamc->access_start = nvuap->access_start;
        nvamc->access_size = nvuap->access_size;
    }

    nvamc->prot = prot;
    nvamc->caching = nvuap->caching;
    nvamc->valid = NV_TRUE;

    plist = nv_acquire_file_va(nvfp, NV_TRUE);
    if (*plist == NULL)
    {
        *plist = node;
        if (NV_IS_CTL_DEVICE(nv))
            nv_alloc_hold(nvamc->alloc);
        node = NULL;
    }
    else
    {
        status = NV_ERR_STATE_IN_USE;
    }
    nv_release_file_va(nvfp, NV_TRUE);

    if (node == NULL)
    {
        nv_put_file_private(priv);
        return NV_OK;
    }

fail:
    if (nvamc->memArea.pRanges != NULL)
        os_free_mem(nvamc->memArea.pRanges);
    kmem_free(node, sizeof (*node));
    nv_put_file_private(priv);
    return status;
}

