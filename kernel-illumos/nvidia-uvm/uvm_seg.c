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
 * seg_nvuvm, the segment driver that backs UVM CPU mappings, and the Linux
 * mm interfaces built on it: vm_insert_page(), unmap_mapping_range(),
 * find_vma() and the mmap lock.
 *
 * Each segment owns a Linux VMA and replays the Linux vm_ops open, close
 * and fault calls on it, so kernel-open/nvidia-uvm/uvm.c runs unchanged.
 * Translations are loaded with HAT_LOAD_LOCK so the HAT never steals one
 * that UVM believes is present, and every unload passes HAT_UNLOAD_UNLOCK.
 *
 * Lock order: AS lock, then UVM locks, then am_lock of the file's
 * address_space, then HAT locks.  Nothing holding am_lock takes an AS lock
 * or a UVM lock.
 */

#include "uvm_illumos.h"
#include "uvm_seg.h"

#include <sys/vmsystm.h>
#include <sys/model.h>
#include <vm/as.h>
#include <vm/seg.h>
#include <vm/hat.h>
#include <vm/faultcode.h>

static int uvm_seg_dup(struct seg *, struct seg *);
static int uvm_seg_unmap(struct seg *, caddr_t, size_t);
static void uvm_seg_free(struct seg *);
static faultcode_t uvm_seg_fault(struct hat *, struct seg *, caddr_t, size_t,
    enum fault_type, enum seg_rw);
static faultcode_t uvm_seg_faulta(struct seg *, caddr_t);
static int uvm_seg_setprot(struct seg *, caddr_t, size_t, uint_t);
static int uvm_seg_checkprot(struct seg *, caddr_t, size_t, uint_t);
static int uvm_seg_sync(struct seg *, caddr_t, size_t, int, uint_t);
static size_t uvm_seg_incore(struct seg *, caddr_t, size_t, char *);
static int uvm_seg_lockop(struct seg *, caddr_t, size_t, int, int, ulong_t *,
    size_t);
static int uvm_seg_getprot(struct seg *, caddr_t, size_t, uint_t *);
static u_offset_t uvm_seg_getoffset(struct seg *, caddr_t);
static int uvm_seg_gettype(struct seg *, caddr_t);
static int uvm_seg_getvp(struct seg *, caddr_t, struct vnode **);
static int uvm_seg_advise(struct seg *, caddr_t, size_t, uint_t);
static void uvm_seg_dump(struct seg *);
static int uvm_seg_pagelock(struct seg *, caddr_t, size_t, struct page ***,
    enum lock_type, enum seg_rw);
static int uvm_seg_setpagesize(struct seg *, caddr_t, size_t, uint_t);
static int uvm_seg_getmemid(struct seg *, caddr_t, memid_t *);
static int uvm_seg_capable(struct seg *, segcapability_t);

static struct seg_ops uvm_seg_ops = {
    .dup            = uvm_seg_dup,
    .unmap          = uvm_seg_unmap,
    .free           = uvm_seg_free,
    .fault          = uvm_seg_fault,
    .faulta         = uvm_seg_faulta,
    .setprot        = uvm_seg_setprot,
    .checkprot      = uvm_seg_checkprot,
    .kluster        = NULL,
    .swapout        = NULL,
    .sync           = uvm_seg_sync,
    .incore         = uvm_seg_incore,
    .lockop         = uvm_seg_lockop,
    .getprot        = uvm_seg_getprot,
    .getoffset      = uvm_seg_getoffset,
    .gettype        = uvm_seg_gettype,
    .getvp          = uvm_seg_getvp,
    .advise         = uvm_seg_advise,
    .dump           = uvm_seg_dump,
    .pagelock       = uvm_seg_pagelock,
    .setpagesize    = uvm_seg_setpagesize,
    .getmemid       = uvm_seg_getmemid,
    .getpolicy      = NULL,
    .capable        = uvm_seg_capable,
    .inherit        = seg_inherit_notsup,
};

/* Segments call into this module, so _fini refuses while any exist. */
static volatile uint_t uvm_seg_live;

typedef struct uvm_seg_crargs {
    struct linux_file  *file;
    uchar_t             prot;
    uchar_t             maxprot;
} uvm_seg_crargs_t;

uint_t
uvm_seg_count(void)
{
    return (uvm_seg_live);
}

