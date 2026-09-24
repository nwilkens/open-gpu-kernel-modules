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
 * seg_nvuvm: the segment driver behind mmap() of /dev/nvidia-uvm.
 */

#ifndef _UVM_SEG_H_
#define _UVM_SEG_H_

#include "uvm_kpi_types.h"

/*
 * Private data of a seg_nvuvm segment.  The Linux VMA is a separate
 * allocation because splitting a segment moves it to another segment.  A
 * segment copied by fork has no VMA and no file and faults with SIGBUS.
 */
typedef struct uvm_seg_data {
    struct seg             *usd_seg;
    struct vm_area_struct  *usd_vma;
    struct linux_file      *usd_file;       /* held */
    struct address_space   *usd_mapping;    /* usd_file->f_mapping */
    list_node_t             usd_link;       /* on usd_mapping->am_segs */
    boolean_t               usd_listed;
    uchar_t                 usd_prot;       /* PROT_* without PROT_USER */
    uchar_t                 usd_maxprot;
} uvm_seg_data_t;

int     uvm_seg_segmap(dev_t, off_t, struct as *, caddr_t *, off_t, uint_t,
            uint_t, uint_t, cred_t *);
uint_t  uvm_seg_count(void);

/* Fault in [start, start + len) of one segment of mm; returns an errno. */
int     uvm_seg_fault_range(struct mm_struct *, unsigned long, unsigned long,
            boolean_t);

#endif /* _UVM_SEG_H_ */
