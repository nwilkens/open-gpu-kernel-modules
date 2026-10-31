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

#ifndef NV_GPU_OPS_CHANNEL_GATE_IMPL_H
#define NV_GPU_OPS_CHANNEL_GATE_IMPL_H

/* STARTED blocks new work; STOPPED means STOP and GR eviction succeeded. */
#define NV_GPU_OPS_CHANNEL_REVOKE_STARTED  1U
#define NV_GPU_OPS_CHANNEL_REVOKE_STOPPED  2U

/*
 * A retained instance may disappear when the user frees its RM channel
 * during process exit.  Only the lookup failure can be handed to the later
 * all-client VA-space scan; a failure from the stop control stays fatal.
 */
static inline NV_STATUS
nvGpuOpsMissingChannelRevokeStatus(NV_STATUS lookupStatus, NvBool bRevoke)
{
    if (bRevoke && lookupStatus == NV_ERR_INVALID_CHANNEL)
        return NV_WARN_MORE_PROCESSING_REQUIRED;
    return lookupStatus;
}

static inline NV_STATUS
nvGpuOpsChannelRevocationStatus(OBJVASPACE *pVAS, KernelChannel *pChannel)
{
#if defined(NV_SUNOS)
    NvU32 revokeState;

    if (pChannel->pVAS != pVAS)
        return NV_OK;

    revokeState = portAtomicOrU32(&pChannel->uvmRevoked, 0);

    /* A never-bound user channel could not have passed RM admission. */
    if (revokeState == 0 && !pChannel->bUvmOwned &&
        !pChannel->bIsContextBound)
        return NV_OK;

    if ((revokeState &
         (NV_GPU_OPS_CHANNEL_REVOKE_STARTED |
          NV_GPU_OPS_CHANNEL_REVOKE_STOPPED)) !=
        (NV_GPU_OPS_CHANNEL_REVOKE_STARTED |
         NV_GPU_OPS_CHANNEL_REVOKE_STOPPED))
        return NV_ERR_INVALID_STATE;

    return NV_OK;
#else
    (void)pVAS;
    (void)pChannel;
    return NV_OK;
#endif
}

#endif
