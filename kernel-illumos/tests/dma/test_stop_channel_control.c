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

/* Stop and GR eviction failures must leave the channel state unconfirmed. */
#include <assert.h>

#if !defined(TEST_LINUX)
#define NV_SUNOS 1
#endif
#include <stddef.h>
#include <string.h>

typedef int NV_STATUS;
typedef int NvBool;
typedef unsigned int NvU32;
typedef struct OBJVASPACE { int unused; } OBJVASPACE;
typedef struct { NvBool bImmediate; } NVA06F_CTRL_STOP_CHANNEL_PARAMS;
typedef struct {
    NvU32 engineType;
    NvU32 hClient;
    NvU32 hChanClient;
    NvU32 hObject;
} NV2080_CTRL_GPU_EVICT_CTX_PARAMS;
typedef struct { NvU32 handle; } TEST_SESSION;
typedef struct { NvU32 subDeviceHandle; } TEST_SUBDEVICE;
typedef struct {
    TEST_SESSION *session;
    TEST_SUBDEVICE *rmSubDevice;
    NvU32 channelEngineType;
} gpuRetainedChannel;
typedef struct {
    NvBool bIsContextBound;
    NvBool bUvmOwned;
    OBJVASPACE *pVAS;
    volatile NvU32 uvmRevoked;
    NvU32 client;
    NvU32 handle;
} KernelChannel;
typedef struct RM_API RM_API;
struct RM_API {
    NV_STATUS (*Control)(RM_API *, NvU32, NvU32, NvU32, void *, size_t);
};

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_INVALID_STATE 2
#define NV_ERR_INVALID_CHANNEL 3
#define NV_WARN_MORE_PROCESSING_REQUIRED 4
#define NV_FALSE 0
#define NV_TRUE 1
#define NVA06F_CTRL_CMD_STOP_CHANNEL 10
#define NV2080_CTRL_CMD_GPU_EVICT_CTX 20
#define UVM_GPU_CHANNEL_ENGINE_TYPE_GR 30
#define NV2080_ENGINE_TYPE_GR(x) (40 + (x))
#define RES_GET_CLIENT_HANDLE(channel) ((channel)->client)
#define RES_GET_HANDLE(channel) ((channel)->handle)
#define portMemSet memset
#define NV_ASSERT_OK(status) ((void)(status))

static NV_STATUS stop_status;
static NV_STATUS evict_status;
static int stop_count;
static int evict_count;
static NvBool seen_immediate;
static KernelChannel *controlled_channel;
static NvBool revoked_before_stop;

#if defined(NV_SUNOS)
static NvU32
portAtomicOrU32(volatile NvU32 *value, NvU32 mask)
{
    NvU32 previous = *value;
    *value |= mask;
    return previous;
}
#endif

static NV_STATUS
control(RM_API *api, NvU32 client, NvU32 handle, NvU32 command,
        void *params, size_t size)
{
    (void)api;
    (void)client;
    (void)handle;
    if (command == NVA06F_CTRL_CMD_STOP_CHANNEL) {
        revoked_before_stop = controlled_channel->uvmRevoked != 0;
        assert(size == sizeof (NVA06F_CTRL_STOP_CHANNEL_PARAMS));
        seen_immediate = ((NVA06F_CTRL_STOP_CHANNEL_PARAMS *)params)->bImmediate;
        stop_count++;
        return stop_status;
    }
    assert(command == NV2080_CTRL_CMD_GPU_EVICT_CTX);
    assert(size == sizeof (NV2080_CTRL_GPU_EVICT_CTX_PARAMS));
    evict_count++;
    return evict_status;
}

#include "../../../src/nvidia/src/kernel/rmapi/nv_gpu_ops_stop_impl.h"

