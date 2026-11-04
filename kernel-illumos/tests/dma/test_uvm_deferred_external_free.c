/*
 * SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC
 * SPDX-License-Identifier: MIT
 */

/* A failed UVM_FREE must release its VA after the last tracked map retires. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_INVALID_STATE 2
#define UVM_VA_RANGE_TYPE_EXTERNAL 1
#define UVM_VA_RANGE_TYPE_MANAGED 2

typedef struct uvm_va_range {
    int type;
    struct uvm_va_range *next;
} uvm_va_range_t;
typedef struct {
    uvm_va_range_t va_range;
    bool deferred_free_pending;
    int mapped_gpus;
    bool freed;
} uvm_va_range_external_t;
typedef struct {
    uvm_va_range_t *first;
    bool write_locked;
} uvm_va_space_t;
static uvm_va_space_t *destroy_space;

static void uvm_va_space_down_write(uvm_va_space_t *vs)
{
    assert(!vs->write_locked);
    vs->write_locked = true;
}
static void uvm_va_space_up_write(uvm_va_space_t *vs)
{
    assert(vs->write_locked);
    vs->write_locked = false;
}
static bool uvm_processor_mask_empty(const int *mask) { return *mask == 0; }
static NV_STATUS uvm_va_range_destroy(uvm_va_range_t *range, void *deferred)
{
    uvm_va_range_external_t *external = (uvm_va_range_external_t *)range;
    uvm_va_range_t **slot = &destroy_space->first;
    (void)deferred;
    assert(destroy_space->write_locked);
    assert(external->deferred_free_pending && external->mapped_gpus == 0);
    while (*slot != range) {
        assert(*slot != NULL);
        slot = &(*slot)->next;
    }
    *slot = range->next;
    external->freed = true;
    return NV_OK;
}

#define UVM_ASSERT(value) assert(value)
#define uvm_va_range_to_external(range) ((uvm_va_range_external_t *)(range))
#define uvm_for_each_va_range_safe(range, next, vs) \
    for ((range) = (vs)->first; (range) != NULL && (((next) = (range)->next), true); (range) = (next))

#include "../../../kernel-open/nvidia-uvm/uvm_va_range_deferred_external_impl.h"

int main(void)
{
    uvm_va_range_external_t unrelated = {{UVM_VA_RANGE_TYPE_EXTERNAL, NULL}, false, 0, false};
    uvm_va_range_external_t still_mapped = {{UVM_VA_RANGE_TYPE_EXTERNAL, &unrelated.va_range}, true, 1, false};
    uvm_va_range_external_t stale = {{UVM_VA_RANGE_TYPE_EXTERNAL, &still_mapped.va_range}, true, 0, false};
    uvm_va_space_t vs = {&stale.va_range, false};
    destroy_space = &vs;

    /* UVM_FREE already requested removal: neither map API may reuse it. */
    assert(uvm_va_range_external_map_status(&stale) == NV_ERR_INVALID_STATE);
    assert(uvm_va_range_external_map_status(&still_mapped) == NV_ERR_INVALID_STATE);
    assert(uvm_va_range_external_map_status(&unrelated) == NV_OK);

    uvm_va_space_free_deferred_external_ranges(&vs);
    assert(stale.freed);
    assert(vs.first == &still_mapped.va_range);
    assert(!still_mapped.freed && !unrelated.freed);
    assert(!vs.write_locked);

    still_mapped.mapped_gpus = 0;
    uvm_va_space_free_deferred_external_ranges(&vs);
    assert(still_mapped.freed);
    assert(vs.first == &unrelated.va_range);
    assert(!unrelated.freed);
    return 0;
}
