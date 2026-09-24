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
 * The GPU device that nvidia hands to nvidia-uvm through
 * nvUvmInterfaceRegisterGpu() (UvmGpuPlatformInfo::pci_dev).
 */

#ifndef _NV_ILLUMOS_PCI_DEV_H_
#define _NV_ILLUMOS_PCI_DEV_H_

#include <sys/sunddi.h>

/*
 * dma_start and dma_limit bound the bus addresses the GPU can reach; they
 * are set when nvidia-uvm registers the GPU.
 */
struct device {
    dev_info_t *dip;
    uint64_t    dma_start;
    uint64_t    dma_limit;
};

/* nvidia-uvm takes &pci_dev->dev for its DMA calls. */
struct pci_dev {
    dev_info_t *dip;
    struct device dev;
};

#endif /* _NV_ILLUMOS_PCI_DEV_H_ */
