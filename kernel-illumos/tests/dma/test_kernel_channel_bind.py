#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Carlos Neira
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
"""Check the real RM channel BIND path before its GSP scheduling RPC."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src/nvidia/src/kernel/gpu/fifo/kernel_channel.c"
GROUP = ROOT / "src/nvidia/src/kernel/gpu/fifo/kernel_channel_group_api.c"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"unterminated function: {signature}")


bind = function_body(SOURCE.read_text(), "NV_STATUS kchannelBindToRunlist_IMPL")
direct_bind = function_body(SOURCE.read_text(), "NV_STATUS\nkchannelCtrlCmdBind_IMPL")
group_bind = function_body(GROUP.read_text(), "NV_STATUS\nkchangrpapiCtrlCmdBind_IMPL")
harness = r'''
#include <assert.h>
#include <stdio.h>

typedef int NV_STATUS;
typedef int NvBool;
typedef unsigned int NvU32;
typedef int RM_ENGINE_TYPE;
typedef int ENGDESCRIPTOR;
typedef struct OBJGPU { int unused; } OBJGPU;
typedef struct KernelFifo { int unused; } KernelFifo;
typedef struct KernelMIGManager { int unused; } KernelMIGManager;
typedef struct Device { int unused; } Device;
typedef int MIG_INSTANCE_REF;
typedef struct OBJGVASPACE {
    int external;
    volatile NvU32 uvmRevoked;
} OBJGVASPACE;
typedef struct KernelChannel {
    OBJGVASPACE *pVAS;
    struct KernelChannelGroupApi *pKernelChannelGroupApi;
    volatile NvU32 uvmRevoked;
    NvBool bUvmOwned;
    NvBool bIsContextBound;
} KernelChannel;
typedef struct NVA06F_CTRL_BIND_PARAMS { NvU32 engineType; } NVA06F_CTRL_BIND_PARAMS;
typedef struct NVA06C_CTRL_BIND_PARAMS { NvU32 engineType; } NVA06C_CTRL_BIND_PARAMS;
typedef struct CHANNEL_NODE {
    KernelChannel *pKernelChannel;
    struct CHANNEL_NODE *pNext;
} CHANNEL_NODE;
typedef struct CHANNEL_LIST { CHANNEL_NODE *pHead; } CHANNEL_LIST;
typedef struct KernelChannelGroup {
    CHANNEL_LIST *pChanList;
    int grpID;
    int runlistId;
    int bAllocatedByRm;
} KernelChannelGroup;
typedef struct KernelChannelGroupApi {
    KernelChannelGroup *pKernelChannelGroup;
} KernelChannelGroupApi;

#define NV_OK 0
#define NV_ERR_INVALID_ARGUMENT 1
#define NV_ERR_INVALID_STATE 2
#define NV_TRUE 1
#define ENG_SW 3
#define ENG_BUS 4
#define LEVEL_ERROR 0
#define LEVEL_INFO 1
#define FMT_CHANNEL_DEBUG_TAG "%u"
#define NV_ASSERT_OR_RETURN(expr, result) do { if (!(expr)) return result; } while (0)
#define NV_ASSERT_OK_OR_RETURN(status) do { if ((status) != NV_OK) return status; } while (0)
#define NV_ASSERT_OK_OR_CAPTURE_FIRST_ERROR(result, call) \
    do { NV_STATUS s = (call); if (s != NV_OK) ++asserted_failures; \
         if ((result) == NV_OK) (result) = s; } while (0)
#define NV_CHECK_OK_OR_RETURN(level, call) \
    do { (void)(level); NV_STATUS s = (call); if (s != NV_OK) return s; } while (0)
#define NV_PRINTF(level, format, ...) ((void)(level), (void)(format))
#define dynamicCast(value, type) (value)
#define GPU_RES_GET_GPU(channel) (&gpu)
#define GPU_GET_KERNEL_FIFO(gpu_ptr) (&fifo)
#define GPU_RES_GET_DEVICE(group) (&device)
#define GPU_GET_KERNEL_MIG_MANAGER(gpu_ptr) (&mig)
#define IS_GSP_CLIENT(gpu_ptr) 1
#define IS_VIRTUAL(gpu_ptr) 0
#define IS_VIRTUAL_WITH_FULL_SRIOV(gpu_ptr) 0
#define IS_MIG_IN_USE(gpu_ptr) 0
#define SLI_LOOP_FLAGS_BC_ONLY 0
#define SLI_LOOP_START(flags) do {
#define SLI_LOOP_BREAK break
#define SLI_LOOP_END } while (0)
#define RES_GET_CLIENT_HANDLE(channel) 1
#define RES_GET_HANDLE(channel) 2
#define NVA06F_CTRL_CMD_BIND 3
#define ENGINE_INFO_TYPE_ENG_DESC 5
#define ENGINE_INFO_TYPE_RUNLIST 6
#define kchannelBindToRunlist kchannelBindToRunlist_IMPL
#define NV_RM_RPC_CONTROL(gpu_ptr, client, handle, cmd, params, size, result) \
    do { (void)(gpu_ptr); (void)(client); (void)(handle); (void)(cmd); \
         (void)(params); (void)(size); ++rpc_calls; (result) = NV_OK; } while (0)

static OBJGPU gpu;
static KernelFifo fifo;
static KernelMIGManager mig;
static Device device;
static int rpc_calls;
static int asserted_failures;
static __attribute__((unused)) NvU32 portAtomicOrU32(volatile NvU32 *v, NvU32 bits)
{ NvU32 old = *v; *v |= bits; return old; }
static __attribute__((unused)) NvBool gvaspaceIsExternallyOwned(OBJGVASPACE *vas)
{ return vas->external; }
static NvU32 gpuGetNv2080EngineType(RM_ENGINE_TYPE engine) { return (NvU32)engine; }
static RM_ENGINE_TYPE gpuGetRmEngineType(NvU32 engine) { return (RM_ENGINE_TYPE)engine; }
static NV_STATUS kmigmgrGetInstanceRefFromDevice(OBJGPU *gpu_ptr,
                                                 KernelMIGManager *mgr,
                                                 Device *dev, MIG_INSTANCE_REF *ref)
{ (void)gpu_ptr; (void)mgr; (void)dev; *ref = 0; return NV_OK; }
static NV_STATUS kmigmgrGetLocalToGlobalEngineType(OBJGPU *gpu_ptr,
                                                   KernelMIGManager *mgr,
                                                   MIG_INSTANCE_REF ref,
                                                   RM_ENGINE_TYPE local,
                                                   RM_ENGINE_TYPE *global)
{ (void)gpu_ptr; (void)mgr; (void)ref; *global = local; return NV_OK; }
static NV_STATUS gpuXlateClientEngineIdToEngDesc(OBJGPU *gpu_ptr,
                                                 RM_ENGINE_TYPE type,
                                                 ENGDESCRIPTOR *desc)
{ (void)gpu_ptr; *desc = type; return NV_OK; }
static NV_STATUS kfifoEngineInfoXlate_HAL(OBJGPU *gpu_ptr, KernelFifo *fifo_ptr,
                                          int from_type, ENGDESCRIPTOR desc,
                                          int to_type, int *runlist)
{ (void)gpu_ptr; (void)fifo_ptr; (void)from_type; (void)to_type;
  *runlist = desc; return NV_OK; }
static __attribute__((unused)) NvU32 kchannelGetDebugTag(KernelChannel *channel)
{ (void)channel; return 0; }
static NvBool kfifoIsPerRunlistChramEnabled(KernelFifo *unused)
{ (void)unused; return 0; }
static NV_STATUS kfifoRunlistSetIdByEngine_HAL(OBJGPU *gpu_ptr, KernelFifo *fifo_ptr,
                                               KernelChannel *channel, ENGDESCRIPTOR engine)
{ (void)gpu_ptr; (void)fifo_ptr; (void)channel; (void)engine; return NV_OK; }

''' + bind + '\n' + direct_bind + '\n' + group_bind + r'''

static void expect(const char *case_name, KernelChannel *channel,
                   NV_STATUS wanted_status, int wanted_rpc_calls)
{
    rpc_calls = 0;
    NV_STATUS actual = kchannelBindToRunlist_IMPL(channel, 1, 1);
    if (actual != wanted_status || rpc_calls != wanted_rpc_calls) {
        fprintf(stderr, "%s: status %d (want %d), RPCs %d (want %d)\n",
                case_name, actual, wanted_status, rpc_calls, wanted_rpc_calls);
        assert(actual == wanted_status && rpc_calls == wanted_rpc_calls);
    }
}

int main(void)
{
    OBJGVASPACE vas = {.external = 1};
    KernelChannel channel = {.pVAS = &vas};
#ifdef NV_SUNOS
    expect("raw user BIND on UVM VA", &channel, NV_ERR_INVALID_STATE, 0);
#else
    expect("Linux raw BIND unchanged", &channel, NV_OK, 1);
#endif
    channel.bUvmOwned = 1;
    expect("UVM-owned BIND", &channel, NV_OK, 1);
    channel.bUvmOwned = 0;
    channel.bIsContextBound = 1;
    expect("registered user BIND", &channel, NV_OK, 1);
    channel.bIsContextBound = 0;
    vas.external = 0;
    expect("ordinary VA BIND", &channel, NV_OK, 1);
#ifdef NV_SUNOS
    vas.external = 1;
    channel.uvmRevoked = 1;
    expect("revoked channel BIND", &channel, NV_ERR_INVALID_STATE, 0);
    channel.uvmRevoked = 0;
    vas.uvmRevoked = 1;
    expect("revoked VA BIND", &channel, NV_ERR_INVALID_STATE, 0);
#endif

    /* The first channel is allowed; the second is an unregistered raw user. */
    vas.uvmRevoked = 0;
    channel.bIsContextBound = 1;
    OBJGVASPACE raw_vas = {.external = 1};
    KernelChannel raw = {.pVAS = &raw_vas};
    CHANNEL_NODE second = {.pKernelChannel = &raw};
    CHANNEL_NODE first = {.pKernelChannel = &channel, .pNext = &second};
    CHANNEL_LIST list = {.pHead = &first};
    KernelChannelGroup group = {.pChanList = &list};
    KernelChannelGroupApi api = {.pKernelChannelGroup = &group};
    raw.pKernelChannelGroupApi = &api;
    NVA06C_CTRL_BIND_PARAMS params = {.engineType = 1};
    rpc_calls = 0;
    NV_STATUS group_status = kchangrpapiCtrlCmdBind_IMPL(&api, &params);
#ifdef NV_SUNOS
    if (group_status != NV_ERR_INVALID_STATE || rpc_calls != 0)
        fprintf(stderr, "mixed group BIND: status %d, RPCs %d\n",
                group_status, rpc_calls);
    assert(group_status == NV_ERR_INVALID_STATE && rpc_calls == 0);
#else
    assert(group_status == NV_OK && rpc_calls == 2);
#endif

    first.pKernelChannel = &raw;
    second.pKernelChannel = &channel;
    rpc_calls = 0;
    group_status = kchangrpapiCtrlCmdBind_IMPL(&api, &params);
#ifdef NV_SUNOS
    assert(group_status == NV_ERR_INVALID_STATE && rpc_calls == 0);
#else
    assert(group_status == NV_OK && rpc_calls == 2);
#endif
    raw.bUvmOwned = 1;
    rpc_calls = 0;
    group_status = kchangrpapiCtrlCmdBind_IMPL(&api, &params);
    assert(group_status == NV_OK && rpc_calls == 2);
    raw.bUvmOwned = 0;
    raw.bIsContextBound = 1;
    rpc_calls = 0;
    group_status = kchangrpapiCtrlCmdBind_IMPL(&api, &params);
    assert(group_status == NV_OK && rpc_calls == 2);

    raw.bIsContextBound = 0;
    rpc_calls = 0;
    asserted_failures = 0;
    NVA06F_CTRL_BIND_PARAMS direct_params = {.engineType = 1};
    NV_STATUS direct_status = kchannelCtrlCmdBind_IMPL(&raw, &direct_params);
#ifdef NV_SUNOS
    assert(direct_status == NV_ERR_INVALID_STATE && rpc_calls == 0);
    assert(asserted_failures == 0);
#else
    assert(direct_status == NV_OK && rpc_calls == 1);
    assert(asserted_failures == 0);
#endif
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    c_file = Path(temp) / "bind.c"
    c_file.write_text(harness)
    for platform in ("illumos", "linux"):
        binary = Path(temp) / platform
        command = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG"]
        if platform == "illumos":
            command.append("-DNV_SUNOS")
        command.extend([str(c_file), "-o", str(binary)])
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
