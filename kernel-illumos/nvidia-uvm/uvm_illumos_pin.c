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
 * Pinned user pages for the UVM tools event queues and counters, and the
 * per-thread record of mmap lock holds that pinning relies on.
 *
 * UVM reaches pinned pages only through the kernel mapping vmap() makes of
 * them.  A pin is locked with umem_lockmemory(DDI_UMEMLOCK_LONGTERM), so
 * munmap(), mprotect() and exit of the range call uvm_pin_cleanup(), which
 * must unlock the pages or the caller waits forever.  UVM cannot be told to
 * stop writing, so a revoked pin keeps its kernel address but every page of
 * it is remapped in place to a sink page of its own before the user pages
 * are unlocked.  Unlocking takes the AS lock, so outside the callback it is
 * left to a taskq: UVM unpins with its own locks held, and a munmap of a
 * managed range holds the AS lock while it takes those.
 *
 * Lock order: AS lock, then up_lock of a pin, then uvm_pin_lock.  The AS
 * callback runs with no AS lock held.
 */

#include "uvm_illumos.h"
#include "uvm_illumos_mem.h"

#include <sys/avl.h>
#include <sys/vmem.h>
#include <sys/taskq_impl.h>
#include <sys/ddidevmap.h>
#include <sys/esunddi.h>
#include <vm/as.h>
#include <vm/seg.h>
#include <vm/seg_vn.h>
#include <vm/seg_kmem.h>
#include <vm/hat.h>

extern struct seg_ops segspt_shmops;

/* One pin covers at most 256MB; all pins together at most physmem / 8. */
#define UVM_PIN_MAX_PAGES       btop(256UL << 20)

/* How many duplicate entries of one page unpin looks at for a better match. */
#define UVM_PIN_DUP_SCAN        16

typedef enum {
    UVM_PIN_NEW,
    UVM_PIN_LIVE,               /* pages locked */
    UVM_PIN_REVOKED             /* unlocking; the kernel mapping hits the sink */
} uvm_pin_state_t;

typedef struct uvm_pin uvm_pin_t;

typedef struct uvm_pin_page {
    avl_node_t          upp_link;       /* uvm_pin_pages while held */
    page_t             *upp_page;
    uvm_pin_t          *upp_pin;
    pgcnt_t             upp_index;
} uvm_pin_page_t;

struct uvm_pin {
    kmutex_t            up_lock;
    uvm_pin_state_t     up_state;       /* changed under both locks */
    caddr_t             up_kva;         /* up_lock */

    /* uvm_pin_lock */
    avl_node_t          up_cookie_link;
    avl_node_t          up_kva_link;
    taskq_ent_t         up_tqent;
    list_node_t         up_pending_link;
    uint64_t            up_seq;
    ddi_umem_cookie_t   up_cookie;
    struct page        *up_sink;
    uvm_charge_t       *up_charge;      /* the sink and this structure */
    struct as          *up_as;
    struct page       **up_array;       /* the caller's array, for vmap() */
    pgcnt_t             up_npages;
    pgcnt_t             up_held;
    uint_t              up_refs;
    boolean_t           up_write;
    boolean_t           up_in_table;    /* on uvm_pin_cookies */
    boolean_t           up_cb_seen;     /* uvm_pin_cleanup() ran for it */
    boolean_t           up_mapped;
    boolean_t           up_kmapped;     /* up_kva made by kmap(), kept to the end */

    uvm_pin_page_t      up_pages[];
};

static kmutex_t     uvm_pin_lock;
static kcondvar_t   uvm_pin_cv;
static list_t       uvm_pin_pending;    /* inside umem_lockmemory() */
static uint64_t     uvm_pin_seq;
static avl_tree_t   uvm_pin_pages;
static avl_tree_t   uvm_pin_cookies;
static avl_tree_t   uvm_pin_kvas;
static pgcnt_t      uvm_pin_total;
static volatile ulong_t uvm_pin_held_total;  /* read without the lock */
static pgcnt_t      uvm_pin_limit;
static boolean_t    uvm_pin_used;
static taskq_t     *uvm_pin_tq;

