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
 * illumos version of kernel-open/nvidia-uvm/uvm_populate_pageable.c.  The
 * caller already holds the mmap lock, which is the AS lock, so pages are
 * faulted in through the segment driver directly, as as_fault() would.
 */

#include "uvm_common.h"
#include "uvm_ioctl.h"
#include "uvm_linux.h"
#include "uvm_lock.h"
#include "uvm_api.h"
#include "uvm_va_range.h"
#include "uvm_va_space.h"
#include "uvm_populate_pageable.h"
#include "uvm_seg.h"

static bool is_write_populate(struct vm_area_struct *vma,
                              uvm_populate_permissions_t populate_permissions)
{
    switch (populate_permissions) {
        case UVM_POPULATE_PERMISSIONS_INHERIT:
            return vma->vm_flags & VM_WRITE;
        case UVM_POPULATE_PERMISSIONS_ANY:
            return false;
        case UVM_POPULATE_PERMISSIONS_WRITE:
            return true;
        default:
            UVM_ASSERT(0);
            return false;
    }
}

NV_STATUS uvm_populate_pageable_vma(struct vm_area_struct *vma,
                                    unsigned long start,
                                    unsigned long length,
                                    uvm_populate_permissions_t populate_permissions,
                                    NvU32 flags)
{
    unsigned long outer = start + length;
    bool is_write = is_write_populate(vma, populate_permissions);
    struct mm_struct *mm = vma->vm_mm;
    bool is_uvm_managed_vma = uvm_file_is_nvidia_uvm_va_space(vma->vm_file);
    int ret;

    UVM_ASSERT(PAGE_ALIGNED(start));
    UVM_ASSERT(PAGE_ALIGNED(outer));
    UVM_ASSERT(vma->vm_end > start);
    UVM_ASSERT(vma->vm_start < outer);
    uvm_assert_mmap_lock_locked(mm);

    if (!(flags & UVM_POPULATE_PAGEABLE_FLAG_ALLOW_MANAGED) && is_uvm_managed_vma)
        return NV_ERR_INVALID_ADDRESS;

    if (!(flags & UVM_POPULATE_PAGEABLE_FLAG_SKIP_PROT_CHECK) && !(vma->vm_flags & (VM_READ | VM_WRITE)))
        return NV_ERR_INVALID_ADDRESS;

    if (is_write && !(vma->vm_flags & VM_WRITE))
        return NV_ERR_INVALID_ADDRESS;

    start = max(start, vma->vm_start);
    outer = min(outer, vma->vm_end);

    // The fault handler of a managed range records the mmap lock again.
    if (is_uvm_managed_vma)
        uvm_record_unlock_mmap_lock_read(mm);

    ret = uvm_seg_fault_range(mm, start, outer - start, is_write);

    if (is_uvm_managed_vma)
        uvm_record_lock_mmap_lock_read(mm);

    if (ret == ENOMEM)
        return NV_ERR_NO_MEMORY;

    return (ret == 0) ? NV_OK : errno_to_nv_status(-ret);
}

NV_STATUS uvm_populate_pageable(struct mm_struct *mm,
                                const unsigned long start,
                                const unsigned long length,
                                uvm_populate_permissions_t populate_permissions,
                                NvU32 flags)
{
    struct vm_area_struct *vma;
    const unsigned long end = start + length;
    unsigned long prev_end = end;

    UVM_ASSERT(PAGE_ALIGNED(start));
    UVM_ASSERT(PAGE_ALIGNED(length));
    uvm_assert_mmap_lock_locked(mm);

    vma = find_vma_intersection(mm, start, end);
    if (!vma || (start < vma->vm_start))
         return NV_ERR_INVALID_ADDRESS;

    // VMAs are validated and populated one at a time, since they may have
    // different protection flags
    for (; vma && vma->vm_start <= prev_end; vma = find_vma_intersection(mm, prev_end, end)) {
        NV_STATUS status = uvm_populate_pageable_vma(vma, start, end - start, populate_permissions, flags);
        if (status != NV_OK)
            return status;

        if (vma->vm_end >= end)
            return NV_OK;

        prev_end = vma->vm_end;
    }

    // Input range not fully covered by VMAs
    return NV_ERR_INVALID_ADDRESS;
}

NV_STATUS uvm_api_populate_pageable(const UVM_POPULATE_PAGEABLE_PARAMS *params, struct file *filp)
{
    NV_STATUS status;

    if ((params->flags & ~UVM_POPULATE_PAGEABLE_FLAGS_ALL) || (params->flags & UVM_POPULATE_PAGEABLE_FLAGS_INTERNAL))
        return NV_ERR_INVALID_ARGUMENT;

    if ((params->flags & UVM_POPULATE_PAGEABLE_FLAGS_TEST) && !uvm_enable_builtin_tests) {
        UVM_INFO_PRINT("Test flag set for UVM_POPULATE_PAGEABLE. Did you mean to insmod with uvm_enable_builtin_tests=1?\n");
        return NV_ERR_INVALID_ARGUMENT;
    }

    // Check size, alignment and overflow. VMA validations are performed by
    // populate_pageable
    if (uvm_api_range_invalid(params->base, params->length))
        return NV_ERR_INVALID_ADDRESS;

    // This API works on current->mm, not the mm of the VA space, so the VA
    // space lock is not needed.
    uvm_down_read_mmap_lock(current->mm);

    status = uvm_populate_pageable(current->mm,
                                   params->base,
                                   params->length,
                                   UVM_POPULATE_PERMISSIONS_INHERIT,
                                   params->flags);

    uvm_up_read_mmap_lock(current->mm);

    return status;
}
