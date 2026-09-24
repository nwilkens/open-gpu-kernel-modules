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
 * Replaces kernel-open/nvidia-uvm/uvm_ats_faults.c, which needs Linux
 * mempolicy internals.  ATS is never enabled on illumos, so GPU faults and
 * access counters never take these paths.
 */

#include "uvm_ats_faults.h"
#include "uvm_va_space.h"

NV_STATUS uvm_ats_service_faults(uvm_gpu_va_space_t *gpu_va_space,
                                 struct vm_area_struct *vma,
                                 NvU64 base,
                                 uvm_ats_fault_context_t *ats_context)
{
    UVM_ASSERT_MSG(0, "ATS fault on a system without ATS\n");
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS uvm_ats_service_access_counters(uvm_gpu_va_space_t *gpu_va_space,
                                          struct vm_area_struct *vma,
                                          NvU64 base,
                                          uvm_ats_fault_context_t *ats_context)
{
    UVM_ASSERT_MSG(0, "ATS access counter on a system without ATS\n");
    return NV_ERR_NOT_SUPPORTED;
}

// Every GPU fault address is treated as part of a GMMU region.
bool uvm_ats_check_in_gmmu_region(uvm_va_space_t *va_space, NvU64 address, uvm_va_range_t *next)
{
    return true;
}

void uvm_flush_tlb_va_region(uvm_gpu_va_space_t *gpu_va_space,
                             NvU64 addr,
                             size_t size,
                             uvm_fault_client_type_t client_type)
{
    UVM_ASSERT_MSG(0, "ATS TLB flush on a system without ATS\n");
}

// Nothing ever starts an ATS TLB batch.
NV_STATUS uvm_ats_invalidate_tlbs(uvm_gpu_va_space_t *gpu_va_space,
                                  uvm_ats_fault_invalidate_t *ats_invalidate,
                                  uvm_tracker_t *out_tracker)
{
    UVM_ASSERT(!ats_invalidate->tlb_batch_pending);
    return NV_OK;
}
