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
 * Linux kernel services for nvidia-uvm: the current task, kernel memory,
 * printing, sleeping, wait queues, delayed work and bit locks.
 */

#include "uvm_illumos.h"

#include <sys/user.h>
#include <sys/random.h>
#include <sys/kmem_impl.h>
#include <sys/bitmap.h>
#include <sys/avl.h>

extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));

struct cpumask  linux_cpu_all_mask;
nodemask_t      linux_node_online_map;

/*
 * The current task.  Each thread that enters UVM gets a task_struct in
 * thread-specific data, refreshed on every call because a thread's process
 * address space changes across exec.
 */
static uint_t linux_task_key;

/* Interrupt threads get a shared placeholder and never allocate. */
static struct task_struct linux_intr_task = {
    .pid = -1,
    .tgid = -1,
    .flags = PF_KTHREAD,
    .comm = "interrupt",
};

static void
linux_task_free(void *arg)
{
    struct task_struct *t = arg;

    if (t->shadow_vma != NULL)
        kmem_free(t->shadow_vma, sizeof (struct vm_area_struct));
    kmem_free(t, sizeof (*t));
}

struct task_struct *
linux_current(void)
{
    struct task_struct *t;
    proc_t *p = curproc;

    if (servicing_interrupt())
        return (&linux_intr_task);

    t = tsd_get(linux_task_key);
    if (t == NULL) {
        t = kmem_zalloc(sizeof (*t), KM_SLEEP);
        t->thread = curthread;
        t->did = curthread->t_did;
        VERIFY0(tsd_set(linux_task_key, t));
    }

    t->mm = (p->p_as == &kas) ? NULL : (struct mm_struct *)p->p_as;
    t->pid = (pid_t)curthread->t_did;
    t->tgid = p->p_pid;
    t->flags = (p == &p0) ? PF_KTHREAD : 0;
    (void) strlcpy(t->comm, PTOU(p)->u_comm, sizeof (t->comm));

    return (t);
}

/*
 * kmalloc and vmalloc.  Both come from kmem_alloc() behind a header that
 * records the size, for ksize().  The 64-bit magic sits right before the
 * returned pointer.  UVM passes is_vmalloc_addr() pointers into its own
 * vmalloc header, so vmalloc allocations are also kept in an AVL tree of
 * address ranges.
 */
#define LINUX_KMALLOC_MAGIC     0xfee1dead6b6d616cULL
#define LINUX_VMALLOC_MAGIC     0xfee1dead766d616cULL

/* Linux vmalloc fails under memory pressure; kmem_alloc(KM_SLEEP) waits. */
#define LINUX_VMALLOC_SLEEP_MAX (1024 * 1024)

typedef struct linux_kmalloc_hdr {
    size_t      lkh_size;
    uint64_t    lkh_magic;
} linux_kmalloc_hdr_t;

typedef struct linux_vmalloc_hdr {
    avl_node_t  lvh_link;
    uintptr_t   lvh_start;
    size_t      lvh_size;
    uint64_t    lvh_magic;
} linux_vmalloc_hdr_t;

CTASSERT(sizeof (linux_kmalloc_hdr_t) == 16);
CTASSERT(sizeof (linux_vmalloc_hdr_t) % 16 == 0);

static kmutex_t     linux_vmalloc_lock;
static avl_tree_t   linux_vmalloc_tree;

static int
linux_kmflag(gfp_t gfp)
{
    return ((gfp & __GFP_NOSLEEP) ? KM_NOSLEEP : KM_SLEEP);
}

/* Orders disjoint ranges; a lookup key of size 1 finds its container. */
static int
linux_vmalloc_compare(const void *a, const void *b)
{
    const linux_vmalloc_hdr_t *x = a, *y = b;

    if (x->lvh_start + x->lvh_size <= y->lvh_start)
        return (-1);
    if (y->lvh_start + y->lvh_size <= x->lvh_start)
        return (1);
    return (0);
}

