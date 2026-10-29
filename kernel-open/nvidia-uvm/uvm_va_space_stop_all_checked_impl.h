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

/* Included by uvm_va_space.c and the focused channel-stop test. */
static NV_STATUS
uvm_va_space_stop_all_user_channels_status(uvm_va_space_t *va_space, bool immediate)
{
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_user_channel_t *user_channel;
    NV_STATUS status = NV_OK;

    uvm_assert_rwsem_locked_read(&va_space->lock);

    if (atomic_read(&va_space->user_channels_stopped))
        return NV_OK;

    for_each_gpu_va_space(gpu_va_space, va_space) {
        list_for_each_entry(user_channel, &gpu_va_space->registered_channels, list_node) {
            NV_STATUS channel_status = uvm_user_channel_stop_mode_checked(user_channel, immediate);
            if (status == NV_OK)
                status = channel_status;
        }
    }

    /* Attempt every stop, but certify the set only if all RM calls passed. */
    if (status == NV_OK)
        atomic_set(&va_space->user_channels_stopped, 1);
    return status;
}

NV_STATUS
uvm_va_space_stop_all_user_channels_checked(uvm_va_space_t *va_space)
{
    return uvm_va_space_stop_all_user_channels_status(va_space, false);
}

NV_STATUS
uvm_va_space_revoke_all_user_channels_checked(uvm_va_space_t *va_space)
{
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_user_channel_t *user_channel;
    NV_STATUS status = NV_OK;

    uvm_assert_rwsem_locked_read(&va_space->lock);

    /* The ordinary stopped flag is insufficient: stopped channels can rebind. */
    atomic_set(&va_space->user_channels_revoking, 1);
    for_each_gpu_va_space(gpu_va_space, va_space) {
        NV_STATUS va_status = uvm_rm_locked_call(
            nvUvmInterfaceRevokeAddressSpace(gpu_va_space->duped_gpu_va_space));
        if (status == NV_OK)
            status = va_status;
    }
    if (status != NV_OK)
        return status;

    for_each_gpu_va_space(gpu_va_space, va_space) {
        list_for_each_entry(user_channel, &gpu_va_space->registered_channels, list_node) {
            NV_STATUS channel_status = uvm_user_channel_revoke_checked(user_channel);
            if (channel_status == NV_WARN_MORE_PROCESSING_REQUIRED)
                continue; /* The all-client RM scan below must certify it. */
            if (status == NV_OK)
                status = channel_status;
        }
    }

    if (status != NV_OK)
        return status;

    /* A duplicated VAS can also have raw channels in another RM client. */
    for_each_gpu_va_space(gpu_va_space, va_space) {
        do {
            status = uvm_rm_locked_call(nvUvmInterfaceVerifyVaSpaceChannelsRevoked(
                gpu_va_space->duped_gpu_va_space));
            /* RM releases all client locks before reporting snapshot churn. */
            if (status == NV_ERR_BUSY_RETRY)
                usleep_range(10000, 12000);
        } while (status == NV_ERR_BUSY_RETRY);
        if (status != NV_OK)
            return status;
    }

    /* A missing retained channel is certified only after the RM scan. */
    for_each_gpu_va_space(gpu_va_space, va_space) {
        list_for_each_entry(user_channel, &gpu_va_space->registered_channels, list_node)
            atomic_set(&user_channel->is_bound, 0);
    }

    if (status == NV_OK) {
        atomic_set(&va_space->user_channels_stopped, 1);
        atomic_set(&va_space->user_channels_revoked, 1);
    }
    return status;
}