static uvm_seg_data_t *
uvm_seg_data_alloc(struct seg *seg, uchar_t prot, uchar_t maxprot)
{
    uvm_seg_data_t *sd = kmem_zalloc(sizeof (*sd), KM_SLEEP);

    sd->usd_seg = seg;
    sd->usd_prot = prot;
    sd->usd_maxprot = maxprot;
    list_link_init(&sd->usd_link);
    atomic_inc_uint(&uvm_seg_live);

    return (sd);
}

static void
uvm_seg_list_add(uvm_seg_data_t *sd)
{
    struct address_space *m = sd->usd_mapping;

    mutex_enter(&m->am_lock);
    list_insert_tail(&m->am_segs, sd);
    sd->usd_listed = B_TRUE;
    mutex_exit(&m->am_lock);
}

static void
uvm_seg_list_remove_locked(uvm_seg_data_t *sd)
{
    ASSERT(MUTEX_HELD(&sd->usd_mapping->am_lock));

    if (sd->usd_listed) {
        list_remove(&sd->usd_mapping->am_segs, sd);
        sd->usd_listed = B_FALSE;
    }
}

/* Unload a range and take the segment off the list, as one step. */
static void
uvm_seg_unload_remove(uvm_seg_data_t *sd, caddr_t addr, size_t len)
{
    struct seg *seg = sd->usd_seg;

    if (sd->usd_mapping == NULL) {
        hat_unload(seg->s_as->a_hat, addr, len,
            HAT_UNLOAD_UNMAP | HAT_UNLOAD_UNLOCK);
        return;
    }

    mutex_enter(&sd->usd_mapping->am_lock);
    hat_unload(seg->s_as->a_hat, addr, len,
        HAT_UNLOAD_UNMAP | HAT_UNLOAD_UNLOCK);
    uvm_seg_list_remove_locked(sd);
    mutex_exit(&sd->usd_mapping->am_lock);
}

static struct vm_area_struct *
uvm_vma_copy(const struct vm_area_struct *vma, unsigned long start,
    unsigned long end)
{
    struct vm_area_struct *copy = kmem_alloc(sizeof (*copy), KM_SLEEP);

    *copy = *vma;
    copy->vm_start = start;
    copy->vm_end = end;
    copy->vm_pgoff = start >> PAGE_SHIFT;

    return (copy);
}

static void
uvm_vma_set_bounds(struct vm_area_struct *vma, unsigned long start,
    unsigned long end)
{
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_pgoff = start >> PAGE_SHIFT;
}

/* vm_ops may change during a call, so it is read again every time. */
static void
uvm_vma_open(struct vm_area_struct *vma)
{
    const struct vm_operations_struct *ops = vma->vm_ops;

    if (ops != NULL && ops->open != NULL)
        ops->open(vma);
}

static void
uvm_vma_close(struct vm_area_struct *vma)
{
    const struct vm_operations_struct *ops = vma->vm_ops;

    if (ops != NULL && ops->close != NULL)
        ops->close(vma);
}

/* Runs from as_map() with the AS write lock held, like Linux ->mmap. */
static int
uvm_seg_create(struct seg **segpp, void *argsp)
{
    struct seg *seg = *segpp;
    uvm_seg_crargs_t *args = argsp;
    struct linux_file *file = args->file;
    struct vm_area_struct *vma;
    uvm_seg_data_t *sd;
    int ret;

    ASSERT(AS_WRITE_HELD(seg->s_as));

    if (file->f_mapping == NULL)
        return (ENODEV);

    vma = kmem_zalloc(sizeof (*vma), KM_SLEEP);
    uvm_vma_set_bounds(vma, (unsigned long)seg->s_base,
        (unsigned long)seg->s_base + seg->s_size);
    vma->vm_mm = (struct mm_struct *)seg->s_as;
    vma->vm_flags = VM_SHARED | VM_MAYSHARE | VM_READ | VM_MAYREAD |
        VM_WRITE | VM_MAYWRITE;
    if (args->prot & PROT_EXEC)
        vma->vm_flags |= VM_EXEC | VM_MAYEXEC;
    vma->vm_page_prot.prot = args->prot;
    vma->vm_file = file;

    sd = uvm_seg_data_alloc(seg, args->prot, args->maxprot);
    sd->usd_vma = vma;
    sd->usd_file = file;
    sd->usd_mapping = file->f_mapping;
    uvm_file_hold(file);

    seg->s_ops = &uvm_seg_ops;
    seg->s_data = sd;

    /*
     * The segment goes on the list first, so that a revocation racing with
     * the mapping of a semaphore pool reaches its translations.
     */
    uvm_seg_list_add(sd);

    ret = file->f_op->mmap(file, vma);
    if (ret == 0)
        return (0);

    uvm_seg_unload_remove(sd, seg->s_base, seg->s_size);

    /* as_map() frees the segment itself when s_data is NULL. */
    seg->s_ops = NULL;
    seg->s_data = NULL;
    uvm_file_rele(file);
    kmem_free(vma, sizeof (*vma));
    kmem_free(sd, sizeof (*sd));
    atomic_dec_uint(&uvm_seg_live);

    return ((ret < 0) ? -ret : EINVAL);
}