static linux_kmalloc_hdr_t *
linux_kmalloc_hdr(const void *p)
{
    linux_kmalloc_hdr_t *h = (linux_kmalloc_hdr_t *)p - 1;

    VERIFY(h->lkh_magic == LINUX_KMALLOC_MAGIC);
    return (h);
}

static linux_vmalloc_hdr_t *
linux_vmalloc_hdr(const void *p)
{
    linux_vmalloc_hdr_t *h = (linux_vmalloc_hdr_t *)p - 1;

    VERIFY(h->lvh_magic == LINUX_VMALLOC_MAGIC);
    return (h);
}

void *
linux_kmalloc(size_t size, gfp_t gfp)
{
    linux_kmalloc_hdr_t *h;
    int kmflag = linux_kmflag(gfp);

    if (size == 0)
        return (ZERO_SIZE_PTR);

    /* A sleeping allocation larger than memory would never return. */
    if (size > ptob(physmem) / 2)
        return (NULL);

    h = (gfp & __GFP_ZERO) ? kmem_zalloc(size + sizeof (*h), kmflag) :
        kmem_alloc(size + sizeof (*h), kmflag);
    if (h == NULL)
        return (NULL);

    h->lkh_size = size;
    h->lkh_magic = LINUX_KMALLOC_MAGIC;
    return (h + 1);
}

void
linux_kfree(const void *p)
{
    linux_kmalloc_hdr_t *h;

    if (ZERO_OR_NULL_PTR(p))
        return;

    h = linux_kmalloc_hdr(p);
    h->lkh_magic = 0;
    kmem_free(h, h->lkh_size + sizeof (*h));
}

size_t
linux_ksize(const void *p)
{
    if (ZERO_OR_NULL_PTR(p))
        return (0);

    return (linux_kmalloc_hdr(p)->lkh_size);
}

void *
linux_krealloc(const void *p, size_t size, gfp_t gfp)
{
    void *n;

    if (ZERO_OR_NULL_PTR(p))
        return (linux_kmalloc(size, gfp));

    if (size == 0) {
        linux_kfree(p);
        return (ZERO_SIZE_PTR);
    }

    n = linux_kmalloc(size, gfp);
    if (n == NULL)
        return (NULL);

    bcopy(p, n, MIN(size, linux_ksize(p)));
    linux_kfree(p);
    return (n);
}

void *
linux_vmalloc(size_t size, boolean_t zero)
{
    linux_vmalloc_hdr_t *h;
    int kmflag;

    if (size == 0 || size > ptob(physmem) / 2)
        return (NULL);

    kmflag = (size > LINUX_VMALLOC_SLEEP_MAX) ?
        (KM_NOSLEEP | KM_NORMALPRI) : KM_SLEEP;
    h = zero ? kmem_zalloc(size + sizeof (*h), kmflag) :
        kmem_alloc(size + sizeof (*h), kmflag);
    if (h == NULL)
        return (NULL);

    h->lvh_start = (uintptr_t)(h + 1);
    h->lvh_size = size;
    h->lvh_magic = LINUX_VMALLOC_MAGIC;

    mutex_enter(&linux_vmalloc_lock);
    avl_add(&linux_vmalloc_tree, h);
    mutex_exit(&linux_vmalloc_lock);

    return (h + 1);
}

void
linux_vfree(const void *p)
{
    linux_vmalloc_hdr_t *h;

    if (p == NULL)
        return;

    h = linux_vmalloc_hdr(p);

    mutex_enter(&linux_vmalloc_lock);
    avl_remove(&linux_vmalloc_tree, h);
    mutex_exit(&linux_vmalloc_lock);

    h->lvh_magic = 0;
    kmem_free(h, h->lvh_size + sizeof (*h));
}