static void uvm_pin_cleanup(ddi_umem_cookie_t *);

static struct umem_callback_ops uvm_pin_cbops = {
    .cbo_umem_callback_version = UMEM_CALLBACK_VERSION,
    .cbo_umem_lock_cleanup = uvm_pin_cleanup,
};

static int
uvm_pin_page_compare(const void *a, const void *b)
{
    const uvm_pin_page_t *x = a, *y = b;

    if (x->upp_page != y->upp_page)
        return ((uintptr_t)x->upp_page < (uintptr_t)y->upp_page ? -1 : 1);
    if (x->upp_pin != y->upp_pin)
        return ((uintptr_t)x->upp_pin < (uintptr_t)y->upp_pin ? -1 : 1);
    if (x->upp_index != y->upp_index)
        return (x->upp_index < y->upp_index ? -1 : 1);
    return (0);
}

static int
uvm_pin_cookie_compare(const void *a, const void *b)
{
    const uvm_pin_t *x = a, *y = b;

    if (x->up_cookie == y->up_cookie)
        return (0);
    return ((uintptr_t)x->up_cookie < (uintptr_t)y->up_cookie ? -1 : 1);
}

static int
uvm_pin_kva_compare(const void *a, const void *b)
{
    const uvm_pin_t *x = a, *y = b;

    if (x->up_kva == y->up_kva)
        return (0);
    return ((uintptr_t)x->up_kva < (uintptr_t)y->up_kva ? -1 : 1);
}


/*
 * Per-thread record of mmap lock holds taken through nv_mmap_*_lock(),
 * because a reader hold on an rwlock does not say which thread took it.  It
 * also carries the thread's memcg accounting target and the pin it last
 * pinned or unmapped, which UVM unpins next.
 */
#define UVM_MMAP_HOLDS          4

typedef struct uvm_thread_rec {
    struct {
        struct as  *as;
        uint_t      readers;
        uint_t      writers;
    } utr_holds[UVM_MMAP_HOLDS];
    void           *utr_memcg;
    const void     *utr_pin;            /* compared, never dereferenced */
} uvm_thread_rec_t;

static uint_t uvm_thread_key;

static void
uvm_thread_rec_free(void *arg)
{
    kmem_free(arg, sizeof (uvm_thread_rec_t));
}

static uvm_thread_rec_t *
uvm_thread_rec(boolean_t create)
{
    uvm_thread_rec_t *r;

    if (servicing_interrupt())
        return (NULL);

    r = tsd_get(uvm_thread_key);
    if (r == NULL && create) {
        r = kmem_zalloc(sizeof (*r), KM_SLEEP);
        VERIFY0(tsd_set(uvm_thread_key, r));
    }
    return (r);
}

void
uvm_mmap_note(struct as *as, boolean_t write, int delta)
{
    uvm_thread_rec_t *r = uvm_thread_rec(B_TRUE);
    uint_t i, slot = UVM_MMAP_HOLDS;

    for (i = 0; i < UVM_MMAP_HOLDS; i++) {
        if (r->utr_holds[i].as == as) {
            slot = i;
            break;
        }
        if (slot == UVM_MMAP_HOLDS && r->utr_holds[i].as == NULL)
            slot = i;
    }
    VERIFY3U(slot, <, UVM_MMAP_HOLDS);

    if (delta > 0) {
        r->utr_holds[slot].as = as;
        if (write)
            r->utr_holds[slot].writers++;
        else
            r->utr_holds[slot].readers++;
        return;
    }

    VERIFY(r->utr_holds[slot].as == as);
    if (write) {
        VERIFY(r->utr_holds[slot].writers > 0);
        r->utr_holds[slot].writers--;
    } else {
        VERIFY(r->utr_holds[slot].readers > 0);
        r->utr_holds[slot].readers--;
    }
    if (r->utr_holds[slot].readers == 0 && r->utr_holds[slot].writers == 0)
        r->utr_holds[slot].as = NULL;
}

