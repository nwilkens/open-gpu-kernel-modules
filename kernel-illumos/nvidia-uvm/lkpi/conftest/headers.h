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
 * kernel-open/common/inc/conftest.h includes the files that conftest.sh
 * generates on Linux; the illumos build supplies them here.  Every test is
 * left undefined except the three presence tests that let uvm_linux.h skip
 * compatibility code needing Linux struct page and scatterlist internals.
 */

#ifndef _UVM_ILLUMOS_CONFTEST_H_
#define _UVM_ILLUMOS_CONFTEST_H_

#define NV_PAGE_PGMAP_PRESENT
#define NV_SG_DMA_PAGE_ITER_PRESENT
#define NV_FOR_EACH_SGTABLE_DMA_PAGE_PRESENT

#define NV_IS_EXPORT_SYMBOL_PRESENT_int_active_memcg            1
#define NV_IS_EXPORT_SYMBOL_PRESENT_migrate_vma_setup           0
#define NV_IS_EXPORT_SYMBOL_PRESENT___iowrite64_lo_hi           0
#define NV_IS_EXPORT_SYMBOL_PRESENT_make_device_exclusive       0
#define NV_IS_EXPORT_SYMBOL_GPL_dma_iova_try_alloc              0

#endif /* _UVM_ILLUMOS_CONFTEST_H_ */
