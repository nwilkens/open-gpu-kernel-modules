/*
 * SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC.
 * SPDX-License-Identifier: MIT
 */

#ifndef _NVIDIA_MEM_CACHE_POLICY_H
#define _NVIDIA_MEM_CACHE_POLICY_H

/*
 * Experimental policy for driver-owned DMA RAM. On illumos x86, KPM maps RAM
 * through a cached direct-map alias. DDI's WC allocation left that alias
 * cached while the DDI and user mappings were WC on the tested host. Use a
 * consistent cached type while the corruption hypothesis is investigated.
 *
 * Do not use this for BARs, peer I/O, or aliased physical pages: the driver
 * does not own those pages or their other mappings.
 */
static inline NvU32
nv_sysmem_effective_cache_type(NvU32 requested)
{
    return requested == NV_MEMORY_WRITECOMBINED ?
        NV_MEMORY_CACHED : requested;
}

#endif /* _NVIDIA_MEM_CACHE_POLICY_H */
