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

#include <assert.h>
#include <stdio.h>

#include "../../../src/nvidia/inc/kernel/rmapi/nv_gpu_ops_original_gate.h"

static nv_original_gate_input_t
safe_original(void)
{
    nv_original_gate_input_t input = {
        .dup_count = 1,
        .ref_count = 2,
        .guards = 1,
        .all_imports_complete = 1,
        .cpu_mapping_count = 1,
        .cpu_mapping_is_os_descriptor = 1,
    };
    return input;
}

int
main(void)
{
    nv_original_gate_input_t input = safe_original();

    assert(nv_original_gate_allows_free(&input));
    input.ref_count = 3;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.dup_count = 2;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.all_imports_complete = 0;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.inter_mapping_count = 1;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.cpu_mapping_is_os_descriptor = 0;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.backref_count = 1;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.inter_backref_count = 1;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.child_count = 1;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.dependent_count = 1;
    assert(!nv_original_gate_allows_free(&input));
    input = safe_original();
    input.guards = 0;
    input.ref_count = 1;
    assert(nv_original_gate_allows_free(&input));

    assert(nv_original_guard_action(1, 1, 1) ==
           NV_ORIGINAL_GUARD_TRANSFER);
    assert(nv_original_guard_action(1, 0, 0) ==
           NV_ORIGINAL_GUARD_TRANSFER);
    assert(nv_original_guard_action(1, 0, 1) ==
           NV_ORIGINAL_GUARD_RELEASE);
    assert(nv_original_guard_action(0, 0, 1) ==
           NV_ORIGINAL_GUARD_NONE);

    /* A live duplicate owns another reference: keep the guard, no error. */
    assert(nv_original_memory_intermediate(1, 1, 3));
    assert(nv_original_memory_intermediate(1, 2, 4));
    assert(!nv_original_memory_intermediate(1, 1, 2));
    assert(!nv_original_memory_intermediate(1, 0, 3));
    assert(!nv_original_memory_intermediate(1, 0, 2));
    assert(!nv_original_memory_intermediate(0, 1, 2));

    /* The final Memory may go away before UVM finishes GPU cleanup. */
    assert(nv_original_memory_release_pending(1, 0, 2, 0));
    assert(!nv_original_memory_release_pending(1, 0, 2, 1));
    assert(!nv_original_memory_release_pending(1, 1, 2, 0));
    assert(!nv_original_memory_release_pending(1, 0, 1, 0));
    assert(!nv_original_memory_release_pending(0, 0, 1, 0));

    puts("original OS descriptor gate: 26 cases passed");
    return 0;
}
