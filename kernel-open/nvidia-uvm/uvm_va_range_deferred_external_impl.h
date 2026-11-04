/*
 * SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC
 * SPDX-License-Identifier: MIT
 */

/* Included by uvm_va_range.c and the focused deferred-free test. */
NV_STATUS uvm_va_range_external_map_status(const uvm_va_range_external_t *external_range)
{
    return external_range->deferred_free_pending ? NV_ERR_INVALID_STATE : NV_OK;
}

void uvm_va_space_free_deferred_external_ranges(uvm_va_space_t *va_space)
{
    uvm_va_range_t *va_range, *next;

    uvm_va_space_down_write(va_space);
    uvm_for_each_va_range_safe(va_range, next, va_space) {
        uvm_va_range_external_t *external_range;
        NV_STATUS status;

        if (va_range->type != UVM_VA_RANGE_TYPE_EXTERNAL)
            continue;

        external_range = uvm_va_range_to_external(va_range);
        if (!external_range->deferred_free_pending ||
            !uvm_processor_mask_empty(&external_range->mapped_gpus))
            continue;

        /* All checked map teardown has completed before the range is removed. */
        status = uvm_va_range_destroy(va_range, NULL);
        UVM_ASSERT(status == NV_OK);
    }
    uvm_va_space_up_write(va_space);
}
