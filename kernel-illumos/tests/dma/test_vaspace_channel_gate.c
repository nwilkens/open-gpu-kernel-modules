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

#define NV_SUNOS 1

typedef int NV_STATUS;
typedef int NvBool;
typedef unsigned int NvU32;
typedef struct OBJVASPACE { int unused; } OBJVASPACE;
typedef struct KernelChannel {
    OBJVASPACE *pVAS;
    volatile NvU32 uvmRevoked;
    NvBool bUvmOwned;
    NvBool bIsContextBound;
} KernelChannel;

#define NV_OK 0
#define NV_ERR_INVALID_STATE 1
#define NV_ERR_INVALID_CHANNEL 2
#define NV_WARN_MORE_PROCESSING_REQUIRED 3

static NvU32
portAtomicOrU32(volatile NvU32 *value, NvU32 mask)
{
    NvU32 old = *value;
    *value |= mask;
    return old;
}

#include "../../../src/nvidia/src/kernel/rmapi/nv_gpu_ops_channel_gate_impl.h"

int
main(void)
{
    OBJVASPACE target = {0}, other = {0};
    KernelChannel channel = {.pVAS = &other};

    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_OK);
    channel.pVAS = &target;
    /* Raw user channels have never run and can be certified by the scan. */
    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_OK);
    channel.bUvmOwned = 1;
    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_ERR_INVALID_STATE);
    channel.bUvmOwned = 0;
    channel.bIsContextBound = 1;
    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_ERR_INVALID_STATE);
    channel.bIsContextBound = 0;
    channel.uvmRevoked = NV_GPU_OPS_CHANNEL_REVOKE_STARTED;
    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_ERR_INVALID_STATE);
    channel.uvmRevoked = NV_GPU_OPS_CHANNEL_REVOKE_STOPPED;
    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_ERR_INVALID_STATE);
    channel.uvmRevoked = NV_GPU_OPS_CHANNEL_REVOKE_STARTED;
    channel.uvmRevoked |= NV_GPU_OPS_CHANNEL_REVOKE_STOPPED;
    assert(nvGpuOpsChannelRevocationStatus(&target, &channel) == NV_OK);

    /* A missing retained instance needs the later global VA-space scan. */
    assert(nvGpuOpsMissingChannelRevokeStatus(NV_ERR_INVALID_CHANNEL, 1) ==
           NV_WARN_MORE_PROCESSING_REQUIRED);
    assert(nvGpuOpsMissingChannelRevokeStatus(NV_ERR_INVALID_CHANNEL, 0) ==
           NV_ERR_INVALID_CHANNEL);
    assert(nvGpuOpsMissingChannelRevokeStatus(NV_ERR_INVALID_STATE, 1) ==
           NV_ERR_INVALID_STATE);
    return 0;
}