int
uvm_seg_segmap(dev_t dev, off_t off, struct as *as, caddr_t *addrp,
    off_t len, uint_t prot, uint_t maxprot, uint_t flags, cred_t *credp)
{
    struct linux_file *file;
    uvm_seg_crargs_t args;
    int err;

    if ((flags & MAP_TYPE) != MAP_SHARED)
        return (EINVAL);
    if ((prot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE))
        return (EINVAL);
    if (off <= 0 || len <= 0 || (off & PAGEOFFSET) != 0 ||
        (len & PAGEOFFSET) != 0)
        return (EINVAL);
    if ((uint64_t)off > TASK_SIZE || (uint64_t)len > TASK_SIZE - (uint64_t)off)
        return (EINVAL);
    if (get_udatamodel() != DATAMODEL_NATIVE)
        return (ENOTSUP);

    /* UVM mappings sit at the address equal to their offset. */
    if ((flags & MAP_FIXED) && *addrp != (caddr_t)off)
        return (EINVAL);

    file = uvm_file_hold_dev(dev);
    if (file == NULL)
        return (ENXIO);

    if (file->f_op->mmap == NULL) {
        uvm_file_rele(file);
        return (ENODEV);
    }

    args.file = file;
    args.prot = (uchar_t)(prot & PROT_ALL & ~PROT_USER);
    args.maxprot = (uchar_t)(maxprot & PROT_ALL & ~PROT_USER);

    as_rangelock(as);
    if (valid_usr_range((caddr_t)off, (size_t)len, prot, as,
        as->a_userlimit) != RANGE_OKAY) {
        err = ENOMEM;
    } else {
        err = choose_addr(as, addrp, (size_t)len, off, ADDR_VACALIGN, flags);
        if (err == 0 && *addrp != (caddr_t)off)
            err = EINVAL;
    }
    if (err == 0)
        err = as_map(as, *addrp, (size_t)len, uvm_seg_create, &args);
    as_rangeunlock(as);

    uvm_file_rele(file);

    return (err);
}

/*
 * fork.  Every UVM VMA is VM_DONTCOPY, which as_dup() cannot express, so
 * the child gets a segment with no VMA and no file whose accesses fault
 * with SIGBUS.  x86 hat_dup() copies no translations.
 */
static int
uvm_seg_dup(struct seg *seg, struct seg *newseg)
{
    uvm_seg_data_t *sd = seg->s_data;

    ASSERT(AS_WRITE_HELD(seg->s_as));

    newseg->s_ops = &uvm_seg_ops;
    newseg->s_data = uvm_seg_data_alloc(newseg, sd->usd_prot,
        sd->usd_maxprot);

    return (0);
}

/*
 * munmap, including partial unmaps, with the AS write lock held.  Under
 * am_lock the range loses its translations and the segments are reshaped,
 * so unmap_mapping_range() always covers every surviving byte.  Then the
 * Linux VMA split is replayed: open() on the new VMA before the old one
 * shrinks, then close() on the piece going away.
 */
