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
 * Internal interfaces of the illumos nvidia_uvm driver, shared by its
 * illumos-side sources.
 */

#ifndef _UVM_ILLUMOS_H_
#define _UVM_ILLUMOS_H_

#include "uvm_kpi_types.h"

#define UVM_ILLUMOS_NAME        "nvidia_uvm"

/* Base minors, as in kernel-open/nvidia-uvm/uvm_common.h. */
#define UVM_NODE_UVM            0
#define UVM_NODE_TOOLS          1
#define UVM_NODE_COUNT          2

/* Clone minors: clone id in the upper 16 bits, base minor below. */
#define UVM_MINOR_NODE(m)       ((m) & 0xffffU)
#define UVM_MINOR_CLONE(m)      ((m) >> 16)
#define UVM_MINOR_MAKE(c, n)    ((minor_t)(((c) << 16) | (n)))
#define UVM_CLONE_MAX           0xffffU

/* uvm_illumos.c */
extern major_t uvm_illumos_major;

struct linux_file *uvm_file_hold_dev(dev_t);
void    uvm_file_hold(struct linux_file *);
void    uvm_file_rele(struct linux_file *);
void    uvm_file_queue_pollwakeup(struct linux_file *);

/* uvm_illumos_kpi.c */
int     uvm_kpi_init(void);
void    uvm_kpi_fini(void);
kthread_t *uvm_thread_create(void (*)(void *), void *);

/* uvm_illumos_params.c */
void    uvm_params_init(void);
void    uvm_params_apply(dev_info_t *);
void    uvm_params_fini(void);

/* uvm_illumos_pm.c */
void    uvm_pm_init(void);
void    uvm_pm_fini(void);
boolean_t uvm_pm_is_suspended(void);

/* uvm_illumos_stack.c */
void    uvm_stack_call(void (*)(void *), void *);
vm_fault_t uvm_fault_call(vm_fault_t (*)(struct vm_fault *), struct vm_fault *);
void    uvm_vma_op_call(void (*)(struct vm_area_struct *),
            struct vm_area_struct *);
boolean_t uvm_stack_split_supported(void);

/* uvm_illumos_page.c */
int     uvm_page_init(void);
void    uvm_page_fini(void);

#endif /* _UVM_ILLUMOS_H_ */
