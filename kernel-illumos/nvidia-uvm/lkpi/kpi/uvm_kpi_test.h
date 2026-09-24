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
 * Linux interfaces that only the UVM builtin tests use.  The tests run only
 * through uvm_test_ioctl(), which requires uvm_enable_builtin_tests.  The
 * functions are in nvidia-uvm/uvm_illumos_test.c.
 */

#ifndef _UVM_KPI_TEST_H_
#define _UVM_KPI_TEST_H_

#define INIT_RADIX_TREE(root, gfp)  linux_radix_tree_init(root, gfp)
#define radix_tree_lookup(root, index)                                      \
    linux_radix_tree_lookup(root, index)
#define radix_tree_insert(root, index, item)                                \
    linux_radix_tree_insert(root, index, item)
#define radix_tree_delete(root, index)                                      \
    linux_radix_tree_delete(root, index)

/* The loop body may delete the entry at iter->index. */
#define radix_tree_for_each_slot(slot, root, iter, start)                   \
    for ((slot) = linux_radix_tree_iter_first(root, iter, start);           \
         (slot) != NULL;                                                    \
         (slot) = linux_radix_tree_iter_next(root, iter))

#define remap_pfn_range(vma, addr, pfn, size, prot)                         \
    linux_remap_pfn_range(vma, addr, pfn, size, prot)

#define vma_pages(vma)          (((vma)->vm_end - (vma)->vm_start) >> PAGE_SHIFT)

#define phys_to_virt(pa)        linux_phys_to_virt((phys_addr_t)(pa))

#define memset_io(p, v, n)      memset((void *)(p), v, n)

#define swap(a, b)                                                          \
    do { typeof(a) __swap_t = (a); (a) = (b); (b) = __swap_t; } while (0)

static inline bool cpumask_subset(const struct cpumask *a,
    const struct cpumask *b)
{
    return bitmap_subset(a->bits, b->bits, LINUX_NR_CPUS);
}

#define THREAD_SIZE             UVM_THREAD_STACK_SIZE

#define kthread_run(fn, data, namefmt, ...) linux_kthread_run(fn, data)
#define kthread_stop(t)         linux_kthread_stop(t)
#define kthread_should_stop()   linux_kthread_should_stop()

#endif /* _UVM_KPI_TEST_H_ */
