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
 * UVM entry points that can reach RM (CPU faults, ioctls, the last release
 * of a file) need more stack than an LWP's 20 KB when UVM evicts GPU memory
 * or reports a fatal error; RM runs on its caller's stack.  They run on a
 * split stack from thread_splitstack(), as fem does, which keeps the
 * calling thread, its process, credentials and signals.  Kernels without
 * thread_splitstack() run them on the caller's stack.
 */

#include "uvm_illumos.h"

#include <sys/proc.h>

#pragma weak thread_splitstack

/* Split when less than this is left; the split stack is UVM_THREAD_STACK_SIZE. */
#define UVM_STACK_NEEDED        (48 * 1024)

static boolean_t
uvm_stack_split_needed(void)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);

    if (thread_splitstack == NULL)
        return (B_FALSE);

    return (sp < (uintptr_t)curthread->t_stkbase + UVM_STACK_NEEDED);
}

void
uvm_stack_call(void (*fn)(void *), void *arg)
{
    kthread_t *t = curthread;
    label_t *onfault;

    if (!uvm_stack_split_needed()) {
        fn(arg);
        return;
    }

    /*
     * A fault on a UVM mapping can come from code under on_fault(), such as
     * uucopy() or the lwp_mutex calls.  thread_splitstack() refuses that,
     * because a longjmp would leave the split stack.  The fault's own
     * result goes back through trap(), which restores t_onfault itself.
     */
    onfault = t->t_onfault;
    t->t_onfault = NULL;
    thread_splitstack(fn, arg, UVM_THREAD_STACK_SIZE);
    t->t_onfault = onfault;
}

typedef struct uvm_fault_baton {
    vm_fault_t        (*ufb_fn)(struct vm_fault *);
    struct vm_fault    *ufb_vmf;
    vm_fault_t          ufb_ret;
} uvm_fault_baton_t;

static void
uvm_fault_handoff(void *arg)
{
    uvm_fault_baton_t *b = arg;

    b->ufb_ret = b->ufb_fn(b->ufb_vmf);
}

/* Calls a vm_ops fault handler. */
vm_fault_t
uvm_fault_call(vm_fault_t (*fn)(struct vm_fault *), struct vm_fault *vmf)
{
    uvm_fault_baton_t b = { fn, vmf, 0 };

    uvm_stack_call(uvm_fault_handoff, &b);

    return (b.ufb_ret);
}

typedef struct uvm_vma_baton {
    void                  (*uvb_op)(struct vm_area_struct *);
    struct vm_area_struct  *uvb_vma;
} uvm_vma_baton_t;

static void
uvm_vma_handoff(void *arg)
{
    uvm_vma_baton_t *b = arg;

    b->uvb_op(b->uvb_vma);
}

/* Calls a vm_ops open or close handler; closing can free GPU memory. */
void
uvm_vma_op_call(void (*op)(struct vm_area_struct *), struct vm_area_struct *vma)
{
    uvm_vma_baton_t b = { op, vma };

    uvm_stack_call(uvm_vma_handoff, &b);
}

boolean_t
uvm_stack_split_supported(void)
{
    return (thread_splitstack != NULL);
}
