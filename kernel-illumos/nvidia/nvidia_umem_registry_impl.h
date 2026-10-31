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

#ifndef _NVIDIA_UMEM_REGISTRY_IMPL_H_
#define _NVIDIA_UMEM_REGISTRY_IMPL_H_

/*
 * Small state machine shared by the illumos pin registry and its focused
 * tests. All transitions are made while holding the registry mutex.
 */
typedef struct nv_umem_pin_state {
    unsigned int imports;
    unsigned int pending;
    unsigned int guards;
    unsigned int revoking;
    unsigned int unlocked;
} nv_umem_pin_state_t;

/* Only the thread performing UVM's tracked RM duplicate may copy a pin. */
typedef struct nv_umem_copy_state {
    const void *authorized_thread;
} nv_umem_copy_state_t;

static inline void
nv_umem_copy_authorize(nv_umem_copy_state_t *copy, const void *thread)
{
    copy->authorized_thread = thread;
}

static inline void
nv_umem_copy_revoke(nv_umem_copy_state_t *copy)
{
    copy->authorized_thread = 0;
}

static inline int
nv_umem_copy_allowed(int pinned, const nv_umem_copy_state_t *copy,
    const void *thread)
{
    return !pinned || (copy->authorized_thread != 0 &&
        copy->authorized_thread == thread);
}

/* RM mappings of an imported pin belong to the tracked UVM client. */
static inline int
nv_umem_map_allowed(int pinned, unsigned int importer,
    unsigned int caller)
{
    return !pinned || (importer != 0 && importer == caller);
}

typedef enum nv_umem_original_phase {
    NV_UMEM_ORIGINAL_PENDING,
    NV_UMEM_ORIGINAL_ACTIVE,
    NV_UMEM_ORIGINAL_CLAIMED,
    NV_UMEM_ORIGINAL_CLOSING,
    NV_UMEM_ORIGINAL_CANCELED,
} nv_umem_original_phase_t;

typedef struct nv_umem_original_state {
    nv_umem_original_phase_t phase;
} nv_umem_original_state_t;

static inline void
nv_umem_original_init(nv_umem_original_state_t *owner)
{
    owner->phase = NV_UMEM_ORIGINAL_PENDING;
}

static inline int
nv_umem_original_publish(nv_umem_original_state_t *owner)
{
    if (owner->phase != NV_UMEM_ORIGINAL_PENDING)
        return 0;
    owner->phase = NV_UMEM_ORIGINAL_ACTIVE;
    return 1;
}

static inline void
nv_umem_original_cancel(nv_umem_original_state_t *owner)
{
    if (owner->phase == NV_UMEM_ORIGINAL_PENDING)
        owner->phase = NV_UMEM_ORIGINAL_CANCELED;
}

static inline int
nv_umem_original_claim(nv_umem_original_state_t *owner)
{
    if (owner->phase != NV_UMEM_ORIGINAL_ACTIVE)
        return 0;
    owner->phase = NV_UMEM_ORIGINAL_CLAIMED;
    return 1;
}

static inline void
nv_umem_original_begin_close(nv_umem_original_state_t *owner)
{
    if (owner->phase == NV_UMEM_ORIGINAL_ACTIVE ||
        owner->phase == NV_UMEM_ORIGINAL_CLAIMED)
        owner->phase = NV_UMEM_ORIGINAL_CLOSING;
}

static inline void
nv_umem_original_begin_close_if_match(nv_umem_original_state_t *owner,
    unsigned int owner_client, unsigned int owner_handle,
    unsigned int closing_client, unsigned int closing_handle)
{
    if (owner_client == closing_client && owner_handle == closing_handle)
        nv_umem_original_begin_close(owner);
}

/* RM stores the allocation wrapper, while os_lock_user_pages returns pin. */
static inline int
nv_umem_pin_matches(const void *pin, const void *alloc_private,
    const void *candidate)
{
    return candidate != 0 &&
        (candidate == pin || candidate == alloc_private);
}

static inline void
nv_umem_pin_state_init(nv_umem_pin_state_t *pin)
{
    pin->imports = 0;
    pin->pending = 0;
    pin->guards = 0;
    pin->revoking = 0;
    pin->unlocked = 0;
}

static inline int
nv_umem_pin_reserve(nv_umem_pin_state_t *pin)
{
    if (pin->revoking || pin->unlocked)
        return 0;
    pin->imports++;
    pin->pending++;
    return 1;
}

static inline void
nv_umem_pin_publish(nv_umem_pin_state_t *pin)
{
    --pin->pending;
}

static inline void
nv_umem_pin_cancel(nv_umem_pin_state_t *pin, int pending)
{
    if (pending)
        --pin->pending;
    --pin->imports;
}

static inline void
nv_umem_pin_hardware_complete(nv_umem_pin_state_t *pin)
{
    --pin->imports;
}

static inline void
nv_umem_pin_hardware_complete_pending(nv_umem_pin_state_t *pin)
{
    --pin->pending;
    --pin->imports;
}

static inline void
nv_umem_pin_guard_acquired(nv_umem_pin_state_t *pin)
{
    pin->guards++;
}

static inline void
nv_umem_pin_guard_released(nv_umem_pin_state_t *pin)
{
    --pin->guards;
}

static inline void
nv_umem_pin_begin_revoke(nv_umem_pin_state_t *pin)
{
    pin->revoking = 1;
}

static inline int
nv_umem_pin_can_unlock(const nv_umem_pin_state_t *pin)
{
    return pin->imports == 0 && pin->guards == 0;
}

static inline void
nv_umem_pin_mark_unlocked(nv_umem_pin_state_t *pin)
{
    pin->unlocked = 1;
}

/* Claim the one owner unlock only after every GPU import and guard is gone. */
static inline int
nv_umem_pin_claim_unlock(nv_umem_pin_state_t *pin)
{
    if (pin->unlocked || !nv_umem_pin_can_unlock(pin))
        return 0;
    nv_umem_pin_mark_unlocked(pin);
    return 1;
}

#endif