static int
uvm_seg_unmap(struct seg *seg, caddr_t addr, size_t len)
{
    uvm_seg_data_t *sd = seg->s_data, *nsd;
    struct vm_area_struct *vma = sd->usd_vma, *left, *mid, *tmp;
    unsigned long s = (unsigned long)seg->s_base;
    unsigned long e = s + seg->s_size;
    unsigned long a = (unsigned long)addr;
    unsigned long b = a + len;
    kmutex_t *lock = (sd->usd_mapping != NULL) ?
        &sd->usd_mapping->am_lock : NULL;
    struct seg *nseg;

    ASSERT(AS_WRITE_HELD(seg->s_as));

    if (a < s || b > e || b <= a || ((a | len) & PAGEOFFSET) != 0)
        return (EINVAL);

    if (a == s && b == e) {
        uvm_seg_unload_remove(sd, addr, len);
        if (vma != NULL) {
            uvm_vma_close(vma);
            sd->usd_vma = NULL;
            kmem_free(vma, sizeof (*vma));
        }
        seg_free(seg);
        return (0);
    }

    if (lock != NULL)
        mutex_enter(lock);
    hat_unload(seg->s_as->a_hat, addr, len,
        HAT_UNLOAD_UNMAP | HAT_UNLOAD_UNLOCK);

    if (a == s || b == e) {
        seg->s_base = (a == s) ? (caddr_t)b : (caddr_t)s;
        seg->s_size = (a == s) ? e - b : a - s;
        if (lock != NULL)
            mutex_exit(lock);

        if (vma == NULL)
            return (0);

        /* The piece going away is a new VMA on the split side. */
        tmp = (a == s) ? uvm_vma_copy(vma, s, b) : uvm_vma_copy(vma, a, e);
        uvm_vma_open(tmp);
        if (a == s)
            uvm_vma_set_bounds(vma, b, e);
        else
            uvm_vma_set_bounds(vma, s, a);
        uvm_vma_close(tmp);
        kmem_free(tmp, sizeof (*tmp));
        return (0);
    }

    /* Unmap from the middle: [s, a) stays here and [b, e) moves to nseg. */
    seg->s_size = a - s;
    nseg = seg_alloc(seg->s_as, (caddr_t)b, e - b);
    if (nseg == NULL) {
        seg->s_size = e - s;
        if (lock != NULL)
            mutex_exit(lock);
        return (ENOMEM);
    }
    nsd = uvm_seg_data_alloc(nseg, sd->usd_prot, sd->usd_maxprot);
    nseg->s_ops = &uvm_seg_ops;
    nseg->s_data = nsd;
    if (sd->usd_file != NULL) {
        nsd->usd_file = sd->usd_file;
        nsd->usd_mapping = sd->usd_mapping;
        uvm_file_hold(nsd->usd_file);
        list_insert_tail(&nsd->usd_mapping->am_segs, nsd);
        nsd->usd_listed = B_TRUE;
    }
    if (lock != NULL)
        mutex_exit(lock);

    if (vma == NULL)
        return (0);

    left = uvm_vma_copy(vma, s, a);
    uvm_vma_open(left);
    uvm_vma_set_bounds(vma, a, e);
    mid = uvm_vma_copy(vma, a, b);
    uvm_vma_open(mid);
    uvm_vma_set_bounds(vma, b, e);
    uvm_vma_close(mid);
    kmem_free(mid, sizeof (*mid));

    sd->usd_vma = left;
    nsd->usd_vma = vma;

    return (0);
}

static void
uvm_seg_free(struct seg *seg)
{
    uvm_seg_data_t *sd = seg->s_data;

    ASSERT(sd != NULL);

    if (sd->usd_mapping != NULL) {
        mutex_enter(&sd->usd_mapping->am_lock);
        uvm_seg_list_remove_locked(sd);
        mutex_exit(&sd->usd_mapping->am_lock);
    }
    if (sd->usd_vma != NULL) {
        uvm_vma_close(sd->usd_vma);
        kmem_free(sd->usd_vma, sizeof (struct vm_area_struct));
    }

    /* The AS write lock is held, so the last release is deferred. */
    if (sd->usd_file != NULL)
        uvm_file_rele(sd->usd_file);

    kmem_free(sd, sizeof (*sd));
    seg->s_data = NULL;
    atomic_dec_uint(&uvm_seg_live);
}

