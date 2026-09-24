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
 * The UVM module parameters, which are nvidia_uvm driver properties.  Every
 * module_param() in the UVM sources must be listed here with its type, or
 * the source does not compile.  The bounds apply to integer values; for
 * charp they bound the length.  They are the type's range except where UVM
 * uses a value without checking it: uvm_perf_pma_batch_nonpinned_order is
 * a shift count, and uvm_perf_thrashing_lapse_usec times 1000 times the
 * epoch or pin multiplier must fit in 64 bits.  500 ms is the largest lapse
 * UVM's own adjustment reaches.
 */

#ifndef _UVM_KPI_PARAMS_H_
#define _UVM_KPI_PARAMS_H_

#define UVM_PARAMS(P)                                                                   \
    P(uvm_ats_mode,                                 int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_block_cpu_to_cpu_copy_with_ce,            int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_channel_gpfifo_loc,                       charp,  0,          255)            \
    P(uvm_channel_gpput_loc,                        charp,  0,          255)            \
    P(uvm_channel_num_gpfifo_entries,               uint,   0,          UINT32_MAX)     \
    P(uvm_channel_pushbuffer_loc,                   charp,  0,          255)            \
    P(uvm_conf_computing_channel_iv_rotation_limit, ulong,  0,          UINT64_MAX)     \
    P(uvm_cpu_chunk_allocation_sizes,               uint,   0,          UINT32_MAX)     \
    P(uvm_debug_enable_push_acquire_info,           uint,   0,          UINT32_MAX)     \
    P(uvm_debug_enable_push_desc,                   uint,   0,          UINT32_MAX)     \
    P(uvm_debug_prints,                             int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_disable_hmm,                              bool,   0,          1)              \
    P(uvm_disable_sam_migration,                    bool,   0,          1)              \
    P(uvm_downgrade_force_membar_sys,               uint,   0,          UINT32_MAX)     \
    P(uvm_enable_builtin_tests,                     int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_enable_va_space_mm,                       int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_exp_gpu_cache_peermem,                    uint,   0,          UINT32_MAX)     \
    P(uvm_exp_gpu_cache_sysmem,                     uint,   0,          UINT32_MAX)     \
    P(uvm_fault_force_sysmem,                       int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_force_conf_computing,                     uint,   0,          UINT32_MAX)     \
    P(uvm_global_oversubscription,                  int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_leak_checker,                             int,    0,          2)              \
    P(uvm_page_table_location,                      charp,  0,          255)            \
    P(uvm_peer_copy,                                charp,  0,          255)            \
    P(uvm_perf_access_counter_batch_count,          uint,   0,          UINT32_MAX)     \
    P(uvm_perf_access_counter_migration_enable,     int,    -1,         1)              \
    P(uvm_perf_access_counter_threshold,            uint,   0,          UINT32_MAX)     \
    P(uvm_perf_fault_batch_count,                   uint,   0,          UINT32_MAX)     \
    P(uvm_perf_fault_coalesce,                      uint,   0,          UINT32_MAX)     \
    P(uvm_perf_fault_max_batches_per_service,       uint,   0,          UINT32_MAX)     \
    P(uvm_perf_fault_max_throttle_per_service,      uint,   0,          UINT32_MAX)     \
    P(uvm_perf_fault_replay_policy,                 uint,   0,          UINT32_MAX)     \
    P(uvm_perf_fault_replay_update_put_ratio,       uint,   0,          UINT32_MAX)     \
    P(uvm_perf_lapse_vote_threshold,                uint,   0,          UINT32_MAX)     \
    P(uvm_perf_map_remote_on_eviction,              int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_perf_map_remote_on_native_atomics_fault,  uint,   0,          UINT32_MAX)     \
    P(uvm_perf_migrate_cpu_preunmap_block_order,    uint,   0,          UINT32_MAX)     \
    P(uvm_perf_migrate_cpu_preunmap_enable,         int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_perf_pma_batch_nonpinned_order,           uint,   0,          10)             \
    P(uvm_perf_prefetch_enable,                     uint,   0,          UINT32_MAX)     \
    P(uvm_perf_prefetch_min_faults,                 uint,   0,          UINT32_MAX)     \
    P(uvm_perf_prefetch_threshold,                  uint,   0,          UINT32_MAX)     \
    P(uvm_perf_reenable_prefetch_faults_lapse_msec, uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_enable,                    uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_epoch,                     uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_lapse_usec,                uint,   0,          500000)         \
    P(uvm_perf_thrashing_max_resets,                uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_nap,                       uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_pin,                       uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_pin_threshold,             uint,   0,          UINT32_MAX)     \
    P(uvm_perf_thrashing_threshold,                 uint,   0,          UINT32_MAX)     \
    P(uvm_release_asserts,                          int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_release_asserts_dump_stack,               int,    INT32_MIN,  INT32_MAX)      \
    P(uvm_release_asserts_set_global_error,         int,    INT32_MIN,  INT32_MAX)

typedef enum linux_param_type {
    LINUX_PARAM_TYPE_int,
    LINUX_PARAM_TYPE_uint,
    LINUX_PARAM_TYPE_ulong,
    LINUX_PARAM_TYPE_bool,
    LINUX_PARAM_TYPE_charp,
} linux_param_type_t;

/* module_param() defines one of these, nv_uvm_param_<name>, per parameter. */
struct linux_param_var {
    void           *lpv_addr;
};

#endif /* _UVM_KPI_PARAMS_H_ */