int
main(void)
{
    RM_API api = {.Control = control};
    TEST_SESSION session = {.handle = 1};
    TEST_SUBDEVICE subdevice = {.subDeviceHandle = 2};
    gpuRetainedChannel retained = {
        .session = &session,
        .rmSubDevice = &subdevice,
        .channelEngineType = UVM_GPU_CHANNEL_ENGINE_TYPE_GR,
    };
    KernelChannel channel = {
        .bIsContextBound = NV_TRUE,
        .client = 3,
        .handle = 4,
    };
    controlled_channel = &channel;

    stop_status = NV_ERR_OPERATING_SYSTEM;
#if defined(TEST_LINUX)
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_OK);
    assert(channel.bIsContextBound == NV_FALSE);
    assert(stop_count == 1 && evict_count == 1);
    channel.bIsContextBound = NV_TRUE;
#else
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_ERR_OPERATING_SYSTEM);
    assert(channel.bIsContextBound == NV_TRUE);
    assert(stop_count == 1 && evict_count == 0 && seen_immediate == NV_FALSE);
#endif

    stop_status = NV_OK;
    evict_status = NV_ERR_OPERATING_SYSTEM;
#if defined(TEST_LINUX)
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_OK);
#else
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_ERR_OPERATING_SYSTEM);
#endif
#if defined(TEST_LINUX)
    assert(channel.bIsContextBound == NV_FALSE);
    assert(stop_count == 2 && evict_count == 2);
    channel.bIsContextBound = NV_TRUE;
#else
    assert(channel.bIsContextBound == NV_TRUE);
    assert(stop_count == 2 && evict_count == 1);
#endif

    evict_status = NV_OK;
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_OK);
    assert(channel.bIsContextBound == NV_FALSE);
#if defined(TEST_LINUX)
    assert(stop_count == 3 && evict_count == 3);
#else
    assert(stop_count == 3 && evict_count == 2);
#endif

    channel.bIsContextBound = NV_TRUE;
    retained.channelEngineType = 99;
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_OK);
    assert(channel.bIsContextBound == NV_FALSE);
    assert(stop_count == 4);

#if !defined(TEST_LINUX)
    /* Revocation must precede RM stop, including on failure. */
    channel.bIsContextBound = NV_TRUE;
    retained.channelEngineType = UVM_GPU_CHANNEL_ENGINE_TYPE_GR;
    stop_status = NV_ERR_OPERATING_SYSTEM;
    assert(nvGpuOpsRevokeChannelControl(&api, &channel, &retained)
           == NV_ERR_OPERATING_SYSTEM);
    assert(revoked_before_stop == NV_TRUE);
    assert(channel.uvmRevoked == 1);
    assert(channel.bIsContextBound == NV_TRUE);

    stop_status = NV_OK;
    evict_status = NV_ERR_OPERATING_SYSTEM;
    assert(nvGpuOpsRevokeChannelControl(&api, &channel, &retained)
           == NV_ERR_OPERATING_SYSTEM);
    assert(channel.uvmRevoked == 1);
    assert(channel.bIsContextBound == NV_TRUE);

    evict_status = NV_OK;
    assert(nvGpuOpsRevokeChannelControl(&api, &channel, &retained) == NV_OK);
    assert(channel.bIsContextBound == NV_FALSE);
    assert(channel.uvmRevoked == 3);

    /* A retry after a transient scan failure must not stop it twice. */
    {
        int completed_stops = stop_count;
        int completed_evictions = evict_count;
        assert(nvGpuOpsRevokeChannelControl(&api, &channel, &retained) == NV_OK);
        assert(stop_count == completed_stops);
        assert(evict_count == completed_evictions);
    }

    /* Ordinary UVM stop does not revoke a reusable channel. */
    channel.uvmRevoked = 0;
    channel.bIsContextBound = NV_TRUE;
    assert(nvGpuOpsStopChannelControl(&api, &channel, &retained,
                                      NV_FALSE) == NV_OK);
    assert(channel.uvmRevoked == 0);
#endif
    return 0;
}