static faultcode_t
uvm_seg_vm_fault(uvm_seg_data_t *sd, caddr_t addr, size_t len,
    unsigned int flags)
{
    struct vm_area_struct *vma = sd->usd_vma;
    caddr_t va, end = addr + len;

    for (va = (caddr_t)((uintptr_t)addr & PAGEMASK); va < end;
        va += PAGESIZE) {
        const struct vm_operations_struct *ops = vma->vm_ops;
        struct vm_fault vmf = {
            .vma = vma,
            .flags = flags,
            .pgoff = (unsigned long)va >> PAGE_SHIFT,
            .address = (unsigned long)va,
        };
        vm_fault_t ret;

        if (ops == NULL || ops->fault == NULL)
            return (FC_MAKE_ERR(EFAULT));

        ret = ops->fault(&vmf);
        if (ret & VM_FAULT_OOM)
            return (FC_MAKE_ERR(ENOMEM));
        if (ret & (VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV))
            return (FC_MAKE_ERR(EFAULT));
    }

    return (0);
}

static faultcode_t
uvm_seg_fault(struct hat *hat, struct seg *seg, caddr_t addr, size_t len,
    enum fault_type type, enum seg_rw rw)
{
    uvm_seg_data_t *sd = seg->s_data;

    ASSERT(AS_LOCK_HELD(seg->s_as));

    /*
     * Managed pages move, so they cannot be soft-locked for I/O; see the
     * design notes.
     */
    if (type == F_SOFTLOCK || type == F_SOFTUNLOCK)
        return (FC_NOSUPPORT);

    if (sd->usd_vma == NULL)
        return (FC_MAKE_ERR(EFAULT));

    /*
     * UVM reduces an access to read or write, so execute and write access
     * is checked here against the mapping.
     */
    if ((rw == S_WRITE && !(sd->usd_prot & PROT_WRITE)) ||
        (rw == S_EXEC && !(sd->usd_prot & PROT_EXEC)) ||
        !(sd->usd_prot & PROT_READ))
        return (FC_PROT);

    /* A write to a read-only translation is Linux page_mkwrite. */
    if (type == F_PROT && rw != S_WRITE)
        return (FC_PROT);

    return (uvm_seg_vm_fault(sd, addr, len,
        (rw == S_WRITE) ? FAULT_FLAG_WRITE : 0));
}

static faultcode_t
uvm_seg_faulta(struct seg *seg, caddr_t addr)
{
    return (0);
}

/* UVM supports mprotect() only with HMM. */
static int
uvm_seg_setprot(struct seg *seg, caddr_t addr, size_t len, uint_t prot)
{
    uvm_seg_data_t *sd = seg->s_data;

    if ((prot & PROT_ALL & ~PROT_USER) == sd->usd_prot)
        return (0);
    return (EACCES);
}

static int
uvm_seg_checkprot(struct seg *seg, caddr_t addr, size_t len, uint_t prot)
{
    uvm_seg_data_t *sd = seg->s_data;

    prot &= ~PROT_USER;
    return (((sd->usd_prot & prot) == prot) ? 0 : EACCES);
}

static int
uvm_seg_sync(struct seg *seg, caddr_t addr, size_t len, int attr,
    uint_t flags)
{
    return (0);
}

static size_t
uvm_seg_incore(struct seg *seg, caddr_t addr, size_t len, char *vec)
{
    size_t sz = 0;

    len = (len + PAGEOFFSET) & PAGEMASK;
    while (len > 0) {
        *vec++ = 1;
        sz += PAGESIZE;
        len -= PAGESIZE;
    }

    return (sz);
}

static int
uvm_seg_lockop(struct seg *seg, caddr_t addr, size_t len, int attr, int op,
    ulong_t *lockmap, size_t pos)
{
    return (0);
}

static int
uvm_seg_getprot(struct seg *seg, caddr_t addr, size_t len, uint_t *protv)
{
    uvm_seg_data_t *sd = seg->s_data;
    size_t pgno = seg_page(seg, addr + len) - seg_page(seg, addr) + 1;

    while (pgno > 0)
        protv[--pgno] = sd->usd_prot;

    return (0);
}

static u_offset_t
uvm_seg_getoffset(struct seg *seg, caddr_t addr)
{
    return ((u_offset_t)(uintptr_t)addr);
}

static int
uvm_seg_gettype(struct seg *seg, caddr_t addr)
{
    return (MAP_SHARED);
}

static int
uvm_seg_getvp(struct seg *seg, caddr_t addr, struct vnode **vpp)
{
    *vpp = NULL;
    return (0);
}

