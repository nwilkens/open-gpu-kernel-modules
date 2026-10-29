/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Carlos Neira
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

#include <assert.h>

typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1

#include "../../../src/nvidia/src/kernel/rmapi/nv_gpu_ops_free_status_impl.h"

int
main(void)
{
#if defined(NV_SUNOS)
    assert(nvGpuOpsDupFreeResult(NV_OK, NV_ERR_OPERATING_SYSTEM) ==
           NV_ERR_OPERATING_SYSTEM);
#else
    assert(nvGpuOpsDupFreeResult(NV_OK, NV_ERR_OPERATING_SYSTEM) == NV_OK);
#endif
    assert(nvGpuOpsDupFreeResult(NV_ERR_OPERATING_SYSTEM, NV_OK) ==
           NV_ERR_OPERATING_SYSTEM);
    return 0;
}
