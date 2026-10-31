/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Carlos Neira
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

#ifndef NVIDIA_UMEM_TEST_KERNEL_H
#define NVIDIA_UMEM_TEST_KERNEL_H

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef unsigned int uint_t;
typedef char *caddr_t;
typedef int NvBool;
typedef int NV_STATUS;
typedef int boolean_t;
typedef struct { int unused; } kthread_t;
typedef pthread_mutex_t kmutex_t;
typedef pthread_cond_t kcondvar_t;
typedef struct { int unused; } taskq_ent_t;
typedef struct { int unused; } taskq_t;
typedef struct as { int unused; } as_t;
typedef struct ddi_umem_cookie { struct as *asp; } *ddi_umem_cookie_t;
static atomic_uint umem_test_cv_wait_calls;
static atomic_uint umem_test_error_logs;
static char umem_test_last_error[256];

#define NV_API_CALL
#define NV_TRUE 1
#define NV_FALSE 0
#define B_TRUE 1
#define B_FALSE 0
#define NV_OK 0
#define NV_ERR_INVALID_ARGUMENT 1
#define NV_ERR_INVALID_STATE 2
#define NV_ERR_NO_MEMORY 3
#define NV_ERR_BUSY_RETRY 4
#define NV_DBG_ERRORS 0
#define KM_NOSLEEP 0
#define MUTEX_DRIVER 0
#define CV_DRIVER 0
#define TASKQ_PREPOPULATE 0
#define minclsyspri 0
#define UMEM_CALLBACK_VERSION 1
#define curthread ((kthread_t *)(uintptr_t)pthread_self())
#define ASSERT(x) assert(x)
#define VERIFY(x) assert(x)
#define MUTEX_HELD(x) 1

typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} list_node_t;
typedef struct list {
    list_node_t head;
    size_t offset;
} list_t;

static inline list_node_t *
test_list_node(list_t *list, void *item)
{
    return (list_node_t *)((char *)item + list->offset);
}

static inline void *
test_list_item(list_t *list, list_node_t *node)
{
    return node == &list->head ? NULL : (char *)node - list->offset;
}

static inline void
list_create(list_t *list, size_t size, size_t offset)
{
    (void)size;
    list->offset = offset;
    list->head.next = list->head.prev = &list->head;
}

static inline void
list_destroy(list_t *list)
{
    assert(list->head.next == &list->head);
}

static inline int
list_is_empty(list_t *list)
{
    return list->head.next == &list->head;
}

static inline void *
list_head(list_t *list)
{
    return test_list_item(list, list->head.next);
}

static inline void *
list_next(list_t *list, void *item)
{
    return test_list_item(list, test_list_node(list, item)->next);
}

static inline void
list_insert_tail(list_t *list, void *item)
{
    list_node_t *node = test_list_node(list, item);
    node->prev = list->head.prev;
    node->next = &list->head;
    node->prev->next = node;
    list->head.prev = node;
}

static inline void
list_remove(list_t *list, void *item)
{
    list_node_t *node = test_list_node(list, item);
    (void)list;
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = NULL;
}

static inline void mutex_init(kmutex_t *lock, void *a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    assert(pthread_mutex_init(lock, NULL) == 0);
}
static inline void mutex_destroy(kmutex_t *lock)
{
    assert(pthread_mutex_destroy(lock) == 0);
}
static inline void mutex_enter(kmutex_t *lock)
{
    assert(pthread_mutex_lock(lock) == 0);
}
static inline void mutex_exit(kmutex_t *lock)
{
    assert(pthread_mutex_unlock(lock) == 0);
}
static inline void cv_init(kcondvar_t *cv, void *a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    assert(pthread_cond_init(cv, NULL) == 0);
}
static inline void cv_destroy(kcondvar_t *cv)
{
    assert(pthread_cond_destroy(cv) == 0);
}
static inline void cv_wait(kcondvar_t *cv, kmutex_t *lock)
{
    atomic_fetch_add(&umem_test_cv_wait_calls, 1);
    assert(pthread_cond_wait(cv, lock) == 0);
}
static inline void cv_broadcast(kcondvar_t *cv)
{
    assert(pthread_cond_broadcast(cv) == 0);
}

static inline void *kmem_zalloc(size_t n, int flags)
{
    (void)flags;
    return calloc(1, n);
}
static inline void kmem_free(void *ptr, size_t n)
{
    (void)n;
    free(ptr);
}
static inline taskq_t *taskq_create(const char *name, int threads, int pri,
    int minalloc, int maxalloc, int flags)
{
    (void)name; (void)threads; (void)pri; (void)minalloc;
    (void)maxalloc; (void)flags;
    return calloc(1, sizeof (taskq_t));
}
static inline void taskq_destroy(taskq_t *taskq)
{
    free(taskq);
}
static inline void ddi_umem_unlock(ddi_umem_cookie_t cookie)
{
    (void)cookie;
}
static inline void nv_printf(int level, const char *fmt, ...)
{
    va_list args;

    if (level != NV_DBG_ERRORS)
        return;
    va_start(args, fmt);
    (void)vsnprintf(umem_test_last_error,
        sizeof (umem_test_last_error), fmt, args);
    va_end(args);
    atomic_fetch_add(&umem_test_error_logs, 1);
}

struct umem_callback_ops {
    int cbo_umem_callback_version;
    void (*cbo_umem_lock_cleanup)(ddi_umem_cookie_t *);
};

#endif