void
uvm_mmap_holds(struct as *as, uint_t *readers, uint_t *writers)
{
    uvm_thread_rec_t *r = uvm_thread_rec(B_FALSE);
    uint_t i;

    *readers = *writers = 0;
    for (i = 0; r != NULL && i < UVM_MMAP_HOLDS; i++) {
        if (r->utr_holds[i].as == as) {
            *readers = r->utr_holds[i].readers;
            *writers = r->utr_holds[i].writers;
            return;
        }
    }
}

void *
uvm_memcg_swap(void *memcg)
{
    uvm_thread_rec_t *r = uvm_thread_rec(B_TRUE);
    void *old = r->utr_memcg;

    r->utr_memcg = memcg;
    return (old);
}

void *
uvm_memcg_active(void)
{
    uvm_thread_rec_t *r = uvm_thread_rec(B_FALSE);

    return ((r != NULL) ? r->utr_memcg : NULL);
}

static void
uvm_pin_hint(const uvm_pin_t *up)
{
    uvm_thread_rec(B_TRUE)->utr_pin = up;
}

static size_t
uvm_pin_size(pgcnt_t npages)
{
    return (offsetof(uvm_pin_t, up_pages) + npages * sizeof (uvm_pin_page_t));
}

/* Drops a reference with uvm_pin_lock held; B_TRUE means free it. */
static boolean_t
uvm_pin_rele_locked(uvm_pin_t *up)
{
    ASSERT(MUTEX_HELD(&uvm_pin_lock));
    VERIFY(up->up_refs > 0);
    return (--up->up_refs == 0);
}

static void
uvm_pin_free(uvm_pin_t *up)
{
    VERIFY(!up->up_in_table && up->up_held == 0);

    if (up->up_kmapped) {
        hat_unload(kas.a_hat, up->up_kva, ptob(up->up_npages),
            HAT_UNLOAD_UNLOCK);
        vmem_free(heap_arena, up->up_kva, ptob(up->up_npages));
        up->up_kva = NULL;
    }
    VERIFY(up->up_kva == NULL);

    mutex_enter(&uvm_pin_lock);
    uvm_pin_total -= up->up_npages;
    mutex_exit(&uvm_pin_lock);

    if (up->up_sink != NULL)
        linux_free_pages(up->up_sink, 0);
    uvm_charge_rele(up->up_charge);
    mutex_destroy(&up->up_lock);
    kmem_free(up, uvm_pin_size(up->up_npages));
}

static void
uvm_pin_rele(uvm_pin_t *up)
{
    boolean_t last;

    mutex_enter(&uvm_pin_lock);
    last = uvm_pin_rele_locked(up);
    mutex_exit(&uvm_pin_lock);

    if (last)
        uvm_pin_free(up);
}

static void
uvm_pin_untable_locked(uvm_pin_t *up)
{
    ASSERT(MUTEX_HELD(&uvm_pin_lock));

    if (up->up_in_table) {
        avl_remove(&uvm_pin_cookies, up);
        up->up_in_table = B_FALSE;
        VERIFY(up->up_refs > 1);
        up->up_refs--;
    }
}

/*
 * Kernel mapping of a pin, or of its sink when sink is set.  NOCONSIST
 * from the start, so a remap may change the frame, and HAT_NOSYNC presets
 * the referenced bit, so that remap always shoots down the TLB entries.
 */
static void
uvm_pin_load(uvm_pin_t *up, boolean_t sink, uint_t flags)
{
    uint_t attr = PROT_READ | (up->up_write ? PROT_WRITE : 0) | HAT_NOSYNC;
    pgcnt_t i;

    ASSERT(MUTEX_HELD(&up->up_lock));

    for (i = 0; i < up->up_npages; i++) {
        hat_memload(kas.a_hat, up->up_kva + ptob(i),
            sink ? up->up_sink : up->up_pages[i].upp_page, attr,
            HAT_LOAD_LOCK | HAT_LOAD_NOCONSIST | flags);
    }
}

static void
uvm_pin_unlock_task(void *arg)
{
    uvm_pin_t *up = arg;

    ddi_umem_unlock(up->up_cookie);
    uvm_pin_rele(up);
}

