/*
 * SPDX-FileCopyrightText: Copyright (c) 2016-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * Work queue (port of nv-kthread-q.c) and reader/writer semaphore for the
 * illumos nvidia-modeset interface layer.
 */

#include "nvidia_modeset_illumos.h"

#include <sys/ddi.h>
#include <sys/sunddi.h>

/*
 * Each queue is served by one long-running task on a single-threaded
 * ddi_taskq, which gives it a dedicated kernel thread like the Linux
 * kthread.
 */
static void nvkms_q_main_loop(void *args)
{
    nvkms_q_t *q = args;
    nvkms_q_item_t *q_item;

    mutex_enter(&q->q_lock);

    for (;;) {
        while (list_is_empty(&q->q_list) && !q->main_loop_should_exit) {
            cv_wait(&q->q_cv, &q->q_lock);
        }

        q_item = list_remove_head(&q->q_list);
        if (q_item == NULL) {
            break;
        }

        mutex_exit(&q->q_lock);

        q_item->function_to_run(q_item->function_args);

        mutex_enter(&q->q_lock);
    }

    mutex_exit(&q->q_lock);
}

int nvkms_q_init(nvkms_q_t *q, const char *qname)
{
    bzero(q, sizeof(*q));

    mutex_init(&q->q_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&q->q_cv, NULL, CV_DRIVER, NULL);
    list_create(&q->q_list, sizeof(nvkms_q_item_t),
                offsetof(nvkms_q_item_t, q_list_node));

    q->q_taskq = ddi_taskq_create(NULL, qname, 1, TASKQ_DEFAULTPRI, 0);
    if (q->q_taskq == NULL) {
        goto fail;
    }

    if (ddi_taskq_dispatch(q->q_taskq, nvkms_q_main_loop, q,
                           DDI_SLEEP) != DDI_SUCCESS) {
        ddi_taskq_destroy(q->q_taskq);
        q->q_taskq = NULL;
        goto fail;
    }

    return 0;

fail:
    list_destroy(&q->q_list);
    cv_destroy(&q->q_cv);
    mutex_destroy(&q->q_lock);
    return ENOMEM;
}

/* Returns non-zero if the item was queued, zero if it was already pending. */
static int nvkms_q_raw_schedule(nvkms_q_t *q, nvkms_q_item_t *q_item)
{
    int ret = 1;

    mutex_enter(&q->q_lock);

    if (!list_link_active(&q_item->q_list_node)) {
        list_insert_tail(&q->q_list, q_item);
        cv_signal(&q->q_cv);
    } else {
        ret = 0;
    }

    mutex_exit(&q->q_lock);

    return ret;
}

void nvkms_q_item_init(nvkms_q_item_t *q_item, nvkms_q_func_t function_to_run,
                       void *function_args)
{
    list_link_init(&q_item->q_list_node);
    q_item->function_to_run = function_to_run;
    q_item->function_args = function_args;
}

int nvkms_q_schedule(nvkms_q_t *q, nvkms_q_item_t *q_item)
{
    if (q->main_loop_should_exit) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX
            "work item scheduled on stopped queue %p", (void *)q);
        return 0;
    }

    return nvkms_q_raw_schedule(q, q_item);
}

typedef struct nvkms_q_completion {
    kmutex_t    lock;
    kcondvar_t  cv;
    NvBool      done;
} nvkms_q_completion_t;

static void nvkms_q_flush_function(void *args)
{
    nvkms_q_completion_t *c = args;

    mutex_enter(&c->lock);
    c->done = NV_TRUE;
    cv_broadcast(&c->cv);
    mutex_exit(&c->lock);
}

static void nvkms_q_raw_flush(nvkms_q_t *q)
{
    nvkms_q_item_t q_item;
    nvkms_q_completion_t c;

    mutex_init(&c.lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&c.cv, NULL, CV_DRIVER, NULL);
    c.done = NV_FALSE;

    nvkms_q_item_init(&q_item, nvkms_q_flush_function, &c);
    (void) nvkms_q_raw_schedule(q, &q_item);

    // Once the flush item has run, everything queued before it has run too.
    mutex_enter(&c.lock);
    while (!c.done) {
        cv_wait(&c.cv, &c.lock);
    }
    mutex_exit(&c.lock);

    cv_destroy(&c.cv);
    mutex_destroy(&c.lock);
}

