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

/* Included by nv_gpu_ops.c and the focused channel-stop status test. */
#include "nv_gpu_ops_channel_gate_impl.h"
static NV_STATUS
nvGpuOpsStopChannelControl(RM_API *pRmApi, KernelChannel *pKernelChannel,
                           gpuRetainedChannel *retainedChannel,
                           NvBool bImmediate)
{
    NVA06F_CTRL_STOP_CHANNEL_PARAMS stopChannelParams = {0};
    NV_STATUS status;

    stopChannelParams.bImmediate = bImmediate;
    status = pRmApi->Control(pRmApi,
                             RES_GET_CLIENT_HANDLE(pKernelChannel),
                             RES_GET_HANDLE(pKernelChannel),
                             NVA06F_CTRL_CMD_STOP_CHANNEL,
                             &stopChannelParams,
                             sizeof (stopChannelParams));
#if defined(NV_SUNOS)
    if (status != NV_OK)
        return status;
#else
    NV_ASSERT_OK(status);
    pKernelChannel->bIsContextBound = NV_FALSE;
#endif

    if (retainedChannel->channelEngineType == UVM_GPU_CHANNEL_ENGINE_TYPE_GR)
    {
        NV2080_CTRL_GPU_EVICT_CTX_PARAMS params;

        portMemSet(&params, 0, sizeof (params));
        params.engineType = NV2080_ENGINE_TYPE_GR(0);
        params.hClient = retainedChannel->session->handle;
        params.hChanClient = RES_GET_CLIENT_HANDLE(pKernelChannel);
        params.hObject = RES_GET_HANDLE(pKernelChannel);

        status = pRmApi->Control(pRmApi,
                                 retainedChannel->session->handle,
                                 retainedChannel->rmSubDevice->subDeviceHandle,
                                 NV2080_CTRL_CMD_GPU_EVICT_CTX,
                                 &params,
                                 sizeof (params));
#if defined(NV_SUNOS)
        if (status != NV_OK)
            return status;
#else
        NV_ASSERT_OK(status);
#endif
    }

#if defined(NV_SUNOS)
    pKernelChannel->bIsContextBound = NV_FALSE;
#endif
    return NV_OK;
}

/* A callback-owned stop must also prevent later binds and scheduling. */
#if defined(NV_SUNOS)
static NV_STATUS
nvGpuOpsRevokeChannelControl(RM_API *pRmApi, KernelChannel *pKernelChannel,
                             gpuRetainedChannel *retainedChannel)
{
    NV_STATUS status;

    /* Mark before STOP so a failed stop cannot leave a reusable channel. */
    portAtomicOrU32(&pKernelChannel->uvmRevoked,
                    NV_GPU_OPS_CHANNEL_REVOKE_STARTED);
    if ((portAtomicOrU32(&pKernelChannel->uvmRevoked, 0) &
         NV_GPU_OPS_CHANNEL_REVOKE_STOPPED) != 0)
        return NV_OK;
    status = nvGpuOpsStopChannelControl(pRmApi, pKernelChannel,
                                        retainedChannel, NV_FALSE);
    if (status == NV_OK)
        portAtomicOrU32(&pKernelChannel->uvmRevoked,
                        NV_GPU_OPS_CHANNEL_REVOKE_STOPPED);
    return status;
}
#endif
