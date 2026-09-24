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
 * The parts of kernel-open/common/inc/nv-linux.h and nv-mm.h that
 * nvidia-uvm uses.
 */

#ifndef _NV_LINUX_H_
#define _NV_LINUX_H_

#include "uvm_illumos_kpi.h"
#include "nvtypes.h"
#include "nvstatus.h"

#define NV_MAY_SLEEP()          (!in_interrupt() && !in_atomic())

#define NV_KMEM_CACHE_CREATE(name, type)                                    \
    linux_kmem_cache_create(name, sizeof(type), __alignof__(type))
#define nv_kmem_cache_create(name, size, align)                             \
    linux_kmem_cache_create(name, size, align)

static inline void *nv_kmem_cache_zalloc(struct kmem_cache *k, gfp_t flags)
{
    return linux_kmem_cache_alloc(k, flags | __GFP_ZERO);
}

#define nv_mmap_read_lock(mm)       linux_mmap_read_lock(mm)
#define nv_mmap_read_unlock(mm)     linux_mmap_read_unlock(mm)
#define nv_mmap_write_lock(mm)      linux_mmap_write_lock(mm)
#define nv_mmap_write_unlock(mm)    linux_mmap_write_unlock(mm)
#define nv_mm_rwsem_is_locked(mm)   linux_mmap_is_locked(mm)
#define nv_mmap_get_lock(mm)        linux_mmap_get_lock(mm)

static inline void nv_vm_flags_set(struct vm_area_struct *vma, vm_flags_t f)
{
    vma->vm_flags |= f;
}

static inline void nv_vm_flags_clear(struct vm_area_struct *vma, vm_flags_t f)
{
    vma->vm_flags &= ~f;
}

#define NV_PIN_USER_PAGES_REMOTE(mm, start, n, flags, pages, locked)        \
    ((long)-EOPNOTSUPP)
#define NV_GET_USER_PAGES_REMOTE(mm, start, n, flags, pages, locked)        \
    ((long)-EOPNOTSUPP)

#define NV_PCI_DOMAIN_NUMBER(pdev)  0
#define NV_PCI_BUS_NUMBER(pdev)     0
#define NV_PCI_DEVFN(pdev)          0
#define NV_GET_DOMAIN_BUS_AND_SLOT(d, b, s)     ((struct pci_dev *)NULL)

#define NV_GPU_BAR_INDEX_FB     1

static inline NvU8 nv_bar_index_to_os_bar_index(struct pci_dev *dev,
    NvU8 nv_bar_index)
{
    return nv_bar_index;
}

static inline NvBool nv_numa_node_has_memory(int node_id)
{
    return node_id == 0;
}

#define nv_ioremap_cache(pa, size)  ((void)(pa), (void *)NULL)
#define nv_iounmap(va, size)        ((void)(va))

#endif /* _NV_LINUX_H_ */
