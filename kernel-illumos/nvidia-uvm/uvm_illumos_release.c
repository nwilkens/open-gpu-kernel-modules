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
 * uvm_release() hands a VA space to g_uvm_global.deferred_release_q when
 * it cannot take pm.lock.  The queue runs its items in order on one thread,
 * so work queued behind such a release runs after the VA space is gone.
 */

#include "uvm_global.h"
#include "uvm_illumos.h"

typedef struct uvm_release_work {
    nv_kthread_q_item_t urw_item;
    void              (*urw_fn)(void *);
    void               *urw_arg;
} uvm_release_work_t;

static void
uvm_release_work_run(void *arg)
{
    uvm_release_work_t *w = arg;

    w->urw_fn(w->urw_arg);
    kmem_free(w, sizeof (*w));
}

/* Run fn(arg) after every VA space release deferred so far. */
void
uvm_after_deferred_release(void (*fn)(void *), void *arg)
{
    uvm_release_work_t *w = kmem_alloc(sizeof (*w), KM_SLEEP);

    w->urw_fn = fn;
    w->urw_arg = arg;
    nv_kthread_q_item_init(&w->urw_item, uvm_release_work_run, w);

    /* The queue stops only once UVM has no files left. */
    if (nv_kthread_q_schedule_q_item(&g_uvm_global.deferred_release_q,
        &w->urw_item) == 0) {
        kmem_free(w, sizeof (*w));
        fn(arg);
    }
}