/* True for any address inside a vmalloc allocation. */
boolean_t
linux_is_vmalloc_addr(const void *p)
{
    linux_vmalloc_hdr_t key;
    boolean_t found;

    if (ZERO_OR_NULL_PTR(p))
        return (B_FALSE);

    key.lvh_start = (uintptr_t)p;
    key.lvh_size = 1;

    mutex_enter(&linux_vmalloc_lock);
    found = (avl_find(&linux_vmalloc_tree, &key, NULL) != NULL);
    mutex_exit(&linux_vmalloc_lock);

    return (found);
}

struct kmem_cache *
linux_kmem_cache_create(const char *name, size_t size, size_t align)
{
    return (kmem_cache_create((char *)name, size,
        (align > KMEM_ALIGN) ? align : 0, NULL, NULL, NULL, NULL, NULL, 0));
}

void *
linux_kmem_cache_alloc(struct kmem_cache *cache, gfp_t gfp)
{
    void *buf = kmem_cache_alloc(cache, linux_kmflag(gfp));

    if (buf != NULL && (gfp & __GFP_ZERO))
        bzero(buf, cache->cache_bufsize);

    return (buf);
}

/*
 * Printing.  The kernel vsnprintf() lacks some conversions that UVM uses,
 * and an unknown conversion leaves its argument unconsumed, so formats are
 * rewritten first.
 */
#define LINUX_PRINTK_MAX        512

static boolean_t
linux_fmt_needs_fixup(const char *fmt)
{
    const char *p;

    for (p = fmt; (p = strchr(p, '%')) != NULL; p++) {
        for (p++; *p != '\0' && strchr("#+ -0123456789.*lhzj", *p); p++) {
            if (*p == '#' || *p == '+' || *p == ' ')
                return (B_TRUE);
        }
        if (*p == 'X' || *p == 'i')
            return (B_TRUE);
        if (*p == '\0')
            break;
    }

    return (B_FALSE);
}

/* Drop the #, + and space flags and map %X to %x and %i to %d. */
static void
linux_fmt_fixup(char *dst, const char *fmt)
{
    const char *p = fmt;

    while (*p != '\0') {
        if (*p != '%') {
            *dst++ = *p++;
            continue;
        }
        *dst++ = *p++;
        for (; *p != '\0' && strchr("#+ -0123456789.*lhzj", *p); p++) {
            if (*p != '#' && *p != '+' && *p != ' ')
                *dst++ = *p;
        }
        if (*p == 'X')
            *dst++ = 'x', p++;
        else if (*p == 'i')
            *dst++ = 'd', p++;
        else if (*p != '\0')
            *dst++ = *p++;
    }
    *dst = '\0';
}

int
linux_vsnprintf(char *buf, size_t len, const char *fmt, va_list ap)
{
    size_t flen;
    char *fixed;
    int ret;

    if (!linux_fmt_needs_fixup(fmt))
        return ((int)vsnprintf(buf, len, fmt, ap));

    flen = strlen(fmt) + 1;
    fixed = kmem_alloc(flen, KM_NOSLEEP);
    if (fixed == NULL) {
        if (len > 0)
            buf[0] = '\0';
        return (0);
    }

    linux_fmt_fixup(fixed, fmt);
    ret = (int)vsnprintf(buf, len, fixed, ap);
    kmem_free(fixed, flen);

    return (ret);
}

int
linux_snprintf(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = linux_vsnprintf(buf, len, fmt, ap);
    va_end(ap);

    return (ret);
}

int
linux_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = linux_vsnprintf(buf, INT_MAX, fmt, ap);
    va_end(ap);

    return (ret);
}

int
linux_vprintk(const char *fmt, va_list ap)
{
    int level = 4;
    char *buf;
    int ret;

    if (fmt[0] == KERN_SOH[0] && fmt[1] != '\0') {
        if (fmt[1] >= '0' && fmt[1] <= '7')
            level = fmt[1] - '0';
        fmt += 2;
    }

    buf = kmem_alloc(LINUX_PRINTK_MAX, KM_NOSLEEP);
    if (buf == NULL)
        return (0);

    ret = linux_vsnprintf(buf, LINUX_PRINTK_MAX, fmt, ap);

    /* Errors go to the console, the rest only to the system log. */
    cmn_err(CE_CONT, (level <= 3) ? "%s" : "!%s", buf);
    kmem_free(buf, LINUX_PRINTK_MAX);

    return (ret);
}

