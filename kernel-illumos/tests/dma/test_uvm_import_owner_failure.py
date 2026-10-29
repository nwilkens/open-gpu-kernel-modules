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
"""Inject failures into the actual UVM import-owner revocation function."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "kernel-open/nvidia-uvm/uvm_map_external.c"


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


revoke = function_body(
    SOURCE.read_text(), "static NV_STATUS uvm_ext_gpu_import_owner_revoke(void *opaque)\n{"
)

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

typedef int NV_STATUS;
typedef struct { int value; } atomic_t;
typedef struct { int unused; } uvm_va_space_t;
typedef struct { int unused; } uvm_gpu_t;
typedef struct { int unused; } uvm_ext_gpu_mem_handle;
typedef struct uvm_ext_gpu_import_owner {
    atomic_t state;
    NV_STATUS failure;
    uvm_va_space_t *va_space;
    uvm_gpu_t *gpu;
    uvm_ext_gpu_mem_handle *mem_handle;
    void *token;
    int state_waitq;
} uvm_ext_gpu_import_owner;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_BUSY_RETRY 2
#define NV_WARN_MORE_PROCESSING_REQUIRED 3
#define UVM_IMPORT_BUILDING 10
#define UVM_IMPORT_ACTIVE 11
#define UVM_IMPORT_NORMAL_FRAGMENT 12
#define UVM_IMPORT_NORMAL_FINALIZING 13
#define UVM_IMPORT_REVOKING 14
#define UVM_IMPORT_COMPLETE 15
#define UVM_IMPORT_FAILED 16
#define UVM_ASSERT(expr) assert(expr)
#define uvm_rm_locked_call(expr) (expr)
#define wait_event(queue, condition) assert(0 && "unexpected wait")

static NV_STATUS revoke_result, hardware_result, guard_result;
static int revoke_calls, hardware_calls, guard_calls, failed_calls, finish_calls;
static int schedule_calls, hardware_busy_once;
static int atomic_read(atomic_t *value) { return value->value; }
static int atomic_cmpxchg(atomic_t *value, int old, int replacement)
{ int prior = value->value; if (prior == old) value->value = replacement; return prior; }
static NV_STATUS uvm_ext_gpu_import_revoke_checked(uvm_va_space_t *vs,
                                                   uvm_ext_gpu_mem_handle *handle)
{ assert(vs && handle); ++revoke_calls; return revoke_result; }
static NV_STATUS nvUvmInterfaceImportHardwareComplete(void *token)
{
    assert(token);
    ++hardware_calls;
    if (hardware_busy_once) { hardware_busy_once = 0; return NV_ERR_BUSY_RETRY; }
    return hardware_result;
}
static void *uvm_gpu_device_handle(uvm_gpu_t *gpu) { return gpu; }
static NV_STATUS nvUvmInterfaceReleaseImportGuard(void *gpu, void *token)
{ assert(gpu && token); ++guard_calls; return guard_result; }
static void schedule(void) { ++schedule_calls; }
static void uvm_import_owner_mark_failed(uvm_ext_gpu_import_owner *owner,
                                         NV_STATUS status)
{ ++failed_calls; owner->failure = status; owner->state.value = UVM_IMPORT_FAILED; }
static void uvm_import_owner_finish(uvm_ext_gpu_import_owner *owner,
                                    int defer_free)
{ assert(defer_free); ++finish_calls; owner->state.value = UVM_IMPORT_COMPLETE; }

''' + revoke + r'''

static void reset_results(void)
{
    revoke_result = hardware_result = guard_result = NV_OK;
    revoke_calls = hardware_calls = guard_calls = failed_calls = finish_calls = 0;
    schedule_calls = hardware_busy_once = 0;
}

static void init_owner(uvm_ext_gpu_import_owner *owner, uvm_va_space_t *vs,
                       uvm_gpu_t *gpu, uvm_ext_gpu_mem_handle *handle, int *token)
{
    *owner = (uvm_ext_gpu_import_owner){0};
    owner->state.value = UVM_IMPORT_ACTIVE;
    owner->va_space = vs;
    owner->gpu = gpu;
    owner->mem_handle = handle;
    owner->token = token;
}

int main(void)
{
    uvm_ext_gpu_import_owner owner;
    uvm_va_space_t vs = {0};
    uvm_gpu_t gpu = {0};
    uvm_ext_gpu_mem_handle handle = {0};
    int token;

    reset_results();
    init_owner(&owner, &vs, &gpu, &handle, &token);
    revoke_result = NV_ERR_OPERATING_SYSTEM; /* GPU stop/PTE/IOMMU failure */
    assert(uvm_ext_gpu_import_owner_revoke(&owner) == NV_ERR_OPERATING_SYSTEM);
    assert(owner.state.value == UVM_IMPORT_FAILED && owner.mem_handle == &handle);
    assert(owner.token == &token && owner.failure == NV_ERR_OPERATING_SYSTEM);
    assert(revoke_calls == 1 && hardware_calls == 0 && guard_calls == 0);
    assert(failed_calls == 1 && finish_calls == 0);
    assert(uvm_ext_gpu_import_owner_revoke(&owner) == NV_ERR_OPERATING_SYSTEM);
    assert(revoke_calls == 1); /* Terminal failure cannot silently retry/free. */

    reset_results();
    init_owner(&owner, &vs, &gpu, &handle, &token);
    hardware_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_import_owner_revoke(&owner) == NV_ERR_OPERATING_SYSTEM);
    assert(owner.state.value == UVM_IMPORT_FAILED && owner.mem_handle == NULL);
    assert(owner.token == &token && guard_calls == 0 && finish_calls == 0);

    reset_results();
    init_owner(&owner, &vs, &gpu, &handle, &token);
    guard_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_import_owner_revoke(&owner) == NV_ERR_OPERATING_SYSTEM);
    assert(owner.state.value == UVM_IMPORT_FAILED && owner.token == &token);
    assert(hardware_calls == 1 && guard_calls == 1 && finish_calls == 0);

    reset_results();
    init_owner(&owner, &vs, &gpu, &handle, &token);
    hardware_busy_once = 1;
    guard_result = NV_WARN_MORE_PROCESSING_REQUIRED;
    assert(uvm_ext_gpu_import_owner_revoke(&owner) == NV_OK);
    assert(owner.state.value == UVM_IMPORT_COMPLETE && finish_calls == 1);
    assert(hardware_calls == 2 && schedule_calls == 1 && guard_calls == 1);
    puts("UVM import-owner failure injection passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    c_file = Path(temp) / "owner_failure.c"
    binary = Path(temp) / "owner_failure"
    c_file.write_text(harness)
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
         str(c_file), "-o", str(binary)], check=True
    )
    subprocess.run([str(binary)], check=True)