/*
 * Stop kernel access to the pages of a live pin and unlock them; runs once
 * per pin.  The kernel mapping, if any, is switched to the sink in place,
 * so a UVM writer racing with this never faults, and only once every page
 * is switched are the user pages let go.
 */
static void
uvm_pin_revoke(uvm_pin_t *up, boolean_t from_callback)
{
    uint_t rc;

    mutex_enter(&up->up_lock);
    if (up->up_state != UVM_PIN_LIVE) {
        mutex_exit(&up->up_lock);
        return;
    }
    mutex_enter(&uvm_pin_lock);
    up->up_state = UVM_PIN_REVOKED;
    mutex_exit(&uvm_pin_lock);

    if (up->up_kva != NULL)
        uvm_pin_load(up, B_TRUE, HAT_LOAD_REMAP);
    mutex_exit(&up->up_lock);

    /*
     * Delete the AS callback here, so that none can start once the pages
     * are unlocked.  DEFERRED outside the callback means one is already on
     * its way to uvm_pin_cleanup(); the pin stays in the cookie table until
     * it arrives, which also keeps the cookie from being freed and reused
     * while it is a key there.
     */
    mutex_enter(&uvm_pin_lock);
    rc = as_delete_callback(up->up_as, up->up_cookie);
    if (from_callback || up->up_cb_seen || rc != AS_CALLBACK_DELETE_DEFERRED)
        uvm_pin_untable_locked(up);
    if (!from_callback)
        up->up_refs++;
    mutex_exit(&uvm_pin_lock);

    if (from_callback)
        ddi_umem_unlock(up->up_cookie);
    else
        taskq_dispatch_ent(uvm_pin_tq, uvm_pin_unlock_task, up, 0,
            &up->up_tqent);
}

/* Is every segment of [addr, addr + len) one whose pages may be pinned? */
static boolean_t
uvm_pin_segs_ok(struct as *as, caddr_t addr, size_t len)
{
    caddr_t end = addr + len;
    struct seg *seg;

    ASSERT(AS_LOCK_HELD(as));

    for (seg = as_segat(as, addr); seg != NULL && addr < end;
        seg = AS_SEGNEXT(as, seg)) {
        if (seg->s_base > addr)
            return (B_FALSE);
        if (seg->s_ops != &segvn_ops && seg->s_ops != &segspt_shmops)
            return (B_FALSE);
        addr = seg->s_base + seg->s_size;
    }

    return (addr >= end);
}

/* Record the locked pages of a live pin.  The AS lock is held. */
static int
uvm_pin_fill(uvm_pin_t *up, caddr_t addr)
{
    page_t **pparray;
    pgcnt_t i;

    mutex_enter(&up->up_lock);
    if (up->up_state != UVM_PIN_LIVE ||
        !uvm_pin_segs_ok(up->up_as, addr, ptob(up->up_npages))) {
        mutex_exit(&up->up_lock);
        return (EFAULT);
    }

    pparray = ((struct ddi_umem_cookie *)up->up_cookie)->pparray;
    for (i = 0; i < up->up_npages; i++) {
        page_t *pp;

        if (pparray != NULL) {
            pp = pparray[i];
        } else {
            /* A softlocked range has locked translations instead. */
            pfn_t pfn = hat_getpfnum(up->up_as->a_hat, addr + ptob(i));

            pp = (pfn == PFN_INVALID) ? NULL : page_numtopp_nolock(pfn);
        }
        if (pp == NULL) {
            mutex_exit(&up->up_lock);
            return (EFAULT);
        }
        up->up_pages[i].upp_page = pp;
        up->up_pages[i].upp_pin = up;
        up->up_pages[i].upp_index = i;
    }

    mutex_enter(&uvm_pin_lock);
    for (i = 0; i < up->up_npages; i++)
        avl_add(&uvm_pin_pages, &up->up_pages[i]);
    up->up_held = up->up_npages;
    uvm_pin_held_total += up->up_npages;
    up->up_refs++;
    mutex_exit(&uvm_pin_lock);
    mutex_exit(&up->up_lock);

    return (0);
}