int
linux_printk(const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = linux_vprintk(fmt, ap);
    va_end(ap);

    return (ret);
}

/* Linux's default: at most 10 messages every 5 seconds. */
boolean_t
linux_ratelimit(struct ratelimit_state *rs)
{
    hrtime_t now = gethrtime();

    if (rs->begin == 0 || now - rs->begin > 5 * NANOSEC) {
        rs->begin = now;
        rs->printed = 0;
    }

    return (rs->printed++ < 10);
}

/* illumos has no driver interface for printing a stack trace. */
void
linux_dump_stack(void)
{
}

void
linux_sort(void *base, size_t num, size_t size,
    int (*cmp)(const void *, const void *))
{
    qsort(base, num, size, cmp);
}

void
linux_get_random_bytes(void *buf, size_t len)
{
    (void) random_get_pseudo_bytes(buf, len);
}

/* Sleeping. */
void
linux_usleep_range(unsigned long lo, unsigned long hi)
{
    kmutex_t mx;
    kcondvar_t cv;
    hrtime_t deadline, now, res;

    res = (hi > lo) ? USEC2NSEC(hi - lo) : 1;
    mutex_init(&mx, NULL, MUTEX_DRIVER, NULL);
    cv_init(&cv, NULL, CV_DRIVER, NULL);

    deadline = gethrtime() + USEC2NSEC(lo);
    mutex_enter(&mx);
    while ((now = gethrtime()) < deadline)
        (void) cv_timedwait_hires(&cv, &mx, deadline - now, res, 0);
    mutex_exit(&mx);

    cv_destroy(&cv);
    mutex_destroy(&mx);
}

void
linux_msleep(unsigned int ms)
{
    delay(MAX(drv_usectohz((clock_t)ms * 1000), 1));
}

/*
 * UVM calls schedule() in spin loops so that RM threads get to run.  UVM
 * threads run in the SYS class, where preempt() would not yield to a TS
 * thread, so sleep for a tick instead.
 */
void
linux_schedule(void)
{
    delay(1);
}

/* Wait queues. */
void
linux_init_waitqueue_head(wait_queue_head_t *wq)
{
    mutex_init(&wq->lock, NULL, MUTEX_DRIVER, DDI_INTR_PRI(nv_intr_pri));
    cv_init(&wq->cv, NULL, CV_DRIVER, NULL);
    wq->poll_file = NULL;
}

void
linux_wake_up_all(wait_queue_head_t *wq)
{
    struct linux_file *f;

    mutex_enter(&wq->lock);
    cv_broadcast(&wq->cv);
    f = wq->poll_file;
    mutex_exit(&wq->lock);

    if (f != NULL)
        uvm_file_queue_pollwakeup(f);
}

void
linux_poll_wait(struct linux_file *filp, wait_queue_head_t *wq,
    poll_table *pt)
{
    if (pt == NULL)
        return;

    mutex_enter(&wq->lock);
    wq->poll_file = filp;
    mutex_exit(&wq->lock);

    pt->ph = &filp->f_pollhead;
}

/*
 * Bit locks.  A waiter sleeps on one of a set of condition variables
 * picked by the word's address; the unlocker clears the bit first and then
 * broadcasts under the same mutex, so no wakeup is lost.
 */
#define LINUX_BIT_WAIT_TABLE    64

static struct {
    kmutex_t    lock;
    kcondvar_t  cv;
} linux_bit_wait[LINUX_BIT_WAIT_TABLE];