static int
uvm_seg_advise(struct seg *seg, caddr_t addr, size_t len, uint_t behav)
{
    return ((behav == MADV_PURGE) ? EINVAL : 0);
}

static void
uvm_seg_dump(struct seg *seg)
{
}

/* as_pagelock() falls back to F_SOFTLOCK, which fails too. */
static int
uvm_seg_pagelock(struct seg *seg, caddr_t addr, size_t len,
    struct page ***ppp, enum lock_type type, enum seg_rw rw)
{
    return (ENOTSUP);
}

static int
uvm_seg_setpagesize(struct seg *seg, caddr_t addr, size_t len, uint_t szc)
{
    return (ENOTSUP);
}

static int
uvm_seg_getmemid(struct seg *seg, caddr_t addr, memid_t *memidp)
{
    uvm_seg_data_t *sd = seg->s_data;

    memidp->val[0] = (sd->usd_file != NULL) ? (uintptr_t)sd->usd_file :
        (uintptr_t)sd;
    memidp->val[1] = (uintptr_t)addr;
    return (0);
}

static int
uvm_seg_capable(struct seg *seg, segcapability_t capability)
{
    return (0);
}

/*
 * The Linux mm interfaces.
 */
void
linux_address_space_init_once(struct address_space *m)
{
    bzero(m, sizeof (*m));
    mutex_init(&m->am_lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&m->am_segs, sizeof (uvm_seg_data_t),
        offsetof(uvm_seg_data_t, usd_link));
    m->am_initialized = B_TRUE;
}

/*
 * Revoke CPU translations of [off, off + len) (offset == VA) in every
 * segment mapping this file, from any thread and without the AS lock, as
 * devmap_unload(9F) does.  A segment leaves am_segs only after its range
 * has been unloaded under the AS write lock, so a HAT seen here is live.
 * The x86 HAT drops the lock count of a locked PTE before it settles a
 * race with another unload, so every HAT_UNLOAD_UNLOCK of a seg_nvuvm
 * range runs under am_lock.
 */
void
linux_unmap_mapping_range(struct address_space *m, loff_t off, loff_t len,
    int even_cows)
{
    uintptr_t s, e;
    uvm_seg_data_t *sd;

    if (m == NULL || !m->am_initialized || off < 0 || len < 0)
        return;

    s = (uintptr_t)off;
    e = (len == 0 || (uintptr_t)len > UINTPTR_MAX - s) ? UINTPTR_MAX :
        s + (uintptr_t)len;

    mutex_enter(&m->am_lock);
    for (sd = list_head(&m->am_segs); sd != NULL;
        sd = list_next(&m->am_segs, sd)) {
        struct seg *seg = sd->usd_seg;
        uintptr_t lo = MAX(s, (uintptr_t)seg->s_base);
        uintptr_t hi = MIN(e, (uintptr_t)seg->s_base + seg->s_size);

        if (lo < hi) {
            hat_unload(seg->s_as->a_hat, (caddr_t)lo, hi - lo,
                HAT_UNLOAD_UNLOCK);
        }
    }
    mutex_exit(&m->am_lock);
}

int
linux_vm_insert_page(struct vm_area_struct *vma, unsigned long addr,
    struct page *pp)
{
    struct as *as = (struct as *)vma->vm_mm;
    uint_t prot = vma->vm_page_prot.prot & (PROT_READ | PROT_WRITE |
        PROT_EXEC);

    ASSERT(AS_LOCK_HELD(as));

    if (addr < vma->vm_start || addr >= vma->vm_end || (addr & PAGEOFFSET))
        return (-EFAULT);

    /* A double insert would leak a lock count on the page table. */
    ASSERT(hat_getpfnum(as->a_hat, (caddr_t)addr) == PFN_INVALID);

    hat_devload(as->a_hat, (caddr_t)addr, PAGESIZE, pp->p_pagenum,
        prot | PROT_USER, HAT_LOAD_LOCK);

    return (0);
}

/*
 * A Linux VMA for a segment.  Segments of other drivers get one filled in
 * per thread, which stays valid until that thread's next find_vma().
 */
