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
 * Linux constants that the illumos-side sources also use.  None of them
 * collides with an illumos name.
 */

#ifndef _UVM_KPI_CONST_H_
#define _UVM_KPI_CONST_H_

/* PAGESIZE is a variable outside machine-dependent code. */
#define PAGE_SHIFT              12
#define PAGE_SIZE               (1UL << PAGE_SHIFT)
#define PAGE_MASK               (~(PAGE_SIZE - 1))

/* GFP flags; only __GFP_NOSLEEP decides whether an allocation may block. */
#define __GFP_ZERO              0x0001U
#define __GFP_NOWARN            0x0002U
#define __GFP_NORETRY           0x0004U
#define __GFP_COMP              0x0008U
#define __GFP_THISNODE          0x0010U
#define __GFP_ACCOUNT           0x0020U
#define __GFP_NOMEMALLOC        0x0040U
#define __GFP_HIGHMEM           0x0080U
#define __GFP_MOVABLE           0x0100U
#define __GFP_NOSLEEP           0x0200U
#define __GFP_RETRY_MAYFAIL     0x0400U
#define __GFP_DMA32             0x0800U
#define GFP_KERNEL              0U
#define GFP_NOIO                0U
#define GFP_NOFS                0U
#define GFP_ATOMIC              (__GFP_NOSLEEP | __GFP_NOWARN)
#define GFP_NOWAIT              __GFP_NOSLEEP
#define GFP_HIGHUSER            __GFP_HIGHMEM
#define GFP_HIGHUSER_MOVABLE    (__GFP_HIGHMEM | __GFP_MOVABLE)
#define GFP_DMA32               __GFP_DMA32

#define MAX_ERRNO               4095
#define DMA_MAPPING_ERROR       (~(dma_addr_t)0)

#define ZERO_SIZE_PTR           ((void *)16)
#define ZERO_OR_NULL_PTR(x)     ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

/* printk: KERN_* is a two-byte "\001<level>" prefix as on Linux. */
#define KERN_SOH                "\001"
#define KERN_EMERG              KERN_SOH "0"
#define KERN_ALERT              KERN_SOH "1"
#define KERN_CRIT               KERN_SOH "2"
#define KERN_ERR                KERN_SOH "3"
#define KERN_WARNING            KERN_SOH "4"
#define KERN_NOTICE             KERN_SOH "5"
#define KERN_INFO               KERN_SOH "6"
#define KERN_DEBUG              KERN_SOH "7"
#define KERN_CONT               KERN_SOH "c"

/* VMA flags. */
#define VM_NONE                 0x00000000UL
#define VM_READ                 0x00000001UL
#define VM_WRITE                0x00000002UL
#define VM_EXEC                 0x00000004UL
#define VM_SHARED               0x00000008UL
#define VM_MAYREAD              0x00000010UL
#define VM_MAYWRITE             0x00000020UL
#define VM_MAYEXEC              0x00000040UL
#define VM_MAYSHARE             0x00000080UL
#define VM_PFNMAP               0x00000400UL
#define VM_IO                   0x00004000UL
#define VM_DONTCOPY             0x00020000UL
#define VM_DONTEXPAND           0x00040000UL
#define VM_HUGETLB              0x00400000UL
#define VM_MIXEDMAP             0x10000000UL
#define VM_SPECIAL              (VM_IO | VM_DONTEXPAND | VM_PFNMAP | VM_MIXEDMAP)

#define VM_FAULT_OOM            0x0001U
#define VM_FAULT_SIGBUS         0x0002U
#define VM_FAULT_MAJOR          0x0004U
#define VM_FAULT_SIGSEGV        0x0040U
#define VM_FAULT_NOPAGE         0x0100U
#define VM_FAULT_LOCKED         0x0200U
#define VM_FAULT_RETRY          0x0400U
#define VM_FAULT_ERROR          (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV)

#define FAULT_FLAG_WRITE        0x01U
#define FAULT_FLAG_REMOTE       0x80U

#define FOLL_WRITE              0x01U
#define FOLL_FORCE              0x10U
#define FOLL_LONGTERM           0x100U

/*
 * UVM mirrors CPU virtual addresses on the GPU, so it is limited to the
 * lower half of the amd64 address space, as on Linux.
 */
#define TASK_SIZE               (1UL << 47)

#endif /* _UVM_KPI_CONST_H_ */