static uint_t
linux_bit_wait_index(const unsigned long *word)
{
    return ((uint_t)(((uintptr_t)word >> 3) % LINUX_BIT_WAIT_TABLE));
}

void
linux_wait_on_bit_lock(unsigned long *word, int bit)
{
    uint_t i = linux_bit_wait_index(word);
    volatile ulong_t *w = (volatile ulong_t *)&word[bit / BT_NBIPUL];
    uint_t b = (uint_t)(bit % BT_NBIPUL);

    while (atomic_set_long_excl(w, b) != 0) {
        mutex_enter(&linux_bit_wait[i].lock);
        while ((*w & (1UL << b)) != 0)
            cv_wait(&linux_bit_wait[i].cv, &linux_bit_wait[i].lock);
        mutex_exit(&linux_bit_wait[i].lock);
    }
    membar_enter();
}

void
linux_wake_up_bit(unsigned long *word, int bit)
{
    uint_t i = linux_bit_wait_index(word);

    mutex_enter(&linux_bit_wait[i].lock);
    cv_broadcast(&linux_bit_wait[i].cv);
    mutex_exit(&linux_bit_wait[i].lock);
}

/*
 * Delayed work, used only by the thrashing detector.  A timeout(9F)
 * callback moves the item to a list that a UVM thread runs, so the work
 * gets a UVM-sized stack.  untimeout() waits for a running callback, so it
 * is never called with linux_work_lock held.
 */
static kmutex_t     linux_work_lock;
static kcondvar_t   linux_work_cv;          /* new work or exit */
static kcondvar_t   linux_work_done_cv;     /* an item finished running */
static list_t       linux_work_list;
static kthread_t   *linux_work_thread;
static kt_did_t     linux_work_did;
static boolean_t    linux_work_exit;

static void
linux_work_thread_main(void *arg)
{
    struct delayed_work *dw;

    mutex_enter(&linux_work_lock);
    for (;;) {
        while (list_is_empty(&linux_work_list) && !linux_work_exit)
            cv_wait(&linux_work_cv, &linux_work_lock);

        dw = list_remove_head(&linux_work_list);
        if (dw == NULL)
            break;

        dw->queued = B_FALSE;
        dw->running = B_TRUE;
        mutex_exit(&linux_work_lock);

        dw->work.func(&dw->work);

        mutex_enter(&linux_work_lock);
        dw->running = B_FALSE;
        cv_broadcast(&linux_work_done_cv);
    }
    mutex_exit(&linux_work_lock);

    thread_exit();
}

static void
linux_work_queue_locked(struct delayed_work *dw)
{
    ASSERT(MUTEX_HELD(&linux_work_lock));

    dw->queued = B_TRUE;
    list_insert_tail(&linux_work_list, dw);
    cv_signal(&linux_work_cv);
}

static void
linux_work_timeout(void *arg)
{
    struct delayed_work *dw = arg;

    mutex_enter(&linux_work_lock);
    if (dw->armed) {
        dw->armed = B_FALSE;
        linux_work_queue_locked(dw);
    }
    mutex_exit(&linux_work_lock);
}

void
linux_init_delayed_work(struct delayed_work *dw, work_func_t fn)
{
    bzero(dw, sizeof (*dw));
    dw->work.func = fn;
    list_link_init(&dw->link);
}

boolean_t
linux_schedule_delayed_work(struct delayed_work *dw, unsigned long ticks)
{
    mutex_enter(&linux_work_lock);
    if (dw->armed || dw->queued || linux_work_exit) {
        mutex_exit(&linux_work_lock);
        return (B_FALSE);
    }

    if (ticks == 0) {
        linux_work_queue_locked(dw);
    } else {
        dw->armed = B_TRUE;
        dw->tid = timeout(linux_work_timeout, dw, (clock_t)ticks);
    }
    mutex_exit(&linux_work_lock);

    return (B_TRUE);
}

