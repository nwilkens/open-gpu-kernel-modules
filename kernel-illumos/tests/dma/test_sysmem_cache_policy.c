/*
 * SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdint.h>

typedef uint32_t NvU32;

#define NV_MEMORY_CACHED 0
#define NV_MEMORY_UNCACHED 1
#define NV_MEMORY_WRITECOMBINED 2
#define NV_MEMORY_UNCACHED_WEAK 3
#define NV_MEMORY_DEFAULT 4

#include "../../nvidia/nvidia_mem_cache_policy.h"

int
main(void)
{
    /* Driver-owned RAM must match illumos's default cached KPM alias. */
    assert(nv_sysmem_effective_cache_type(NV_MEMORY_WRITECOMBINED) ==
        NV_MEMORY_CACHED);

    /* The experiment must not alter the other RM memory requests. */
    assert(nv_sysmem_effective_cache_type(NV_MEMORY_CACHED) ==
        NV_MEMORY_CACHED);
    assert(nv_sysmem_effective_cache_type(NV_MEMORY_UNCACHED) ==
        NV_MEMORY_UNCACHED);
    assert(nv_sysmem_effective_cache_type(NV_MEMORY_UNCACHED_WEAK) ==
        NV_MEMORY_UNCACHED_WEAK);
    assert(nv_sysmem_effective_cache_type(NV_MEMORY_DEFAULT) ==
        NV_MEMORY_DEFAULT);
    return 0;
}
