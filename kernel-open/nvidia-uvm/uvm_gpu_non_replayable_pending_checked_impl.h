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

/* Shared with the focused RM pending-fault query failure test. */
NV_STATUS uvm_parent_gpu_non_replayable_faults_pending_checked(uvm_parent_gpu_t *parent_gpu,
                                                               bool *pending)
{
    NV_STATUS status;
    NvBool has_pending_faults = NV_FALSE;

    UVM_ASSERT(parent_gpu->isr.non_replayable_faults.handling);
    uvm_assert_spinlock_locked(&parent_gpu->isr.interrupts_lock);

    status = nvUvmInterfaceHasPendingNonReplayableFaults(&parent_gpu->fault_buffer.rm_info,
                                                         &has_pending_faults);
    if (status == NV_OK)
        *pending = has_pending_faults == NV_TRUE;
    return status;
}
