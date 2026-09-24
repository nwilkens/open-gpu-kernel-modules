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
 * Charging UVM memory to zones and projects.  UVM pages never page out, so
 * they count as locked memory, against project.max-locked-memory and
 * zone.max-locked-memory like mlock() and umem_lockmemory() do.
 *
 * A managed range is charged in full when it is mapped: UVM moves its pages
 * between CPU and GPU from threads that do not belong to the owner, so the
 * charge cannot follow individual pages.  Other allocations UVM marks
 * __GFP_ACCOUNT, such as semaphore pools, are charged per allocation to the
 * process whose mm is the active memcg.  User GPU page tables are charged
 * per allocation to the owner of the VA space: the project and zone of the
 * process that opened its file.
 *
 * A charge holds its project and zone, so it can be released from any
 * thread after the process is gone.  Pages kept because a device may still
 * reach them are charged to the system for good instead, since a charge
 * that never goes away would keep its zone from halting.
 */

#include "uvm_illumos.h"
#include "uvm_illumos_mem.h"

#include <sys/avl.h>
#include <sys/project.h>
#include <sys/task.h>
#include <sys/zone.h>
#include <sys/rctl.h>

struct uvm_charge {
    kproject_t     *uc_proj;
    zone_t         *uc_zone;
    size_t          uc_bytes;
    uint_t          uc_refs;            /* uvm_acct_lock */

    /* A charge for an allocation, keyed by its first page. */
    avl_node_t      uc_page_link;
    struct page    *uc_page;

    /* A charge waiting for its owner's release. */
    list_node_t     uc_defer_link;
    const void     *uc_owner;
};

struct uvm_acct_owner {
    kproject_t     *uo_proj;
    zone_t         *uo_zone;
};

static kmutex_t     uvm_acct_lock;
static avl_tree_t   uvm_acct_pages;
static list_t       uvm_acct_deferred;
static uint_t       uvm_acct_live;

static int
uvm_acct_page_compare(const void *a, const void *b)
{
    const uvm_charge_t *x = a, *y = b;

    if (x->uc_page == y->uc_page)
        return (0);
    return ((uintptr_t)x->uc_page < (uintptr_t)y->uc_page ? -1 : 1);
}

/*
 * Hold the project and zone of the current process, and charge bytes of
 * locked memory to them unless bytes is 0.  Returns an errno.
 */
static int
uvm_acct_curproc(size_t bytes, kproject_t **projp, zone_t **zonep)
{
    proc_t *p = curproc;
    kproject_t *proj;
    zone_t *zone;

    if (p == &p0 || p->p_as == &kas || servicing_interrupt())
        return (EINVAL);

    /* A process stays in its zone while it runs this. */
    zone = p->p_zone;
    zone_hold(zone);

    mutex_enter(&p->p_lock);
    proj = p->p_task->tk_proj;
    if (proj->kpj_zone != zone ||
        (bytes != 0 && rctl_incr_locked_mem(p, proj, bytes, 0) != 0)) {
        mutex_exit(&p->p_lock);
        zone_rele(zone);
        return (EAGAIN);
    }
    *projp = project_hold(proj);
    mutex_exit(&p->p_lock);

    *zonep = zone;
    return (0);
}

/*
 * rctl_incr_locked_mem() without a process: it tests the project of the
 * process it is given, not proj.  Both controls always deny, so their
 * cached values are the enforced limits; their other actions are skipped.
 */
static int
uvm_acct_incr(kproject_t *proj, rctl_qty_t bytes, boolean_t enforce)
{
    kproject_data_t *d = &proj->kpj_data;
    zone_t *zone = proj->kpj_zone;
    int err = 0;

    mutex_enter(&zone->zone_mem_lock);
    if (enforce &&
        (d->kpd_locked_mem + bytes < d->kpd_locked_mem ||
        d->kpd_locked_mem + bytes > d->kpd_locked_mem_ctl ||
        zone->zone_locked_mem + bytes < zone->zone_locked_mem ||
        zone->zone_locked_mem + bytes > zone->zone_locked_mem_ctl)) {
        err = EAGAIN;
    } else {
        d->kpd_locked_mem += bytes;
        zone->zone_locked_mem += bytes;
    }
    mutex_exit(&zone->zone_mem_lock);

    return (err);
}

static uvm_charge_t *
uvm_charge_new(kproject_t *proj, zone_t *zone, size_t bytes)
{
    uvm_charge_t *c = kmem_zalloc(sizeof (*c), KM_SLEEP);

    c->uc_proj = proj;
    c->uc_zone = zone;
    c->uc_bytes = bytes;
    c->uc_refs = 1;
    atomic_inc_uint(&uvm_acct_live);

    return (c);
}

/*
 * Charge bytes of locked memory to the project and zone of the current
 * process.  Returns NULL with *errp set when a resource control denies it.
 */
uvm_charge_t *
uvm_charge_take(size_t bytes, int *errp)
{
    kproject_t *proj;
    zone_t *zone;

    *errp = uvm_acct_curproc(bytes, &proj, &zone);
    if (*errp != 0)
        return (NULL);

    return (uvm_charge_new(proj, zone, bytes));
}

/*
 * Charge bytes to an owner from any thread.  A thread of the owner's
 * project goes through rctl_incr_locked_mem(), so that the controls' other
 * actions apply to it.
 */
