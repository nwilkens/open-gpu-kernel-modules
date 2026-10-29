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

/* A failed channel stop must not certify a VA space as stopped. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_BUSY_RETRY 2
#define NV_WARN_MORE_PROCESSING_REQUIRED 3

typedef struct {
    int stop_result;
    int stop_calls;
    bool last_immediate;
    int revoke_result;
    int revoke_calls;
    int is_bound;
} uvm_user_channel_t;

typedef struct {
    struct {
        uvm_user_channel_t *channels;
        int channel_count;
    } registered_channels;
    void *duped_gpu_va_space;
    int rm_revoke_result;
    int rm_revoke_calls;
    int rm_verify_result;
    int rm_verify_calls;
    int rm_verify_busy_once;
} uvm_gpu_va_space_t;

typedef struct {
    uvm_gpu_va_space_t *gpus;
    int gpu_count;
    int user_channels_stopped;
    int user_channels_revoking;
    int user_channels_revoked;
    int lock;
} uvm_va_space_t;

#define for_each_gpu_va_space(gpu, va) \
    for (int gpu_index = 0; gpu_index < (va)->gpu_count && \
        (((gpu) = &(va)->gpus[gpu_index]) != NULL); gpu_index++)
#define list_for_each_entry(channel, head, member) \
    for (int channel_index = 0; channel_index < (head)->channel_count && \
        (((channel) = &(head)->channels[channel_index]) != NULL); channel_index++)

static int atomic_read(int *value) { return *value; }
static void atomic_set(int *value, int new_value) { *value = new_value; }
static void uvm_assert_rwsem_locked_read(int *lock) { assert(lock != NULL); }
static void usleep_range(unsigned int min, unsigned int max)
{
    assert(min == 10000 && max == 12000);
}
static NV_STATUS
uvm_user_channel_stop_mode_checked(uvm_user_channel_t *channel, bool immediate)
{
    channel->stop_calls++;
    channel->last_immediate = immediate;
    return channel->stop_result;
}
static NV_STATUS
uvm_user_channel_revoke_checked(uvm_user_channel_t *channel)
{
    channel->revoke_calls++;
    return channel->revoke_result;
}
static NV_STATUS __attribute__((unused))
nvUvmInterfaceRevokeAddressSpace(void *handle)
{
    uvm_gpu_va_space_t *gpu = handle;
    gpu->rm_revoke_calls++;
    return gpu->rm_revoke_result;
}
static NV_STATUS
nvUvmInterfaceVerifyVaSpaceChannelsRevoked(void *handle)
{
    uvm_gpu_va_space_t *gpu = handle;
    gpu->rm_verify_calls++;
    if (gpu->rm_verify_busy_once) {
        gpu->rm_verify_busy_once = 0;
        return NV_ERR_BUSY_RETRY;
    }
    return gpu->rm_verify_result;
}
#define uvm_rm_locked_call(expr) (expr)

#include "../../../kernel-open/nvidia-uvm/uvm_va_space_stop_all_checked_impl.h"

int
main(void)
{
    uvm_user_channel_t channels[] = {
        {.stop_result = NV_OK},
        {.stop_result = NV_ERR_OPERATING_SYSTEM},
        {.stop_result = NV_OK}
    };
    uvm_gpu_va_space_t gpu = {
        .registered_channels = {channels, 3},
    };
    uvm_va_space_t va = {.gpus = &gpu, .gpu_count = 1};
    gpu.duped_gpu_va_space = &gpu;

    assert(uvm_va_space_stop_all_user_channels_checked(&va) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(va.user_channels_stopped == 0);
    assert(channels[0].stop_calls == 1);
    assert(channels[1].stop_calls == 1);
    assert(channels[2].stop_calls == 1);
    assert(!channels[0].last_immediate && !channels[1].last_immediate);

    channels[1].stop_result = NV_OK;
    assert(uvm_va_space_stop_all_user_channels_checked(&va) == NV_OK);
    assert(va.user_channels_stopped == 1);
    assert(channels[0].stop_calls == 2);
    assert(channels[1].stop_calls == 2);
    assert(channels[2].stop_calls == 2);

    assert(uvm_va_space_stop_all_user_channels_checked(&va) == NV_OK);
    assert(channels[0].stop_calls == 2);

    va.user_channels_stopped = 0;
    channels[1].stop_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_va_space_stop_all_user_channels_status(&va, true) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(va.user_channels_stopped == 0);
    assert(channels[0].last_immediate && channels[1].last_immediate);

    /* A prior ordinary stop does not certify permanent revocation. */
    va.user_channels_stopped = 0;
    gpu.rm_revoke_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_va_space_revoke_all_user_channels_checked(&va) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(gpu.rm_revoke_calls == 1);
    assert(channels[0].revoke_calls == 0);
    assert(va.user_channels_revoking == 1);
    assert(va.user_channels_revoked == 0);

    gpu.rm_revoke_result = NV_OK;
    channels[1].revoke_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_va_space_revoke_all_user_channels_checked(&va) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(gpu.rm_revoke_calls == 2);
    assert(channels[0].revoke_calls == 1);
    assert(channels[1].revoke_calls == 1);
    assert(channels[2].revoke_calls == 1);
    assert(va.user_channels_revoking == 1);
    assert(va.user_channels_stopped == 0);
    assert(va.user_channels_revoked == 0);
    channels[1].revoke_result = NV_OK;
    gpu.rm_verify_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_va_space_revoke_all_user_channels_checked(&va) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(gpu.rm_verify_calls == 1);
    assert(va.user_channels_revoked == 0);
    gpu.rm_verify_result = NV_OK;
    gpu.rm_verify_busy_once = 1;
    assert(uvm_va_space_revoke_all_user_channels_checked(&va) == NV_OK);
    assert(channels[0].revoke_calls == 3);
    assert(gpu.rm_revoke_calls == 4);
    assert(gpu.rm_verify_calls == 3);
    assert(va.user_channels_revoked == 1);

    /* Missing RM instance is provisional until the all-client scan passes. */
    va.user_channels_revoked = 0;
    va.user_channels_stopped = 0;
    channels[1].revoke_result = NV_WARN_MORE_PROCESSING_REQUIRED;
    channels[1].is_bound = 1;
    gpu.rm_verify_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_va_space_revoke_all_user_channels_checked(&va) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(va.user_channels_revoked == 0);
    assert(channels[1].is_bound == 1);
    gpu.rm_verify_result = NV_OK;
    assert(uvm_va_space_revoke_all_user_channels_checked(&va) == NV_OK);
    assert(va.user_channels_revoked == 1);
    assert(channels[1].is_bound == 0);
    return 0;
}
