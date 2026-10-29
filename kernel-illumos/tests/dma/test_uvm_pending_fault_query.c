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

/* A failed RM query must not certify that fault service can be drained. */
#include <assert.h>
#include <stdbool.h>

typedef int NV_STATUS;
typedef int NvBool;
#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_TRUE 1
#define NV_FALSE 0
#define UVM_ASSERT(expr) assert(expr)
#define uvm_assert_spinlock_locked(lock) assert(*(lock) == 1)

typedef struct {
    struct { struct { bool handling; } non_replayable_faults; int interrupts_lock; } isr;
    struct { int rm_info; } fault_buffer;
} uvm_parent_gpu_t;

static NV_STATUS query_status;
static NvBool query_pending;

static NV_STATUS
nvUvmInterfaceHasPendingNonReplayableFaults(int *rm_info, NvBool *pending)
{
    assert(*rm_info == 7);
    if (query_status == NV_OK)
        *pending = query_pending;
    return query_status;
}

#include "../../../kernel-open/nvidia-uvm/uvm_gpu_non_replayable_pending_checked_impl.h"

int
main(void)
{
    uvm_parent_gpu_t gpu = {
        .isr.non_replayable_faults.handling = true,
        .isr.interrupts_lock = 1,
        .fault_buffer.rm_info = 7
    };
    bool pending = true;

    query_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_parent_gpu_non_replayable_faults_pending_checked(&gpu, &pending) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(pending);

    query_status = NV_OK;
    query_pending = NV_FALSE;
    assert(uvm_parent_gpu_non_replayable_faults_pending_checked(&gpu, &pending) == NV_OK);
    assert(!pending);

    query_pending = NV_TRUE;
    assert(uvm_parent_gpu_non_replayable_faults_pending_checked(&gpu, &pending) == NV_OK);
    assert(pending);
    return 0;
}