/*
 * pin_user_pages(): lock n pages of the current process from start.  The
 * caller holds the mmap lock for read or not at all; with it held for
 * write, umem_lockmemory() would block on the AS lock.  A reader hold is
 * kept throughout, which keeps as_unmap() out, but as_setprot() calls back
 * as a reader, possibly before the cookie is recorded here; the callback
 * then waits for the pins that were pending when it started.  Returns n or
 * a negative errno.
 */
long
linux_pin_user_pages(unsigned long start, unsigned long n, unsigned int flags,
    struct page **pages)
{
    proc_t *p = curproc;
    struct as *as = p->p_as;
    caddr_t addr = (caddr_t)(start & PAGEMASK);
    ddi_umem_cookie_t cookie;
    uvm_charge_t *charge;
    uvm_pin_t *up;
    size_t len;
    uint_t readers, writers;
    pgcnt_t i;
    int err;

    if (as == &kas || pages == NULL || n == 0 || n > UVM_PIN_MAX_PAGES)
        return (-EINVAL);
    len = ptob(n);
    if ((uintptr_t)addr + len < (uintptr_t)addr ||
        (uintptr_t)addr + len > (uintptr_t)as->a_userlimit)
        return (-EFAULT);

    uvm_mmap_holds(as, &readers, &writers);
    if (writers != 0)
        return (-EDEADLK);

    /*
     * Admission comes before any allocation.  The pinned pages are charged
     * by umem_lockmemory() while locked; what stays until the tracker goes
     * (this structure and the sink) is charged here.
     */
    mutex_enter(&uvm_pin_lock);
    if (uvm_pin_total + n > uvm_pin_limit) {
        mutex_exit(&uvm_pin_lock);
        return (-ENOMEM);
    }
    uvm_pin_total += n;
    uvm_pin_used = B_TRUE;
    mutex_exit(&uvm_pin_lock);

    charge = uvm_charge_take(PAGESIZE + uvm_pin_size(n), &err);
    if (charge == NULL) {
        mutex_enter(&uvm_pin_lock);
        uvm_pin_total -= n;
        mutex_exit(&uvm_pin_lock);
        return (-ENOMEM);
    }

    up = kmem_zalloc(uvm_pin_size(n), KM_SLEEP);
    mutex_init(&up->up_lock, NULL, MUTEX_DRIVER, NULL);
    up->up_state = UVM_PIN_NEW;
    up->up_charge = charge;
    up->up_as = as;
    up->up_array = pages;
    up->up_npages = n;
    up->up_write = (flags & FOLL_WRITE) != 0;
    up->up_refs = 1;

    up->up_sink = linux_alloc_pages(__GFP_ZERO, 0);
    if (up->up_sink == NULL) {
        uvm_pin_rele(up);
        return (-ENOMEM);
    }

    if (readers == 0)
        AS_LOCK_ENTER(as, RW_READER);

    /* Device and UVM mappings are refused before anything touches them. */
    err = uvm_pin_segs_ok(as, addr, len) ? 0 : EFAULT;
    if (err == 0) {
        mutex_enter(&uvm_pin_lock);
        up->up_seq = ++uvm_pin_seq;
        list_insert_tail(&uvm_pin_pending, up);
        mutex_exit(&uvm_pin_lock);

        err = umem_lockmemory(addr, len, DDI_UMEMLOCK_READ |
            (up->up_write ? DDI_UMEMLOCK_WRITE : 0) | DDI_UMEMLOCK_LONGTERM,
            &cookie, &uvm_pin_cbops, p);

        mutex_enter(&up->up_lock);
        mutex_enter(&uvm_pin_lock);
        list_remove(&uvm_pin_pending, up);
        if (err == 0) {
            up->up_cookie = cookie;
            up->up_state = UVM_PIN_LIVE;
            avl_add(&uvm_pin_cookies, up);
            up->up_in_table = B_TRUE;
            up->up_refs++;
        }
        cv_broadcast(&uvm_pin_cv);
        mutex_exit(&uvm_pin_lock);
        mutex_exit(&up->up_lock);
    }
    if (err == 0) {
        err = uvm_pin_fill(up, addr);
        if (err != 0)
            uvm_pin_revoke(up, B_FALSE);
    }

