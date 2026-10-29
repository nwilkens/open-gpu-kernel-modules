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
"""Exercise the real RM admission function for user and UVM-owned channels."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
CHANNEL = ROOT / "src/nvidia/src/kernel/gpu/fifo/kernel_channel.c"
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


channel_source = CHANNEL.read_text()
group_source = GROUP.read_text()
admission = function_body(channel_source, "NvBool\nkchannelIsSchedulable_IMPL")
channel_schedule = function_body(channel_source, "NV_STATUS\nkchannelCtrlCmdGpFifoSchedule_IMPL")
group_schedule = function_body(group_source, "NV_STATUS\nkchangrpapiCtrlCmdGpFifoSchedule_IMPL")
assert "kchannelIsSchedulable_HAL(pGpu, pKernelChannel)" in channel_schedule
assert "kchannelIsSchedulable_HAL(pGpu, pChanNode->pKernelChannel)" in group_schedule

harness = r'''
#include <assert.h>
#include <stdio.h>

typedef int NvBool;
typedef unsigned int NvU32;
typedef int NV_STATUS;
typedef struct OBJGPU { int unused; } OBJGPU;
typedef struct OBJGVASPACE {
    int external;
    volatile NvU32 uvmRevoked;
} OBJGVASPACE;
typedef struct KernelChannel {
    OBJGVASPACE *pVAS;
    volatile NvU32 uvmRevoked;
    NvBool bUvmOwned;
    NvBool bIsContextBound;
    NvU32 gfid;
    NvU32 engine;
} KernelChannel;

#define NV_TRUE 1
#define NV_FALSE 0
#define NV_OK 0
#define LEVEL_INFO 0
#define LEVEL_ERROR 0
#define FMT_CHANNEL_DEBUG_TAG "%u"
#define NV_PRINTF(level, format, tag) ((void)(level), (void)(format), (void)(tag))
#define NV_ASSERT_OR_RETURN(expr, result) do { if (!(expr)) return result; } while (0)
#define dynamicCast(value, type) (value)
#define IS_GFID_VF(value) ((value) == 1)
#define IS_MODS_AMODEL(gpu) 0
#define IS_GR(value) ((value) == 1)
static __attribute__((unused)) NvU32 portAtomicOrU32(volatile NvU32 *value, NvU32 mask)
{ NvU32 old = *value; *value |= mask; return old; }
static NvU32 kchannelGetGfid(KernelChannel *channel) { return channel->gfid; }
static NvU32 kchannelGetDebugTag(KernelChannel *channel)
{ (void)channel; return 0; }
static NV_STATUS kchannelGetEngine_HAL(OBJGPU *gpu, KernelChannel *channel, NvU32 *engine)
{ (void)gpu; *engine = channel->engine; return NV_OK; }
static NvBool gvaspaceIsExternallyOwned(OBJGVASPACE *vas) { return vas->external; }

''' + admission + r'''

static void expect(const char *name, OBJGPU *gpu, KernelChannel *channel, int want)
{
    int actual = kchannelIsSchedulable_IMPL(gpu, channel);
    if (actual != want) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, want, actual);
        assert(actual == want);
    }
}

int main(void)
{
    OBJGPU gpu = {0};
    OBJGVASPACE vas = {.external = 1};
    KernelChannel channel = {.pVAS = &vas, .engine = 2}; /* CE */
#ifdef NV_SUNOS
    expect("raw user CE", &gpu, &channel, NV_FALSE);
    channel.gfid = 1;
    expect("raw user VF CE", &gpu, &channel, NV_FALSE);
    channel.gfid = 0;
#else
    expect("Linux raw CE", &gpu, &channel, NV_TRUE);
#endif
    channel.bUvmOwned = 1;
    expect("unbound kernel UVM CE", &gpu, &channel, NV_TRUE);
    channel.bUvmOwned = 0;
    channel.bIsContextBound = 1;
    expect("bound user CE", &gpu, &channel, NV_TRUE);
#ifdef NV_SUNOS
    channel.gfid = 1;
    expect("bound user VF CE", &gpu, &channel, NV_TRUE);
    channel.gfid = 0;
#endif
    channel.bIsContextBound = 0;
    channel.engine = 1; /* GR */
    expect("raw user GR", &gpu, &channel, NV_FALSE);
    channel.engine = 2;
    vas.external = 0;
    expect("non-external CE", &gpu, &channel, NV_TRUE);
#ifdef NV_SUNOS
    vas.external = 1;
    channel.bIsContextBound = 1;
    channel.uvmRevoked = 1;
    expect("sticky channel revoke", &gpu, &channel, NV_FALSE);
    channel.uvmRevoked = 0;
    vas.uvmRevoked = 1;
    expect("sticky VAS revoke", &gpu, &channel, NV_FALSE);
#endif
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    c_file = Path(temp) / "admission.c"
    c_file.write_text(harness)
    for platform in ("illumos", "linux"):
        binary = Path(temp) / platform
        command = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG"]
        if platform == "illumos":
            command.append("-DNV_SUNOS")
        command.extend([str(c_file), "-o", str(binary)])
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
