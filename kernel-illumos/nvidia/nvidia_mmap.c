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
 * User mappings of system memory and GPU BARs through devmap(9E).
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
 * devmap callbacks.  Each user mapping gets an nv_devmap_priv_t on the open
 * instance's list; dup and partial unmap create more.  Sysmem mappings hold
 * a reference on the allocation so an early nv_free_pages() cannot free
 * pages that are still mapped.
 */
static nv_devmap_priv_t *
nv_devmap_priv_create(nv_illumos_file_private_t *nvifp, devmap_cookie_t dhp,
    nv_illumos_alloc_t *at, offset_t off, size_t len, NvBool device_memory)
{
    nv_devmap_priv_t *dp;

    dp = kmem_zalloc(sizeof (*dp), KM_SLEEP);
    dp->nvifp = nvifp;
    dp->dhp = dhp;
    dp->at = at;
    dp->off = off;
    dp->len = len;
    dp->device_memory = device_memory;

    if (at != NULL)
        nv_alloc_hold(at);

    if (nvifp->nvis != NULL)
        sema_p(&nvifp->nvis->mmap_lock);
    list_insert_tail(&nvifp->mappings, dp);
    if (nvifp->nvis != NULL)
        sema_v(&nvifp->nvis->mmap_lock);

    return dp;
}

static void
nv_devmap_priv_destroy(nv_devmap_priv_t *dp)
{
    nv_illumos_file_private_t *nvifp = dp->nvifp;

    if (nvifp->nvis != NULL)
        sema_p(&nvifp->nvis->mmap_lock);
    list_remove(&nvifp->mappings, dp);
    if (nvifp->nvis != NULL)
        sema_v(&nvifp->nvis->mmap_lock);

    if (dp->at != NULL)
        nv_alloc_rele(dp->at);

    kmem_free(dp, sizeof (*dp));
}

typedef struct nv_devmap_setup_s {
    nv_illumos_file_private_t  *nvifp;
    nv_illumos_alloc_t         *at;
    NvBool                      device_memory;
} nv_devmap_setup_t;

static int
nv_devmap_map(devmap_cookie_t dhp, dev_t dev, uint_t flags, offset_t off,
    size_t len, void **pvtp)
{
    nv_illumos_file_private_t *nvifp;
    nv_alloc_mapping_list_node_t *node;
    nv_illumos_alloc_t *at = NULL;
    NvBool device_memory;

    nvifp = nv_clone_lookup(getminor(dev));
    if (nvifp == NULL || nvifp->nvis == NULL)
        return (ENXIO);

    device_memory = !NV_IS_CTL_DEVICE(NV_STATE_PTR(nvifp->nvis));

    if (!device_memory)
    {
        rw_enter(&nvifp->file_va_lock, RW_READER);
        node = nvifp->file_mapping_list;
        at = (node != NULL) ? node->context.alloc : NULL;
        rw_exit(&nvifp->file_va_lock);
        if (at == NULL)
            return (EINVAL);
    }

    *pvtp = nv_devmap_priv_create(nvifp, dhp, at, off, len, device_memory);
    return (0);
}

static int
nv_devmap_access(devmap_cookie_t dhp, void *pvtp, offset_t off, size_t len,
    uint_t type, uint_t rw)
{
    nv_devmap_priv_t *dp = pvtp;
    nv_illumos_state_t *nvis;
    nv_state_t *nv;
    int rc;

    if (!dp->device_memory)
        return devmap_default_access(dhp, pvtp, off, len, type, rw);

    nvis = dp->nvifp->nvis;
    nv = NV_STATE_PTR(nvis);

    sema_p(&nvis->mmap_lock);

    if (!nvis->safe_to_mmap)
    {
        /*
         * The GPU is powered down.  The wakeup cannot run here because it
         * needs the GPU lock; schedule it once and let the access fault
         * again, as nvidia_fault() does on Linux.
         */
        if (nvis->gpu_wakeup_callback_needed)
        {
            if (rm_schedule_gpu_wakeup(nvis->sp[NV_DEV_STACK_GPU_WAKEUP], nv) != NV_OK)
            {
                sema_v(&nvis->mmap_lock);
                return (EFAULT);
            }
            nvis->gpu_wakeup_callback_needed = NV_FALSE;
        }
        sema_v(&nvis->mmap_lock);
        delay(1);
        return (0);
    }

    rc = devmap_default_access(dhp, pvtp, off, len, type, rw);
    if (rc == 0)
        nvis->all_mappings_revoked = NV_FALSE;

    sema_v(&nvis->mmap_lock);
    return (rc);
}

static int
nv_devmap_dup(devmap_cookie_t dhp, void *pvtp, devmap_cookie_t new_dhp,
    void **new_pvtp)
{
    nv_devmap_priv_t *dp = pvtp;

    *new_pvtp = nv_devmap_priv_create(dp->nvifp, new_dhp, dp->at, dp->off,
        dp->len, dp->device_memory);
    return (0);
}

