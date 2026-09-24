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
 * illumos version of kernel-open/nvidia-uvm/nv-kthread-q.c, on the queue
 * layout of kernel-open/common/inc/nv-kthread-q-os.h.  Each queue has one
 * kernel thread with a UVM-sized stack.  nv_kthread_q_schedule_q_item() is
 * called from the interrupt top half; q_lock is initialized at the nvidia
 * interrupt priority and q_sem is a ksema_t, so that is safe.
 */

#include "nv-kthread-q.h"
#include "uvm_illumos.h"

#define NVQ_WARN(fmt, ...)                                                  \
    cmn_err(CE_WARN, "nvidia_uvm: nv_kthread_q: " fmt, ##__VA_ARGS__)

static void
nv_kthread_q_main_loop(void *args)
{
    nv_kthread_q_t *q = args;
    nv_kthread_q_item_t *q_item;
    unsigned long flags;

    for (;;) {
        down(&q->q_sem);

        if (atomic_read(&q->main_loop_should_exit))
            break;

        spin_lock_irqsave(&q->q_lock, flags);

        if (unlikely(list_empty(&q->q_list_head))) {
            spin_unlock_irqrestore(&q->q_lock, flags);
            NVQ_WARN("empty queue %p", (void *)q);
            continue;
        }

        q_item = list_first_entry(&q->q_list_head, nv_kthread_q_item_t,
            q_list_node);
        list_del_init(&q_item->q_list_node);

        spin_unlock_irqrestore(&q->q_lock, flags);

        q_item->function_to_run(q_item->function_args);
    }

    thread_exit();
}

int
nv_kthread_q_init_on_node(nv_kthread_q_t *q, const char *q_name,
    int preferred_node)
{
    struct task_struct *task;
    kthread_t *t;

    memset(q, 0, sizeof (*q));

    INIT_LIST_HEAD(&q->q_list_head);
    spin_lock_init(&q->q_lock);
    sema_init(&q->q_sem, 0);

    /* Describes the thread for set_cpus_allowed_ptr() and the join. */
    task = kmem_zalloc(sizeof (*task), KM_SLEEP);
    task->flags = PF_KTHREAD;
    (void) strlcpy(task->comm, q_name, sizeof (task->comm));

    t = uvm_thread_create(nv_kthread_q_main_loop, q);
    (void) thread_setname(t, q_name);
    task->thread = t;
    task->did = t->t_did;
    task->pid = (pid_t)t->t_did;
    q->q_kthread = task;

    return 0;
}

int
nv_kthread_q_init(nv_kthread_q_t *q, const char *qname)
{
    return nv_kthread_q_init_on_node(q, qname, NV_KTHREAD_NO_NODE);
}

void
nv_kthread_q_stop(nv_kthread_q_t *q)
{
    struct task_struct *task = q->q_kthread;

    if (unlikely(task == NULL))
        return;

    nv_kthread_q_flush(q);

    if (unlikely(!list_empty(&q->q_list_head)))
        NVQ_WARN("list not empty after flushing");

    if (likely(!atomic_read(&q->main_loop_should_exit))) {
        atomic_set(&q->main_loop_should_exit, 1);
        up(&q->q_sem);
        thread_join(task->did);
        q->q_kthread = NULL;
        kmem_free(task, sizeof (*task));
    }
}

static int
nv_kthread_q_raw_schedule(nv_kthread_q_t *q, nv_kthread_q_item_t *q_item)
{
    unsigned long flags;
    int ret = 1;

    spin_lock_irqsave(&q->q_lock, flags);

    if (likely(list_empty(&q_item->q_list_node)))
        list_add_tail(&q_item->q_list_node, &q->q_list_head);
    else
        ret = 0;

    spin_unlock_irqrestore(&q->q_lock, flags);

    if (likely(ret))
        up(&q->q_sem);

    return ret;
}

void
nv_kthread_q_item_init(nv_kthread_q_item_t *q_item,
    nv_q_func_t function_to_run, void *function_args)
{
    INIT_LIST_HEAD(&q_item->q_list_node);
    q_item->function_to_run = function_to_run;
    q_item->function_args = function_args;
}

int
nv_kthread_q_schedule_q_item(nv_kthread_q_t *q, nv_kthread_q_item_t *q_item)
{
    if (unlikely(atomic_read(&q->main_loop_should_exit))) {
        NVQ_WARN("item scheduled on stopped queue %p", (void *)q);
        return 0;
    }

    return nv_kthread_q_raw_schedule(q, q_item);
}

static void
nv_kthread_q_flush_function(void *args)
{
    complete((struct completion *)args);
}

static void
nv_kthread_q_raw_flush(nv_kthread_q_t *q)
{
    nv_kthread_q_item_t q_item;
    struct completion completion;

    init_completion(&completion);
    nv_kthread_q_item_init(&q_item, nv_kthread_q_flush_function, &completion);

    (void) nv_kthread_q_raw_schedule(q, &q_item);

    wait_for_completion(&completion);
    cv_destroy(&completion.cv);
    mutex_destroy(&completion.lock);
}

void
nv_kthread_q_flush(nv_kthread_q_t *q)
{
    if (unlikely(atomic_read(&q->main_loop_should_exit))) {
        NVQ_WARN("flush of stopped queue %p", (void *)q);
        return;
    }

    /* Twice, to cover an item that reschedules itself. */
    nv_kthread_q_raw_flush(q);
    nv_kthread_q_raw_flush(q);
}