    if (readers == 0)
        AS_LOCK_EXIT(as);

    if (err != 0) {
        uvm_pin_rele(up);
        return (-err);
    }

    for (i = 0; i < n; i++)
        pages[i] = up->up_pages[i].upp_page;
    uvm_pin_hint(up);
    uvm_pin_rele(up);

    return ((long)n);
}

/* The first held entry at or after key. */
static uvm_pin_page_t *
uvm_pin_page_ceil(const uvm_pin_page_t *key)
{
    uvm_pin_page_t *e;
    avl_index_t where;

    ASSERT(MUTEX_HELD(&uvm_pin_lock));

    e = avl_find(&uvm_pin_pages, key, &where);
    return ((e != NULL) ? e : avl_nearest(&uvm_pin_pages, where, AVL_AFTER));
}

/*
 * A held entry for pp.  The pin this thread last pinned or unmapped comes
 * first, then an unmapped pin, then any.
 */
static uvm_pin_page_t *
uvm_pin_lookup_locked(struct page *pp)
{
    uvm_thread_rec_t *r = uvm_thread_rec(B_FALSE);
    uvm_pin_page_t key = { .upp_page = pp }, *e, *best = NULL;
    uint_t scan = 0;

    ASSERT(MUTEX_HELD(&uvm_pin_lock));

    if (r != NULL && r->utr_pin != NULL) {
        key.upp_pin = (uvm_pin_t *)r->utr_pin;
        e = uvm_pin_page_ceil(&key);
        if (e != NULL && e->upp_page == pp && e->upp_pin == key.upp_pin)
            return (e);
        key.upp_pin = NULL;
    }

    for (e = uvm_pin_page_ceil(&key);
        e != NULL && e->upp_page == pp && scan < UVM_PIN_DUP_SCAN;
        e = AVL_NEXT(&uvm_pin_pages, e), scan++) {
        if (best == NULL)
            best = e;
        if (!e->upp_pin->up_mapped)
            return (e);
    }

    return (best);
}

/*
 * unpin_user_page().  The same page may be held by several pins.  UVM
 * unpins the pages of one pin right after pinning or unmapping it, which
 * the thread record remembers; otherwise an unmapped pin is the better
 * match.  Any choice keeps the counts balanced, and a pin whose count drops
 * to zero is revoked, so its kernel mapping never outlives its pages.
 */
void
linux_unpin_user_page(struct page *pp)
{
    uvm_pin_page_t *best;
    uvm_pin_t *up;
    boolean_t last;

    mutex_enter(&uvm_pin_lock);
    best = uvm_pin_lookup_locked(pp);
    if (best == NULL) {
        mutex_exit(&uvm_pin_lock);
        cmn_err(CE_WARN, "nvidia_uvm: unpin of a page not pinned %p",
            (void *)pp);
        return;
    }

    avl_remove(&uvm_pin_pages, best);
    uvm_pin_held_total--;
    up = best->upp_pin;
    last = (--up->up_held == 0);
    mutex_exit(&uvm_pin_lock);

    /* The reference the held pages had passes to us. */
    if (last) {
        uvm_pin_revoke(up, B_FALSE);
        uvm_pin_rele(up);
    }
}

/* The pin whose page array this is, held, or NULL. */
static uvm_pin_t *
uvm_pin_find_array(struct page **pages)
{
    uvm_pin_page_t key = { .upp_page = pages[0] }, *e;
    uvm_pin_t *up = NULL;

    mutex_enter(&uvm_pin_lock);
    for (e = uvm_pin_page_ceil(&key); e != NULL && e->upp_page == pages[0];
        e = AVL_NEXT(&uvm_pin_pages, e)) {
        if (e->upp_pin->up_array == pages) {
            up = e->upp_pin;
            up->up_refs++;
            break;
        }
    }
    mutex_exit(&uvm_pin_lock);

    return (up);
}

