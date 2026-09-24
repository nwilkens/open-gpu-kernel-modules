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
 * Linux kernel interfaces for the nvidia-uvm sources, implemented on
 * illumos.  Every Linux header in the linux and asm directories here
 * includes this header.  After it, no illumos header may be included: part 2 renames
 * identifiers that illumos headers declare (file, min, max, mutex_init,
 * sema_init, list_move_tail, kmem_cache_alloc).
 */

#ifndef _UVM_ILLUMOS_KPI_H_
#define _UVM_ILLUMOS_KPI_H_

#include "uvm_kpi_types.h"

#include "kpi/uvm_kpi_base.h"
#include "kpi/uvm_kpi_atomic.h"
#include "kpi/uvm_kpi_list.h"
#include "kpi/uvm_kpi_sync.h"
#include "kpi/uvm_kpi_mm.h"
#include "kpi/uvm_kpi_file.h"
#include "kpi/uvm_kpi_test.h"

#endif /* _UVM_ILLUMOS_KPI_H_ */
