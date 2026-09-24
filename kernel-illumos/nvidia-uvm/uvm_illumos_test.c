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
 * Linux kernel services that only the UVM builtin tests use: radix trees,
 * remap_pfn_range(), phys_to_virt() and kthreads.
 */

#include "uvm_illumos.h"

#include <vm/as.h>
#include <vm/hat.h>

extern caddr_t hat_kpm_pfn2va(pfn_t);

/*
 * Radix trees.  Like the Linux ones they do no locking of their own.
 */
typedef struct linux_radix_node {
    avl_node_t      lrn_link;
    unsigned long   lrn_index;
    void           *lrn_item;
} linux_radix_node_t;

static int
linux_radix_compare(const void *a, const void *b)
{
    const linux_radix_node_t *x = a, *y = b;

    if (x->lrn_index < y->lrn_index)
        return (-1);
    return (x->lrn_index > y->lrn_index);
}

void
linux_radix_tree_init(struct radix_tree_root *root, gfp_t gfp)
{
    avl_create(&root->rt_tree, linux_radix_compare,
        sizeof (linux_radix_node_t), offsetof(linux_radix_node_t, lrn_link));
    root->rt_gfp = gfp;
}

static linux_radix_node_t *
linux_radix_find(struct radix_tree_root *root, unsigned long index,
    avl_index_t *where)
{
    linux_radix_node_t key;

    key.lrn_index = index;
    return (avl_find(&root->rt_tree, &key, where));
}

void *
linux_radix_tree_lookup(struct radix_tree_root *root, unsigned long index)
{
    linux_radix_node_t *n = linux_radix_find(root, index, NULL);

    return ((n != NULL) ? n->lrn_item : NULL);
}

int
linux_radix_tree_insert(struct radix_tree_root *root, unsigned long index,
    void *item)
{
    linux_radix_node_t *n;
    avl_index_t where;

    if (linux_radix_find(root, index, &where) != NULL)
        return (-EEXIST);

    n = kmem_alloc(sizeof (*n),
        (root->rt_gfp & __GFP_NOSLEEP) ? KM_NOSLEEP : KM_SLEEP);
    if (n == NULL)
        return (-ENOMEM);

    n->lrn_index = index;
    n->lrn_item = item;
    avl_insert(&root->rt_tree, n, where);

    return (0);
}

void *
linux_radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
    linux_radix_node_t *n = linux_radix_find(root, index, NULL);
    void *item;

    if (n == NULL)
        return (NULL);

    avl_remove(&root->rt_tree, n);
    item = n->lrn_item;
    kmem_free(n, sizeof (*n));

    return (item);
}

static void **
linux_radix_iter_at(struct radix_tree_iter *iter, linux_radix_node_t *n)
{
    if (n == NULL)
        return (NULL);

    iter->index = n->lrn_index;
    return (&n->lrn_item);
}

void **
linux_radix_tree_iter_first(struct radix_tree_root *root,
    struct radix_tree_iter *iter, unsigned long start)
{
    linux_radix_node_t *n;
    avl_index_t where;

    n = linux_radix_find(root, start, &where);
    if (n == NULL)
        n = avl_nearest(&root->rt_tree, where, AVL_AFTER);

    return (linux_radix_iter_at(iter, n));
}

/* The entry after iter->index, which the caller may have deleted. */
void **
linux_radix_tree_iter_next(struct radix_tree_root *root,
    struct radix_tree_iter *iter)
{
    linux_radix_node_t *n;
    avl_index_t where;

    n = linux_radix_find(root, iter->index, &where);
    if (n != NULL)
        n = AVL_NEXT(&root->rt_tree, n);
    else
        n = avl_nearest(&root->rt_tree, where, AVL_AFTER);

    return (linux_radix_iter_at(iter, n));
}

/*
 * Maps memory pages into a seg_nvuvm segment of a test file, from its mmap
 * handler, which runs with the AS write lock held.  The translations are
 * locked, as vm_insert_page() makes them, because the segment unmaps with
 * HAT_UNLOAD_UNLOCK.  The pages must stay allocated until the segment is
 * gone; the test file frees them when it is released, and each segment
 * holds the file.
 */
