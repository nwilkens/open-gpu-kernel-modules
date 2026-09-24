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
 * The builtin tests are not built yet.  These stand in for the entry
 * points that the rest of UVM calls; without the tests no UVM_FD_TEST file
 * can be created.
 */

#include "uvm_common.h"
#include "uvm_test.h"
#include "uvm_test_file.h"
#include "uvm_kvmalloc.h"

long uvm_test_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    UVM_INFO_PRINT("ioctl %d not found. Builtin tests are not available.\n", cmd);
    return -ENOSYS;
}

int uvm_test_file_mmap(uvm_test_file_t *test_file, struct vm_area_struct *vma)
{
    return -ENODEV;
}

void uvm_test_file_release(struct file *filp, uvm_test_file_t *test_file)
{
    uvm_kvfree(test_file);
}
