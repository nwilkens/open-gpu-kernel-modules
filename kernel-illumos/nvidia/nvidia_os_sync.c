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
 * Locks, wait queues and work queues for RM.
 */

#include "nv-illumos.h"

#include <sys/disp.h>

/*
 * RM mutexes and semaphores may be released by a thread other than the one
 * that acquired them (they are Linux semaphores), so both map to ksema_t.
 */
NV_STATUS NV_API_CALL os_alloc_mutex(void **ppMutex)
{
    ksema_t *sema;

    sema = kmem_zalloc(sizeof (*sema), KM_SLEEP);
    sema_init(sema, 1, NULL, SEMA_DRIVER, NULL);
    *ppMutex = sema;

    return NV_OK;
}

void NV_API_CALL os_free_mutex(void *pMutex)
{
    ksema_t *sema = pMutex;

    if (sema != NULL)
    {
        sema_destroy(sema);
        kmem_free(sema, sizeof (*sema));
    }
}

NV_STATUS NV_API_CALL os_acquire_mutex(void *pMutex)
{
    if (!nv_may_sleep())
        return NV_ERR_INVALID_REQUEST;

    sema_p((ksema_t *)pMutex);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_mutex(void *pMutex)
{
    if (!nv_may_sleep())
        return NV_ERR_INVALID_REQUEST;

    if (sema_tryp((ksema_t *)pMutex) == 0)
        return NV_ERR_TIMEOUT_RETRY;

    return NV_OK;
}

void NV_API_CALL os_release_mutex(void *pMutex)
{
    sema_v((ksema_t *)pMutex);
}

void* NV_API_CALL os_alloc_semaphore(NvU32 initialValue)
{
    ksema_t *sema;

    sema = kmem_zalloc(sizeof (*sema), KM_NOSLEEP);
    if (sema == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate semaphore!\n");
        return NULL;
    }

    sema_init(sema, initialValue, NULL, SEMA_DRIVER, NULL);
    return sema;
}

void NV_API_CALL os_free_semaphore(void *pSema)
{
    ksema_t *sema = pSema;

    sema_destroy(sema);
    kmem_free(sema, sizeof (*sema));
}

NV_STATUS NV_API_CALL os_acquire_semaphore(void *pSema)
{
    if (!nv_may_sleep())
        return NV_ERR_INVALID_REQUEST;

    sema_p((ksema_t *)pSema);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_semaphore(void *pSema)
{
    if (sema_tryp((ksema_t *)pSema) == 0)
        return NV_ERR_TIMEOUT_RETRY;

    return NV_OK;
}

NV_STATUS NV_API_CALL os_release_semaphore(void *pSema)
{
    sema_v((ksema_t *)pSema);
    return NV_OK;
}

void* NV_API_CALL os_alloc_rwlock(void)
{
    krwlock_t *rw;

    rw = kmem_zalloc(sizeof (*rw), KM_NOSLEEP);
    if (rw == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate rwlock!\n");
        return NULL;
    }

    rw_init(rw, NULL, RW_DRIVER, NULL);
    return rw;
}

void NV_API_CALL os_free_rwlock(void *pRwLock)
{
    krwlock_t *rw = pRwLock;

    rw_destroy(rw);
    kmem_free(rw, sizeof (*rw));
}

NV_STATUS NV_API_CALL os_acquire_rwlock_read(void *pRwLock)
{
    if (!nv_may_sleep())
        return NV_ERR_INVALID_REQUEST;

    rw_enter((krwlock_t *)pRwLock, RW_READER);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_acquire_rwlock_write(void *pRwLock)
{
    if (!nv_may_sleep())
        return NV_ERR_INVALID_REQUEST;

    rw_enter((krwlock_t *)pRwLock, RW_WRITER);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_rwlock_read(void *pRwLock)
{
    if (rw_tryenter((krwlock_t *)pRwLock, RW_READER) == 0)
        return NV_ERR_TIMEOUT_RETRY;

    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_rwlock_write(void *pRwLock)
{
    if (rw_tryenter((krwlock_t *)pRwLock, RW_WRITER) == 0)
        return NV_ERR_TIMEOUT_RETRY;

    return NV_OK;
}

void NV_API_CALL os_release_rwlock_read(void *pRwLock)
{
    rw_exit((krwlock_t *)pRwLock);
}

void NV_API_CALL os_release_rwlock_write(void *pRwLock)
{
    rw_exit((krwlock_t *)pRwLock);
}

/*
 * RM spinlocks are taken from the top-half interrupt handler, which runs as a
 * low-level interrupt thread at nv_intr_pri.  An adaptive mutex initialized
 * at that priority is the DDI equivalent of spin_lock_irqsave(); holders may
 * allocate with KM_NOSLEEP, which a true spin mutex above LOCK_LEVEL could not.
 */
typedef struct os_spinlock_s {
    kmutex_t lock;
} os_spinlock_t;

NV_STATUS NV_API_CALL os_alloc_spinlock(void **ppSpinlock)
{
    os_spinlock_t *sl;

    sl = kmem_zalloc(sizeof (*sl), KM_NOSLEEP);
    if (sl == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate spinlock!\n");
        return NV_ERR_NO_MEMORY;
    }

    mutex_init(&sl->lock, NULL, MUTEX_DRIVER, DDI_INTR_PRI(nv_intr_pri));
    *ppSpinlock = sl;
    return NV_OK;
}

void NV_API_CALL os_free_spinlock(void *pSpinlock)
{
    os_spinlock_t *sl = pSpinlock;

    mutex_destroy(&sl->lock);
    kmem_free(sl, sizeof (*sl));
}

NvU64 NV_API_CALL os_acquire_spinlock(void *pSpinlock)
{
    os_spinlock_t *sl = pSpinlock;

    mutex_enter(&sl->lock);
    return 0;
}

void NV_API_CALL os_release_spinlock(void *pSpinlock, NvU64 oldIrql)
{
    os_spinlock_t *sl = pSpinlock;

    mutex_exit(&sl->lock);
}

NvBool NV_API_CALL os_semaphore_may_sleep(void)
{
    return nv_may_sleep();
}

NvBool NV_API_CALL os_is_isr(void)
{
    return (servicing_interrupt() != 0);
}

typedef struct {
    void *data;
} os_queue_data_t;

static void os_execute_work_item(void *arg)
{
    os_queue_data_t *oqd = arg;
    nvidia_stack_t *sp = NULL;
    void *data = oqd->data;

    kmem_free(oqd, sizeof (*oqd));

    if (nv_stack_alloc(&sp) != 0)
        return;

    rm_execute_work_item(sp, data);

    nv_stack_free(sp);
}

/* A single thread keeps work items in FIFO order, like nv_kthread_q. */
int
nv_queue_init(struct os_work_queue *q, const char *name)
{
    q->is_unload_flush_ongoing = NV_FALSE;
    q->tq = taskq_create(name, 1, minclsyspri, 4, INT_MAX, TASKQ_PREPOPULATE);
    return (q->tq == NULL) ? ENOMEM : 0;
}

void
nv_queue_fini(struct os_work_queue *q)
{
    if (q->tq != NULL)
    {
        taskq_destroy(q->tq);
        q->tq = NULL;
    }
}

NV_STATUS NV_API_CALL os_queue_work_item(struct os_work_queue *queue, void *data)
{
    struct os_work_queue *q = (queue != NULL) ? queue : &nv_global_queue;
    os_queue_data_t *oqd;

    if (q->tq == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: queue is not enabled\n");
        return NV_ERR_NOT_READY;
    }

    oqd = kmem_alloc(sizeof (*oqd), KM_NOSLEEP);
    if (oqd == NULL)
        return NV_ERR_NO_MEMORY;

    oqd->data = data;

    if (taskq_dispatch(q->tq, os_execute_work_item, oqd, TQ_NOSLEEP) ==
        TASKQID_INVALID)
    {
        kmem_free(oqd, sizeof (*oqd));
        return NV_ERR_NO_MEMORY;
    }

    return NV_OK;
}

NV_STATUS NV_API_CALL os_flush_work_queue(struct os_work_queue *queue, NvBool is_unload)
{
    struct os_work_queue *q = (queue != NULL) ? queue : &nv_global_queue;

    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS,
                  "NVRM: os_flush_work_queue: attempted to execute passive"
                  "work from an atomic or interrupt context.\n");
        return NV_ERR_ILLEGAL_ACTION;
    }

    q->is_unload_flush_ongoing = is_unload;
    if (q->tq != NULL)
        taskq_wait(q->tq);
    q->is_unload_flush_ongoing = NV_FALSE;

    return NV_OK;
}

NvBool NV_API_CALL os_is_queue_flush_ongoing(struct os_work_queue *queue)
{
    struct os_work_queue *q = (queue != NULL) ? queue : &nv_global_queue;

    return q->is_unload_flush_ongoing;
}

NV_STATUS NV_API_CALL os_alloc_wait_queue(os_wait_queue **wq)
{
    os_wait_queue *q;

    q = kmem_zalloc(sizeof (*q), KM_SLEEP);
    mutex_init(&q->lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&q->cv, NULL, CV_DRIVER, NULL);
    q->done = NV_FALSE;
    *wq = q;

    return NV_OK;
}

void NV_API_CALL os_free_wait_queue(os_wait_queue *wq)
{
    cv_destroy(&wq->cv);
    mutex_destroy(&wq->lock);
    kmem_free(wq, sizeof (*wq));
}

void NV_API_CALL os_wait_uninterruptible(os_wait_queue *wq)
{
    mutex_enter(&wq->lock);
    while (!wq->done)
        cv_wait(&wq->cv, &wq->lock);
    mutex_exit(&wq->lock);
}

void NV_API_CALL os_wait_interruptible(os_wait_queue *wq)
{
    mutex_enter(&wq->lock);
    while (!wq->done)
    {
        if (cv_wait_sig(&wq->cv, &wq->lock) == 0)
            break;
    }
    mutex_exit(&wq->lock);
}

void NV_API_CALL os_wake_up(os_wait_queue *wq)
{
    mutex_enter(&wq->lock);
    wq->done = NV_TRUE;
    cv_broadcast(&wq->cv);
    mutex_exit(&wq->lock);
}