static struct vm_area_struct *
uvm_seg_vma(struct seg *seg, caddr_t addr)
{
    struct task_struct *t;
    struct vm_area_struct *vma;
    uint_t prot = 0;

    if (seg->s_ops == &uvm_seg_ops) {
        uvm_seg_data_t *sd = seg->s_data;

        if (sd->usd_vma != NULL)
            return (sd->usd_vma);
    }

    t = linux_current();
    if (t->shadow_vma == NULL)
        t->shadow_vma = kmem_alloc(sizeof (*vma), KM_SLEEP);
    vma = t->shadow_vma;
    bzero(vma, sizeof (*vma));

    vma->vm_start = (unsigned long)seg->s_base;
    vma->vm_end = (unsigned long)seg->s_base + seg->s_size;
    vma->vm_mm = (struct mm_struct *)seg->s_as;
    (void) SEGOP_GETPROT(seg, MAX(addr, seg->s_base), 0, &prot);
    if (prot & PROT_READ)
        vma->vm_flags |= VM_READ | VM_MAYREAD;
    if (prot & PROT_WRITE)
        vma->vm_flags |= VM_WRITE | VM_MAYWRITE;
    if (prot & PROT_EXEC)
        vma->vm_flags |= VM_EXEC | VM_MAYEXEC;
    if (SEGOP_GETTYPE(seg, MAX(addr, seg->s_base)) & MAP_SHARED)
        vma->vm_flags |= VM_SHARED | VM_MAYSHARE;
    vma->vm_page_prot.prot = prot & ~PROT_USER;

    return (vma);
}

/* The first VMA ending above addr; the caller holds the mmap lock. */
struct vm_area_struct *
linux_find_vma(struct mm_struct *mm, unsigned long addr)
{
    struct as *as = (struct as *)mm;
    struct seg *seg;

    if (as == NULL)
        return (NULL);

    ASSERT(AS_LOCK_HELD(as));

    seg = as_findseg(as, (caddr_t)addr, 0);
    if (seg == NULL)
        return (NULL);

    return (uvm_seg_vma(seg, (caddr_t)addr));
}

struct vm_area_struct *
linux_find_vma_intersection(struct mm_struct *mm, unsigned long start,
    unsigned long end)
{
    struct vm_area_struct *vma = linux_find_vma(mm, start);

    if (vma != NULL && end <= vma->vm_start)
        return (NULL);
    return (vma);
}

/* Populate a range of one segment, as get_user_pages() would. */
int
uvm_seg_fault_range(struct mm_struct *mm, unsigned long start,
    unsigned long len, boolean_t write)
{
    struct as *as = (struct as *)mm;
    unsigned long end = start + len, chunk;
    struct seg *seg;
    faultcode_t fc;

    ASSERT(AS_LOCK_HELD(as));

    seg = as_segat(as, (caddr_t)start);
    if (seg == NULL || end < start ||
        end > (unsigned long)seg->s_base + seg->s_size)
        return (EFAULT);

    for (; start < end; start += chunk) {
        if (ISSIG(curthread, JUSTLOOKING))
            return (EINTR);

        chunk = MIN(end - start, 512 * PAGESIZE);
        fc = SEGOP_FAULT(as->a_hat, seg, (caddr_t)start, chunk, F_INVAL,
            write ? S_WRITE : S_READ);
        if (fc != 0) {
            if (FC_CODE(fc) == FC_OBJERR)
                return (FC_ERRNO(fc));
            return ((FC_CODE(fc) == FC_PROT) ? EACCES : EFAULT);
        }
    }

    return (0);
}

void
linux_mmap_read_lock(struct mm_struct *mm)
{
    AS_LOCK_ENTER((struct as *)mm, RW_READER);
}

void
linux_mmap_read_unlock(struct mm_struct *mm)
{
    AS_LOCK_EXIT((struct as *)mm);
}

void
linux_mmap_write_lock(struct mm_struct *mm)
{
    AS_LOCK_ENTER((struct as *)mm, RW_WRITER);
}

void
linux_mmap_write_unlock(struct mm_struct *mm)
{
    AS_LOCK_EXIT((struct as *)mm);
}

boolean_t
linux_mmap_is_locked(struct mm_struct *mm)
{
    return (mm != NULL && AS_LOCK_HELD((struct as *)mm));
}

void *
linux_mmap_get_lock(struct mm_struct *mm)
{
    return ((mm != NULL) ? &((struct as *)mm)->a_lock : NULL);
}