/*
 * vmap() of an array filled by linux_pin_user_pages().  Returns B_FALSE if
 * the array is not a pin's, and the caller maps it normally.
 */
boolean_t
uvm_pin_vmap(struct page **pages, unsigned int count, void **vap)
{
    uvm_pin_t *up = uvm_pin_find_array(pages);
    caddr_t va;

    if (up == NULL)
        return (B_FALSE);

    *vap = NULL;
    if (count != up->up_npages) {
        uvm_pin_rele(up);
        return (B_TRUE);
    }

    va = vmem_alloc(heap_arena, ptob(up->up_npages), VM_SLEEP);

    mutex_enter(&up->up_lock);
    if (up->up_kva != NULL) {
        mutex_exit(&up->up_lock);
        vmem_free(heap_arena, va, ptob(up->up_npages));
        uvm_pin_rele(up);
        return (B_TRUE);
    }
    up->up_kva = va;
    uvm_pin_load(up, up->up_state != UVM_PIN_LIVE, 0);

    /* The lookup reference becomes the mapping's. */
    mutex_enter(&uvm_pin_lock);
    avl_add(&uvm_pin_kvas, up);
    up->up_mapped = B_TRUE;
    mutex_exit(&uvm_pin_lock);
    mutex_exit(&up->up_lock);

    *vap = va;
    return (B_TRUE);
}

/*
 * kmap() of a pinned page: the whole pin gets a kernel mapping that is
 * revoked like a vmap() one and lasts until the pin is gone.  Pages are
 * never reached through segkpm, which revocation could not redirect.
 */
void *
uvm_pin_kmap(struct page *pp)
{
    uvm_pin_page_t *e;
    uvm_pin_t *up;
    pgcnt_t index;
    caddr_t va;

    /* The common case, kmap() of our own pages with no pins about. */
    if (uvm_pin_held_total == 0)
        return (NULL);

    mutex_enter(&uvm_pin_lock);
    e = uvm_pin_lookup_locked(pp);
    if (e == NULL) {
        mutex_exit(&uvm_pin_lock);
        return (NULL);
    }
    up = e->upp_pin;
    index = e->upp_index;
    up->up_refs++;
    mutex_exit(&uvm_pin_lock);

    mutex_enter(&up->up_lock);
    if (up->up_kva == NULL) {
        up->up_kva = vmem_alloc(heap_arena, ptob(up->up_npages), VM_SLEEP);
        up->up_kmapped = B_TRUE;
        uvm_pin_load(up, up->up_state != UVM_PIN_LIVE, 0);
        mutex_enter(&uvm_pin_lock);
        up->up_mapped = B_TRUE;
        mutex_exit(&uvm_pin_lock);
    }
    va = up->up_kmapped ? up->up_kva + ptob(index) : NULL;
    mutex_exit(&up->up_lock);

    uvm_pin_rele(up);
    return (va);
}

/* vunmap() of a pin's mapping; B_FALSE if va is not one. */
boolean_t
uvm_pin_vunmap(const void *va)
{
    uvm_pin_t key, *up;
    size_t size;

    key.up_kva = (caddr_t)va;
    mutex_enter(&uvm_pin_lock);
    up = avl_find(&uvm_pin_kvas, &key, NULL);
    if (up != NULL) {
        avl_remove(&uvm_pin_kvas, up);
        up->up_mapped = B_FALSE;
    }
    mutex_exit(&uvm_pin_lock);

    if (up == NULL)
        return (B_FALSE);

    size = ptob(up->up_npages);
    mutex_enter(&up->up_lock);
    hat_unload(kas.a_hat, up->up_kva, size, HAT_UNLOAD_UNLOCK);
    up->up_kva = NULL;
    mutex_exit(&up->up_lock);

    vmem_free(heap_arena, (void *)va, size);
    uvm_pin_hint(up);
    uvm_pin_rele(up);

    return (B_TRUE);
}