uvm_charge_t *
uvm_charge_take_owner(const uvm_acct_owner_t *o, size_t bytes, int *errp)
{
    proc_t *p = curproc;
    kproject_t *proj = o->uo_proj;
    boolean_t local = B_FALSE;
    int err = 0;

    if (servicing_interrupt()) {
        *errp = EINVAL;
        return (NULL);
    }

    if (p != &p0 && p->p_as != &kas) {
        mutex_enter(&p->p_lock);
        if (p->p_task->tk_proj == proj) {
            local = B_TRUE;
            err = rctl_incr_locked_mem(p, proj, bytes, 0);
        }
        mutex_exit(&p->p_lock);
    }
    if (!local)
        err = uvm_acct_incr(proj, bytes, B_TRUE);
    if (err != 0) {
        *errp = EAGAIN;
        return (NULL);
    }

    zone_hold(o->uo_zone);
    *errp = 0;
    return (uvm_charge_new(project_hold(proj), o->uo_zone, bytes));
}

uvm_acct_owner_t *
uvm_acct_owner_create(int *errp)
{
    uvm_acct_owner_t *o = kmem_zalloc(sizeof (*o), KM_SLEEP);

    *errp = uvm_acct_curproc(0, &o->uo_proj, &o->uo_zone);
    if (*errp != 0) {
        kmem_free(o, sizeof (*o));
        return (NULL);
    }
    return (o);
}

void
uvm_acct_owner_free(uvm_acct_owner_t *o)
{
    project_rele(o->uo_proj);
    zone_rele(o->uo_zone);
    kmem_free(o, sizeof (*o));
}

/* Pages that can never be freed are charged to the global zone's project 0. */
void
uvm_acct_quarantine(size_t bytes)
{
    (void) uvm_acct_incr(proj0p, bytes, B_FALSE);
}

void
uvm_charge_hold(uvm_charge_t *c)
{
    mutex_enter(&uvm_acct_lock);
    VERIFY(c->uc_refs > 0);
    c->uc_refs++;
    mutex_exit(&uvm_acct_lock);
}

static void
uvm_charge_free(uvm_charge_t *c)
{
    rctl_decr_locked_mem(NULL, c->uc_proj, c->uc_bytes, 0);
    project_rele(c->uc_proj);
    zone_rele(c->uc_zone);
    kmem_free(c, sizeof (*c));
    atomic_dec_uint(&uvm_acct_live);
}

void
uvm_charge_rele(uvm_charge_t *c)
{
    boolean_t last;

    mutex_enter(&uvm_acct_lock);
    VERIFY(c->uc_refs > 0);
    last = (--c->uc_refs == 0);
    mutex_exit(&uvm_acct_lock);

    if (last)
        uvm_charge_free(c);
}

/*
 * Drop a reference, but keep the charge until uvm_charge_release_owner()
 * if this was the last one.  Used when the memory may outlive the
 * reference, as a UVM range zombified by process exit does.
 */
void
uvm_charge_rele_deferred(uvm_charge_t *c, const void *owner)
{
    mutex_enter(&uvm_acct_lock);
    VERIFY(c->uc_refs > 0);
    if (--c->uc_refs == 0) {
        c->uc_refs = 1;
        c->uc_owner = owner;
        list_insert_tail(&uvm_acct_deferred, c);
    }
    mutex_exit(&uvm_acct_lock);
}

void
uvm_charge_release_owner(const void *owner)
{
    list_t done;
    uvm_charge_t *c, *next;

    list_create(&done, sizeof (uvm_charge_t),
        offsetof(uvm_charge_t, uc_defer_link));

    mutex_enter(&uvm_acct_lock);
    for (c = list_head(&uvm_acct_deferred); c != NULL; c = next) {
        next = list_next(&uvm_acct_deferred, c);
        if (c->uc_owner == owner) {
            list_remove(&uvm_acct_deferred, c);
            list_insert_tail(&done, c);
        }
    }
    mutex_exit(&uvm_acct_lock);

    while ((c = list_remove_head(&done)) != NULL)
        uvm_charge_free(c);
    list_destroy(&done);
}

/* Record the charge of an allocation that starts at pp. */
void
uvm_charge_page_add(uvm_charge_t *c, struct page *pp)
{
    c->uc_page = pp;
    mutex_enter(&uvm_acct_lock);
    avl_add(&uvm_acct_pages, c);
    mutex_exit(&uvm_acct_lock);
}

uvm_charge_t *
uvm_charge_page_remove(struct page *pp)
{
    uvm_charge_t key, *c;

    key.uc_page = pp;
    mutex_enter(&uvm_acct_lock);
    c = avl_find(&uvm_acct_pages, &key, NULL);
    if (c != NULL)
        avl_remove(&uvm_acct_pages, c);
    mutex_exit(&uvm_acct_lock);

    return (c);
}

/*
 * The memcg compatibility: the "cgroup" of an mm is the mm itself, and the
 * active one lives in thread-specific data.
 */
struct mem_cgroup *
linux_get_mem_cgroup_from_mm(struct mm_struct *mm)
{
    return ((struct mem_cgroup *)mm);
}

struct mem_cgroup *
linux_set_active_memcg(struct mem_cgroup *memcg)
{
    return (uvm_memcg_swap(memcg));
}

void
uvm_acct_init(void)
{
    mutex_init(&uvm_acct_lock, NULL, MUTEX_DRIVER, NULL);
    avl_create(&uvm_acct_pages, uvm_acct_page_compare, sizeof (uvm_charge_t),
        offsetof(uvm_charge_t, uc_page_link));
    list_create(&uvm_acct_deferred, sizeof (uvm_charge_t),
        offsetof(uvm_charge_t, uc_defer_link));
}

void
uvm_acct_fini(void)
{
    /* Leaked charges are still linked, so their lists stay. */
    if (uvm_acct_live != 0) {
        cmn_err(CE_WARN, "nvidia_uvm: %u charges leaked", uvm_acct_live);
        return;
    }

    list_destroy(&uvm_acct_deferred);
    avl_destroy(&uvm_acct_pages);
    mutex_destroy(&uvm_acct_lock);
}
