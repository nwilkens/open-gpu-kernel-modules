/*
 * SPDX-FileCopyrightText: Copyright (c) 1999-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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


/* Helpers shared by the system memory, mapping, user memory and DMA code. */

#ifndef _NVIDIA_MEM_H_
#define _NVIDIA_MEM_H_

#include <vm/hat.h>
#include <vm/as.h>
#include <vm/seg_kmem.h>

extern const ddi_device_acc_attr_t nv_sysmem_acc_attr;

uint_t      nv_cache_to_iomem(NvU32 cache_type);
NvBool      nv_cache_type_valid(NvU32 cache_type);
void        nv_dma_attr_init(ddi_dma_attr_t *attr, NvU64 addr_hi, NvU64 align,
                int sgllen);
dev_info_t *nv_dma_dip(nv_state_t *nv);
caddr_t     nv_kmap_phys(const NvU64 *phys, NvU64 count, NvBool contig,
                NvU32 cache_type);
void        nv_kunmap_phys(caddr_t va, NvU64 count);
caddr_t     nv_alloc_kva(nv_illumos_alloc_t *at);
nv_illumos_alloc_t *nvos_create_alloc(dev_info_t *dip, NvU64 num_pages);

static inline NvU64
nv_kva_to_phys(caddr_t va)
{
    return (NvU64)mmu_ptob((NvU64)hat_getpfnum(kas.a_hat, va)) |
        ((uintptr_t)va & MMU_PAGEOFFSET);
}

#endif /* _NVIDIA_MEM_H_ */
