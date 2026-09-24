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
 * Interface that nvidia exports to the nvidia_i2c controller provider.
 */

#ifndef _NV_ILLUMOS_I2C_H_
#define _NV_ILLUMOS_I2C_H_

#include "nv.h"

/* RM I2C ports per GPU, NV402C_CTRL_NUM_I2C_PORTS. */
#define NV_I2C_NUM_PORTS        16

/* Largest transfer accepted, the DP AUX limit and I2C_REQ_MAX. */
#define NV_I2C_MAX_XFER         256

typedef struct nvidia_i2c_gpu_info {
    NvU32   gpu_id;
    NvU64   gen;            /* distinct for every attach of every GPU */
    NvU32   domain;
    NvU8    bus;
    NvU8    slot;
    NvU8    function;
} nvidia_i2c_gpu_info_t;

typedef struct nvidia_i2c_callbacks {
    void (*probe)(const nvidia_i2c_gpu_info_t *);
} nvidia_i2c_callbacks_t;

typedef struct nvidia_i2c_ops {
    const char *version_string;
    int         (*set_callbacks)(const nvidia_i2c_callbacks_t *);
    NvU32       (*enumerate_gpus)(nvidia_i2c_gpu_info_t *, NvU32);
    NV_STATUS   (*transfer)(NvU32 gpu_id, NvU64 gen, NvU32 port,
                    nv_i2c_cmd_t cmd, NvU8 addr, NvU8 command, NvU32 len,
                    NvU8 *buf);
} nvidia_i2c_ops_t;

NV_STATUS nvidia_get_i2c_ops(nvidia_i2c_ops_t *ops);

#endif /* _NV_ILLUMOS_I2C_H_ */