/* Is a pin of this AS no later than seq inside umem_lockmemory()? */
static boolean_t
uvm_pin_pending_on(struct as *as, uint64_t seq)
{
    uvm_pin_t *up;

    ASSERT(MUTEX_HELD(&uvm_pin_lock));

    for (up = list_head(&uvm_pin_pending); up != NULL;
        up = list_next(&uvm_pin_pending, up)) {
        if (up->up_as == as && up->up_seq <= seq)
            return (B_TRUE);
    }
    return (B_FALSE);
}

/*
 * The umem_lock_undo() callback, from as_unmap(), as_setprot() or as_free()
 * with no AS lock held.  The declared argument type is wrong: umem passes
 * the cookie itself.
 */
static void
uvm_pin_cleanup(ddi_umem_cookie_t *arg)
{
    ddi_umem_cookie_t cookie = (ddi_umem_cookie_t)arg;
    struct as *as = ((struct ddi_umem_cookie *)cookie)->asp;
    uvm_pin_t key, *up;
    boolean_t last;
    uint64_t seq;

    key.up_cookie = cookie;

    /* Only a pin already pending can own a cookie this callback is for. */
    mutex_enter(&uvm_pin_lock);
    seq = uvm_pin_seq;
    while ((up = avl_find(&uvm_pin_cookies, &key, NULL)) == NULL &&
        uvm_pin_pending_on(as, seq))
        cv_wait(&uvm_pin_cv, &uvm_pin_lock);
    if (up != NULL) {
        up->up_cb_seen = B_TRUE;
        up->up_refs++;
    }
    mutex_exit(&uvm_pin_lock);

    /* Every cookie of ours is recorded, so the caller would wait forever. */
    if (up == NULL) {
        cmn_err(CE_WARN, "nvidia_uvm: callback for an unknown pin %p",
            (void *)cookie);
        return;
    }

    uvm_pin_revoke(up, B_TRUE);

    mutex_enter(&uvm_pin_lock);
    uvm_pin_untable_locked(up);
    last = uvm_pin_rele_locked(up);
    mutex_exit(&uvm_pin_lock);

    if (last)
        uvm_pin_free(up);
}

/*
 * Once a pin has existed the module stays loaded: an AS callback may still
 * be returning through uvm_pin_cleanup() after every count reaches zero.
 */
boolean_t
uvm_pin_busy(void)
{
    return (uvm_pin_used);
}

void
uvm_pin_init(void)
{
    uvm_pin_tq = taskq_create("nvidia_uvm_unpin", 1, minclsyspri, 1,
        INT_MAX, TASKQ_PREPOPULATE);
    tsd_create(&uvm_thread_key, uvm_thread_rec_free);

    mutex_init(&uvm_pin_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&uvm_pin_cv, NULL, CV_DRIVER, NULL);
    list_create(&uvm_pin_pending, sizeof (uvm_pin_t),
        offsetof(uvm_pin_t, up_pending_link));
    avl_create(&uvm_pin_pages, uvm_pin_page_compare, sizeof (uvm_pin_page_t),
        offsetof(uvm_pin_page_t, upp_link));
    avl_create(&uvm_pin_cookies, uvm_pin_cookie_compare, sizeof (uvm_pin_t),
        offsetof(uvm_pin_t, up_cookie_link));
    avl_create(&uvm_pin_kvas, uvm_pin_kva_compare, sizeof (uvm_pin_t),
        offsetof(uvm_pin_t, up_kva_link));
    uvm_pin_total = 0;
    uvm_pin_limit = physmem / 8;
    uvm_pin_used = B_FALSE;
}

void
uvm_pin_fini(void)
{
    VERIFY(!uvm_pin_used);

    avl_destroy(&uvm_pin_kvas);
    avl_destroy(&uvm_pin_cookies);
    avl_destroy(&uvm_pin_pages);
    list_destroy(&uvm_pin_pending);
    cv_destroy(&uvm_pin_cv);
    mutex_destroy(&uvm_pin_lock);

    tsd_destroy(&uvm_thread_key);
    taskq_destroy(uvm_pin_tq);
}