boolean_t
linux_cancel_delayed_work(struct delayed_work *dw)
{
    boolean_t pending = B_FALSE;
    timeout_id_t tid;

    mutex_enter(&linux_work_lock);
    if (dw->armed) {
        tid = dw->tid;
        mutex_exit(&linux_work_lock);
        (void) untimeout(tid);
        mutex_enter(&linux_work_lock);
        if (dw->armed) {
            dw->armed = B_FALSE;
            pending = B_TRUE;
        }
    }
    if (dw->queued) {
        list_remove(&linux_work_list, dw);
        dw->queued = B_FALSE;
        pending = B_TRUE;
    }
    mutex_exit(&linux_work_lock);

    return (pending);
}

boolean_t
linux_cancel_delayed_work_sync(struct delayed_work *dw)
{
    boolean_t pending;

    /* A running item may have rescheduled itself. */
    do {
        pending = linux_cancel_delayed_work(dw);

        mutex_enter(&linux_work_lock);
        while (dw->running)
            cv_wait(&linux_work_done_cv, &linux_work_lock);
        mutex_exit(&linux_work_lock);
    } while (dw->armed || dw->queued);

    return (pending);
}

kthread_t *
uvm_thread_create(void (*fn)(void *), void *arg)
{
    return (thread_create(NULL, UVM_THREAD_STACK_SIZE, fn, arg, 0, &p0,
        TS_RUN, minclsyspri));
}

int
uvm_kpi_init(void)
{
    uint_t i;

    tsd_create(&linux_task_key, linux_task_free);

    mutex_init(&linux_vmalloc_lock, NULL, MUTEX_DRIVER, NULL);
    avl_create(&linux_vmalloc_tree, linux_vmalloc_compare,
        sizeof (linux_vmalloc_hdr_t), offsetof(linux_vmalloc_hdr_t, lvh_link));

    for (i = 0; i < LINUX_BIT_WAIT_TABLE; i++) {
        mutex_init(&linux_bit_wait[i].lock, NULL, MUTEX_DRIVER, NULL);
        cv_init(&linux_bit_wait[i].cv, NULL, CV_DRIVER, NULL);
    }

    bzero(&linux_cpu_all_mask, sizeof (linux_cpu_all_mask));
    for (i = 0; i < (uint_t)max_ncpus && i < LINUX_NR_CPUS; i++)
        linux_cpu_all_mask.bits[i / 64] |= 1UL << (i % 64);
    bzero(&linux_node_online_map, sizeof (linux_node_online_map));
    linux_node_online_map.bits[0] = 1;

    mutex_init(&linux_work_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&linux_work_cv, NULL, CV_DRIVER, NULL);
    cv_init(&linux_work_done_cv, NULL, CV_DRIVER, NULL);
    list_create(&linux_work_list, sizeof (struct delayed_work),
        offsetof(struct delayed_work, link));
    linux_work_exit = B_FALSE;
    linux_work_thread = uvm_thread_create(linux_work_thread_main, NULL);
    linux_work_did = linux_work_thread->t_did;

    return (0);
}

/* Every UVM thread and every delayed work item must be gone by now. */
void
uvm_kpi_fini(void)
{
    uint_t i;

    mutex_enter(&linux_work_lock);
    linux_work_exit = B_TRUE;
    cv_broadcast(&linux_work_cv);
    mutex_exit(&linux_work_lock);
    thread_join(linux_work_did);

    list_destroy(&linux_work_list);
    cv_destroy(&linux_work_done_cv);
    cv_destroy(&linux_work_cv);
    mutex_destroy(&linux_work_lock);

    for (i = 0; i < LINUX_BIT_WAIT_TABLE; i++) {
        cv_destroy(&linux_bit_wait[i].cv);
        mutex_destroy(&linux_bit_wait[i].lock);
    }

    avl_destroy(&linux_vmalloc_tree);
    mutex_destroy(&linux_vmalloc_lock);

    tsd_destroy(&linux_task_key);
}