void nvkms_q_flush(nvkms_q_t *q)
{
    if (q->main_loop_should_exit) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX
            "flush of stopped queue %p", (void *)q);
        return;
    }

    // Flush twice to cover a q_item that reschedules itself.
    nvkms_q_raw_flush(q);
    nvkms_q_raw_flush(q);
}

void nvkms_q_stop(nvkms_q_t *q)
{
    if (q->q_taskq == NULL) {
        return;
    }

    nvkms_q_flush(q);

    mutex_enter(&q->q_lock);
    if (!list_is_empty(&q->q_list)) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX
            "queue %p not empty after flushing", (void *)q);
    }
    q->main_loop_should_exit = NV_TRUE;
    cv_broadcast(&q->q_cv);
    mutex_exit(&q->q_lock);

    /* Waits for the main loop, which drains anything still queued. */
    ddi_taskq_destroy(q->q_taskq);
    q->q_taskq = NULL;

    list_destroy(&q->q_list);
    cv_destroy(&q->q_cv);
    mutex_destroy(&q->q_lock);
}

/*************************************************************************
 * Reader/writer semaphore.  Waiting writers block new readers.
 *************************************************************************/

void nvkms_rwsem_init(nvkms_rwsem_t *sem)
{
    bzero(sem, sizeof(*sem));
    mutex_init(&sem->lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&sem->cv, NULL, CV_DRIVER, NULL);
}

void nvkms_rwsem_destroy(nvkms_rwsem_t *sem)
{
    cv_destroy(&sem->cv);
    mutex_destroy(&sem->lock);
}

static NvBool nvkms_rwsem_read_blocked(const nvkms_rwsem_t *sem)
{
    return sem->writer || sem->writers_waiting != 0;
}

void nvkms_rwsem_down_read(nvkms_rwsem_t *sem)
{
    mutex_enter(&sem->lock);
    while (nvkms_rwsem_read_blocked(sem)) {
        cv_wait(&sem->cv, &sem->lock);
    }
    sem->readers++;
    mutex_exit(&sem->lock);
}

/* Returns 0, or EINTR if a signal arrived while waiting. */
int nvkms_rwsem_down_read_sig(nvkms_rwsem_t *sem)
{
    mutex_enter(&sem->lock);
    while (nvkms_rwsem_read_blocked(sem)) {
        if (cv_wait_sig(&sem->cv, &sem->lock) == 0) {
            mutex_exit(&sem->lock);
            return EINTR;
        }
    }
    sem->readers++;
    mutex_exit(&sem->lock);

    return 0;
}

NvBool nvkms_rwsem_down_read_trylock(nvkms_rwsem_t *sem)
{
    NvBool ret = NV_FALSE;

    mutex_enter(&sem->lock);
    if (!nvkms_rwsem_read_blocked(sem)) {
        sem->readers++;
        ret = NV_TRUE;
    }
    mutex_exit(&sem->lock);

    return ret;
}

void nvkms_rwsem_up_read(nvkms_rwsem_t *sem)
{
    mutex_enter(&sem->lock);
    ASSERT(sem->readers > 0);
    if (--sem->readers == 0) {
        cv_broadcast(&sem->cv);
    }
    mutex_exit(&sem->lock);
}

void nvkms_rwsem_down_write(nvkms_rwsem_t *sem)
{
    mutex_enter(&sem->lock);
    sem->writers_waiting++;
    while (sem->writer || sem->readers != 0) {
        cv_wait(&sem->cv, &sem->lock);
    }
    sem->writers_waiting--;
    sem->writer = NV_TRUE;
    mutex_exit(&sem->lock);
}

void nvkms_rwsem_up_write(nvkms_rwsem_t *sem)
{
    mutex_enter(&sem->lock);
    ASSERT(sem->writer);
    sem->writer = NV_FALSE;
    cv_broadcast(&sem->cv);
    mutex_exit(&sem->lock);
}