static void
nv_devmap_unmap(devmap_cookie_t dhp, void *pvtp, offset_t off, size_t len,
    devmap_cookie_t new_dhp1, void **new_pvtp1, devmap_cookie_t new_dhp2,
    void **new_pvtp2)
{
    nv_devmap_priv_t *dp = pvtp;

    /* Partial unmaps leave up to two remaining pieces. */
    if (new_dhp1 != NULL && new_pvtp1 != NULL)
    {
        *new_pvtp1 = nv_devmap_priv_create(dp->nvifp, new_dhp1, dp->at,
            dp->off, (size_t)(off - dp->off), dp->device_memory);
    }
    if (new_dhp2 != NULL && new_pvtp2 != NULL)
    {
        *new_pvtp2 = nv_devmap_priv_create(dp->nvifp, new_dhp2, dp->at,
            off + len, (size_t)(dp->off + dp->len - (off + len)),
            dp->device_memory);
    }

    nv_devmap_priv_destroy(dp);
}

struct devmap_callback_ctl nv_devmap_callbacks = {
    .devmap_rev     = DEVMAP_OPS_REV,
    .devmap_map     = nv_devmap_map,
    .devmap_access  = nv_devmap_access,
    .devmap_dup     = nv_devmap_dup,
    .devmap_unmap   = nv_devmap_unmap,
};

static uint_t
nv_prot_to_maxprot(NvU32 prot)
{
    uint_t maxprot = PROT_READ | PROT_USER;

    if (prot & NV_PROTECT_WRITEABLE)
        maxprot |= PROT_WRITE;

    return maxprot;
}

/* Control device: map system memory at mmap context page_index. */
int
nv_devmap_sysmem(nv_illumos_file_private_t *nvifp, devmap_cookie_t dhp,
    nv_alloc_mapping_context_t *mmap_context, offset_t off, size_t len,
    size_t *maplen)
{
    nv_illumos_alloc_t *at = mmap_context->alloc;
    offset_t aoff;
    caddr_t kva;
    uint_t flags;
    int rc;

    if (at == NULL || at->flags.user)
        return (EINVAL);

    /* RM validates mappings at offset 0 of the fd only. */
    if (off != 0)
        return (EINVAL);

    if (mmap_context->page_index >= at->num_pages ||
        len > mmu_ptob(at->num_pages - mmap_context->page_index))
        return (ERANGE);

    aoff = (offset_t)mmu_ptob(mmap_context->page_index);

    if (at->flags.peer_io)
    {
        ddi_device_acc_attr_t acc = nv_sysmem_acc_attr;

        /*
         * Peer I/O memory belongs to another device and has no register set
         * here, so map it by physical address.  Protection is enforced by
         * nvidia_segmap().
         */
        acc.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
        gfxp_map_devmem(dhp, at->page_table[0].phys_addr + aoff, len, &acc);
        *maplen = len;
        return (0);
    }

    kva = nv_alloc_kva(at);
    if (kva == NULL)
        return (ENOMEM);

    if (at->umem_cookie == NULL)
    {
        ddi_umem_cookie_t cookie = gfxp_umem_cookie_init(kva, at->size);

        if (atomic_cas_ptr(&at->umem_cookie, NULL, cookie) != NULL)
            gfxp_umem_cookie_destroy(cookie);
    }

    flags = nv_cache_to_iomem(at->cache_type);

    rc = devmap_umem_setup(dhp, at->dip, &nv_devmap_callbacks, at->umem_cookie,
        aoff, len, nv_prot_to_maxprot(mmap_context->prot), flags, NULL);
    if (rc != 0)
        return (rc);

    *maplen = len;
    return (0);
}

/* Find the "reg" register set that describes the BAR at config offset. */
static int
nv_bar_rnumber(dev_info_t *dip, NvU32 bar_offset, uint_t *rnumber)
{
    pci_regspec_t *regs;
    uint_t nelem, n, i;

    if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
            "reg", (int **)&regs, &nelem) != DDI_PROP_SUCCESS)
        return (DDI_FAILURE);

    n = nelem / (sizeof (pci_regspec_t) / sizeof (int));
    for (i = 0; i < n; i++)
    {
        if (PCI_REG_REG_G(regs[i].pci_phys_hi) == bar_offset)
        {
            *rnumber = i;
            ddi_prop_free(regs);
            return (DDI_SUCCESS);
        }
    }

    ddi_prop_free(regs);
    return (DDI_FAILURE);
}

/*
 * GPU device node: map the BAR ranges of the mmap context.  devmap(9E) is
 * called repeatedly with the offset advanced by *maplen, which maps one
 * physical range per call.
 */
