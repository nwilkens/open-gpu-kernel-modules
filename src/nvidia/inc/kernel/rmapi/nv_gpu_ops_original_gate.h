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

#ifndef _NV_GPU_OPS_ORIGINAL_GATE_H_
#define _NV_GPU_OPS_ORIGINAL_GATE_H_

/* A conservative preflight before RM frees a pinned OS descriptor. */
typedef struct nv_original_gate_input {
    unsigned int dup_count;
    unsigned int ref_count;
    unsigned int guards;
    unsigned int all_imports_complete;
    unsigned int cpu_mapping_count;
    unsigned int cpu_mapping_is_os_descriptor;
    unsigned int backref_count;
    unsigned int inter_mapping_count;
    unsigned int inter_backref_count;
    unsigned int child_count;
    unsigned int dependent_count;
} nv_original_gate_input_t;

typedef enum nv_original_guard_result {
    NV_ORIGINAL_GUARD_NONE,
    NV_ORIGINAL_GUARD_TRANSFER,
    NV_ORIGINAL_GUARD_RELEASE,
} nv_original_guard_result_t;

static inline nv_original_guard_result_t
nv_original_guard_action(unsigned int guards, unsigned int dup_count,
    unsigned int all_imports_complete)
{
    if (guards == 0)
        return NV_ORIGINAL_GUARD_NONE;
    if (dup_count != 0 || !all_imports_complete)
        return NV_ORIGINAL_GUARD_TRANSFER;
    return NV_ORIGINAL_GUARD_RELEASE;
}

/* A Memory destructor can run while another Memory still holds a copy. */
static inline int
nv_original_memory_intermediate(unsigned int guards,
    unsigned int dup_count, unsigned int ref_count)
{
    return guards != 0 && dup_count != 0 && ref_count > guards + 1;
}

/* The last Memory is leaving while an imported GPU mapping is still live. */
static inline int
nv_original_memory_release_pending(unsigned int guards,
    unsigned int dup_count, unsigned int ref_count,
    unsigned int all_imports_complete)
{
    return guards != 0 && dup_count == 0 &&
        ref_count == guards + 1 && !all_imports_complete;
}

static inline int
nv_original_gate_allows_free(const nv_original_gate_input_t *in)
{
    return in != 0 && in->dup_count == 1 &&
        in->ref_count == in->guards + 1 &&
        in->all_imports_complete &&
        in->cpu_mapping_count == 1 &&
        in->cpu_mapping_is_os_descriptor &&
        in->backref_count == 0 &&
        in->inter_mapping_count == 0 &&
        in->inter_backref_count == 0 &&
        in->child_count == 0 &&
        in->dependent_count == 0;
}

#endif