int
linux_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn, unsigned long size, pgprot_t prot)
{
    struct as *as = (struct as *)vma->vm_mm;
    uint_t hprot = (prot.prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) |
        PROT_USER;
    unsigned long i, npages;

    if (as == NULL || !AS_WRITE_HELD(as))
        return (-EINVAL);

    if (size == 0 || size > ULONG_MAX - PAGEOFFSET)
        return (-EINVAL);
    size = P2ROUNDUP(size, PAGESIZE);
    npages = size >> PAGESHIFT;

    if ((addr & PAGEOFFSET) != 0 || addr < vma->vm_start ||
        addr >= vma->vm_end || size > vma->vm_end - addr ||
        pfn > (unsigned long)physmax || npages > physmax + 1 - pfn)
        return (-EINVAL);

    for (i = 0; i < npages; i++) {
        caddr_t va = (caddr_t)(addr + ptob(i));

        if (page_numtopp_nolock((pfn_t)(pfn + i)) == NULL)
            return (-EINVAL);
        /* A second locked load would leak a page table lock count. */
        if (hat_getpfnum(as->a_hat, va) != PFN_INVALID)
            return (-EBUSY);
    }

    for (i = 0; i < npages; i++) {
        hat_devload(as->a_hat, (caddr_t)(addr + ptob(i)), PAGESIZE,
            (pfn_t)(pfn + i), hprot, HAT_LOAD_LOCK);
    }

    vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND;

    return (0);
}

/* Only memory pages, which segkpm maps, have a kernel address. */
void *
linux_phys_to_virt(phys_addr_t pa)
{
    pfn_t pfn = (pfn_t)(pa >> PAGESHIFT);

    VERIFY3U(pfn, <=, physmax);
    VERIFY(page_numtopp_nolock(pfn) != NULL);

    return (hat_kpm_pfn2va(pfn) + (pa & PAGEOFFSET));
}

/*
 * kthreads.  The task_struct belongs to the kthread until kthread_stop()
 * has joined the thread.
 */
struct linux_kthread {
    kmutex_t            lk_lock;
    kcondvar_t          lk_cv;
    boolean_t           lk_go;
    boolean_t           lk_stop;
    int               (*lk_fn)(void *);
    void               *lk_arg;
    int                 lk_result;
    kt_did_t            lk_did;
    struct task_struct *lk_task;
};

static void
linux_kthread_main(void *arg)
{
    struct linux_kthread *kt = arg;

    /* Wait until the creator has recorded the thread id. */
    mutex_enter(&kt->lk_lock);
    while (!kt->lk_go)
        cv_wait(&kt->lk_cv, &kt->lk_lock);
    mutex_exit(&kt->lk_lock);

    linux_task_adopt(kt->lk_task);
    kt->lk_result = kt->lk_fn(kt->lk_arg);
    linux_task_disown();

    thread_exit();
}

struct task_struct *
linux_kthread_run(int (*fn)(void *), void *arg)
{
    struct linux_kthread *kt = kmem_zalloc(sizeof (*kt), KM_SLEEP);
    struct task_struct *t = kmem_zalloc(sizeof (*t), KM_SLEEP);

    mutex_init(&kt->lk_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&kt->lk_cv, NULL, CV_DRIVER, NULL);
    kt->lk_fn = fn;
    kt->lk_arg = arg;
    kt->lk_task = t;
    t->kthread = kt;

    kt->lk_did = uvm_thread_create(linux_kthread_main, kt)->t_did;

    mutex_enter(&kt->lk_lock);
    kt->lk_go = B_TRUE;
    cv_broadcast(&kt->lk_cv);
    mutex_exit(&kt->lk_lock);

    return (t);
}

int
linux_kthread_stop(struct task_struct *t)
{
    struct linux_kthread *kt = t->kthread;
    int result;

    VERIFY(kt != NULL && kt->lk_task == t);

    mutex_enter(&kt->lk_lock);
    kt->lk_stop = B_TRUE;
    mutex_exit(&kt->lk_lock);

    thread_join(kt->lk_did);

    result = kt->lk_result;
    cv_destroy(&kt->lk_cv);
    mutex_destroy(&kt->lk_lock);
    kmem_free(kt, sizeof (*kt));
    linux_task_destroy(t);

    return (result);
}

bool
linux_kthread_should_stop(void)
{
    struct linux_kthread *kt = linux_current()->kthread;
    boolean_t stop;

    if (kt == NULL)
        return (false);

    mutex_enter(&kt->lk_lock);
    stop = kt->lk_stop;
    mutex_exit(&kt->lk_lock);

    return (stop);
}