int
nv_devmap_devmem(nv_illumos_file_private_t *nvifp, devmap_cookie_t dhp,
    nv_alloc_mapping_context_t *mmap_context, offset_t off, size_t len,
    size_t *maplen)
{
    nv_illumos_state_t *nvis = nvifp->nvis;
    nv_state_t *nv = NV_STATE_PTR(nvis);
    ddi_device_acc_attr_t acc = nv_sysmem_acc_attr;
    NvU64 access_start = mmap_context->access_start;
    NvU64 access_len = mmap_context->access_size;
    NvU64 cur = 0, start = 0, size = 0;
    uint_t rnumber;
    NvU32 i, bar;
    int rc;

    if (off < 0 || (NvU64)off + len < (NvU64)off ||
        (NvU64)off + len > memareaSize(mmap_context->memArea))
        return (ENXIO);

    for (i = 0; i < mmap_context->memArea.numRanges; i++)
    {
        MemoryRange *r = &mmap_context->memArea.pRanges[i];

        if ((NvU64)off >= cur && (NvU64)off < cur + r->size)
        {
            start = r->start + ((NvU64)off - cur);
            size = MIN(r->size - ((NvU64)off - cur), (NvU64)len);
            break;
        }
        cur += r->size;
    }

    if (size == 0 || (start & MMU_PAGEOFFSET) != 0 || (size & MMU_PAGEOFFSET) != 0)
        return (ENXIO);

    if (IS_REG_OFFSET(nv, access_start, access_len))
    {
        acc.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
    }
    else if (IS_FB_OFFSET(nv, access_start, access_len))
    {
        if (IS_UD_OFFSET(nv, access_start, access_len) ||
            rm_disable_iomap_wc() ||
            mmap_context->caching != NV_MEMORY_WRITECOMBINED)
            acc.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
        else
            acc.devacc_attr_dataorder = DDI_MERGING_OK_ACC;
    }
    else
    {
        acc.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
    }

    /* Only ranges inside one of this GPU's BARs may be mapped. */
    for (bar = 0; bar < NV_GPU_NUM_BARS; bar++)
    {
        nv_aperture_t *ap = &nv->bars[bar];

        if (ap->size != 0 && start >= ap->cpu_address &&
            start + size <= ap->cpu_address + ap->size)
            break;
    }
    if (bar == NV_GPU_NUM_BARS)
        return (ENXIO);

    if (nv_bar_rnumber(nvis->dip, nv->bars[bar].offset, &rnumber) != DDI_SUCCESS)
        return (ENXIO);

    sema_p(&nvis->mmap_lock);
    if (!nvis->safe_to_mmap)
    {
        sema_v(&nvis->mmap_lock);
        return (EAGAIN);
    }
    nvis->all_mappings_revoked = NV_FALSE;
    sema_v(&nvis->mmap_lock);

    rc = devmap_devmem_setup(dhp, nvis->dip, &nv_devmap_callbacks, rnumber,
        (offset_t)(start - nv->bars[bar].cpu_address), (size_t)size,
        nv_prot_to_maxprot(mmap_context->prot), 0, &acc);
    if (rc != 0)
        return (rc);

    *maplen = (size_t)size;
    return (0);
}

/*
 * Revoke CPU mappings of GPU memory, e.g. before the GPU is powered down.
 * The next access faults into nv_devmap_access(), which reinstates the
 * translations only once the GPU is safe to map again.
 */
void
nv_revoke_mappings_locked(nv_illumos_state_t *nvis)
{
    nv_illumos_file_private_t *nvifp;
    nv_devmap_priv_t *dp;

    for (nvifp = list_head(&nvis->open_files); nvifp != NULL;
         nvifp = list_next(&nvis->open_files, nvifp))
    {
        for (dp = list_head(&nvifp->mappings); dp != NULL;
             dp = list_next(&nvifp->mappings, dp))
        {
            if (dp->device_memory)
                (void) devmap_unload(dp->dhp, dp->off, dp->len);
        }
    }

    nvis->all_mappings_revoked = NV_TRUE;
}

NV_STATUS NV_API_CALL nv_revoke_gpu_mappings(nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    if (NV_IS_CTL_DEVICE(nv))
        return NV_ERR_NOT_SUPPORTED;

    sema_p(&nvis->mmap_lock);
    nv_revoke_mappings_locked(nvis);
    sema_v(&nvis->mmap_lock);

    return NV_OK;
}

void NV_API_CALL nv_acquire_mmap_lock(nv_state_t *nv)
{
    sema_p(&NV_GET_NVIS(nv)->mmap_lock);
}

void NV_API_CALL nv_release_mmap_lock(nv_state_t *nv)
{
    sema_v(&NV_GET_NVIS(nv)->mmap_lock);
}

NvBool NV_API_CALL nv_get_all_mappings_revoked_locked(nv_state_t *nv)
{
    return NV_GET_NVIS(nv)->all_mappings_revoked;
}

void NV_API_CALL nv_set_safe_to_mmap_locked(nv_state_t *nv, NvBool safe_to_mmap)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    if (!safe_to_mmap && nvis->safe_to_mmap)
        nvis->gpu_wakeup_callback_needed = NV_TRUE;

    nvis->safe_to_mmap = safe_to_mmap;
}
