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
 * The small set of Linux kernel interfaces that shared kernel-open sources
 * built into the illumos nvidia module use, implemented on illumos.
 */

#ifndef _NV_ILLUMOS_KPI_H_
#define _NV_ILLUMOS_KPI_H_

#include "nv-illumos.h"

/* Every global of an illumos module is visible to modules that depend on it. */
#define EXPORT_SYMBOL(sym)

#define NV_MAY_SLEEP()                  nv_may_sleep()

#define WARN_ON(cond)                                                       \
    ({                                                                      \
        int __nv_warn = !!(cond);                                           \
        if (__nv_warn)                                                      \
            cmn_err(CE_WARN, "NVRM: WARN_ON(%s) at %s:%d", #cond,           \
                __FILE__, __LINE__);                                        \
        __nv_warn;                                                          \
    })

typedef struct { volatile uint32_t counter; } atomic_t;
typedef struct { volatile ulong_t counter; } atomic_long_t;

#define ATOMIC_INIT(v)                  { (v) }

static inline int atomic_inc_return(atomic_t *v)
{
    return (int)atomic_inc_32_nv(&v->counter);
}

static inline long atomic_long_read(atomic_long_t *v)
{
    return (long)*(volatile ulong_t *)&v->counter;
}

static inline void atomic_long_set(atomic_long_t *v, long i)
{
    (void) atomic_swap_ulong(&v->counter, (ulong_t)i);
}

/* Linux semaphores may be released by another thread, like ksema_t. */
struct semaphore {
    ksema_t s;
};

#define NV_INIT_MUTEX(sem)      sema_init(&(sem)->s, 1, NULL, SEMA_DRIVER, NULL)
#define down(sem)               sema_p(&(sem)->s)
#define up(sem)                 sema_v(&(sem)->s)

static inline int nv_kmem_cache_alloc_stack(nvidia_stack_t **sp)
{
    return (nv_stack_alloc(sp) == 0) ? 0 : -ENOMEM;
}

static inline void nv_kmem_cache_free_stack(nvidia_stack_t *sp)
{
    nv_stack_free(sp);
}

#endif /* _NV_ILLUMOS_KPI_H_ */
