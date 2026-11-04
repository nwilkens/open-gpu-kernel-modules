#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC
# SPDX-License-Identifier: MIT
"""Execute the real map ioctl admission paths before their first map change."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
MAP_SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "kernel-open/nvidia-uvm/uvm_map_external.c"
GUARD_SOURCE = ROOT / "kernel-open/nvidia-uvm/uvm_va_range_deferred_external_impl.h"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start:offset + 1]
    raise ValueError(f"unterminated function: {signature}")


def admission_prefix(source: str, signature: str, next_map_step: str) -> str:
    start = source.index(signature)
    stop = source.index(next_map_step, start)
    return source[start:stop]


source = MAP_SOURCE.read_text()
guard = function_body(GUARD_SOURCE.read_text(), "NV_STATUS uvm_va_range_external_map_status(")
ordinary = admission_prefix(
    source,
    "static NV_STATUS uvm_map_external_allocation(uvm_va_space_t *va_space,",
    "    uvm_processor_mask_zero(mapped_gpus);",
)
sparse = admission_prefix(
    source,
    "static NV_STATUS uvm_map_external_sparse(uvm_va_space_t *va_space,",
    "    mapping_gpu = uvm_va_space_get_gpu_by_uuid_with_gpu_va_space(",
)

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

typedef int NV_STATUS;
typedef unsigned long long NvU64;
#define NV_OK 0
#define NV_ERR_INVALID_STATE 2
#define NV_ERR_INVALID_ARGUMENT 3
#define NV_ERR_INVALID_ADDRESS 4
#define NV_ERR_NO_MEMORY 5
#define UVM_MAX_GPUS 8
#define UVM_TRACKER_INIT() {0}
#define LIST_HEAD(name) int name = 0

typedef struct { int unused; } uvm_gpu_t;
typedef struct { int unused; } uvm_processor_mask_t;
typedef struct { int unused; } uvm_map_rm_params_t;
typedef struct { int unused; } uvm_tracker_t;
typedef struct {
    int rm_control_fd, user_client, user_object;
} uvm_rm_user_object_t;
typedef struct {
    struct { NvU64 end; } node;
} uvm_va_range_t;
typedef struct {
    uvm_va_range_t va_range;
    bool deferred_free_pending;
} uvm_va_range_external_t;
typedef struct {
    int user_channels_revoking, tracked_imports_blocked;
    int read_locked;
} uvm_va_space_t;
typedef struct {
    NvU64 base, length;
    size_t gpuAttributesCount;
    int rmCtrlFd, hClient, hMemory;
} UVM_MAP_EXTERNAL_ALLOCATION_PARAMS;
typedef struct {
    NvU64 base, length;
} UVM_MAP_EXTERNAL_SPARSE_PARAMS;

static uvm_va_range_external_t range;
static int allocations;
static int map_steps;
static bool uvm_api_range_invalid_4k(NvU64 base, NvU64 len)
{ return (base % 4096) != 0 || len == 0 || (len % 4096) != 0; }
static bool uvm_api_range_invalid_64k(NvU64 base, NvU64 len)
{ return (base % 65536) != 0 || len == 0 || (len % 65536) != 0; }
static uvm_processor_mask_t *uvm_processor_mask_cache_alloc(void)
{ ++allocations; return malloc(sizeof(uvm_processor_mask_t)); }
static void uvm_processor_mask_cache_free(uvm_processor_mask_t *mask)
{ assert(allocations == 1); --allocations; free(mask); }
static void uvm_va_space_down_read_rm(uvm_va_space_t *vs)
{ assert(vs->read_locked == 0); vs->read_locked = 1; }
static void uvm_va_space_up_read_rm(uvm_va_space_t *vs)
{ assert(vs->read_locked == 1); vs->read_locked = 0; }
static void uvm_va_space_down_read(uvm_va_space_t *vs)
{ assert(vs->read_locked == 0); vs->read_locked = 1; }
static void uvm_va_space_up_read(uvm_va_space_t *vs)
{ assert(vs->read_locked == 1); vs->read_locked = 0; }
static bool uvm_import_tracking_enabled(void) { return true; }
static int atomic_read(int *value) { return *value; }
static uvm_va_range_external_t *uvm_va_range_external_find(uvm_va_space_t *vs, NvU64 base)
{ assert(vs->read_locked); return base == 0 ? &range : NULL; }
''' + guard + "\n" + ordinary + r'''
    ++map_steps;
    uvm_va_space_up_read_rm(va_space);
    uvm_processor_mask_cache_free(mapped_gpus);
    return NV_OK;
}
''' + sparse + r'''
    ++map_steps;
out:
    uvm_va_space_up_read(va_space);
    return status;
}

int main(void)
{
    uvm_va_space_t vs = {0};
    UVM_MAP_EXTERNAL_ALLOCATION_PARAMS ordinary_params = {
        .base = 0, .length = 65536, .gpuAttributesCount = 1,
    };
    UVM_MAP_EXTERNAL_SPARSE_PARAMS sparse_params = {.base = 0, .length = 65536};
    range.va_range.node.end = 65535;
    range.deferred_free_pending = true;

    assert(uvm_map_external_allocation(&vs, &ordinary_params) == NV_ERR_INVALID_STATE);
    assert(uvm_map_external_sparse(&vs, &sparse_params) == NV_ERR_INVALID_STATE);
    assert(map_steps == 0 && allocations == 0 && vs.read_locked == 0);

    range.deferred_free_pending = false;
    assert(uvm_map_external_allocation(&vs, &ordinary_params) == NV_OK);
    assert(uvm_map_external_sparse(&vs, &sparse_params) == NV_OK);
    assert(map_steps == 2 && allocations == 0 && vs.read_locked == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp_dir:
    path = Path(temp_dir)
    source_file = path / "test.c"
    executable = path / "test"
    source_file.write_text(harness)
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-o", str(executable), str(source_file)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("UVM pending-map ioctl admission passed")
