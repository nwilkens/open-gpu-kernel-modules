/*
 * SPDX-FileCopyrightText: Copyright (c) 2005-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * GPU I2C ports, and the interface exported to the nvidia_i2c module.
 *
 * RM adds its ports from rm_init_adapter() with ldata_lock held, and removes
 * them before each full init and at detach.  The adapter handle RM stores
 * for port N is &nvis->i2c_port_added[N].  Transfers from nvidia_i2c hold
 * ldata_lock, so they are serialized with start, stop, suspend and detach,
 * and reach RM only for an initialized GPU and a port RM added.
 *
 * Callbacks into nvidia_i2c run under a reader lock that set_callbacks()
 * takes as writer, as in nvidia_modeset_interface.c.
 */

#include "nv-illumos.h"

static const nvidia_i2c_callbacks_t *nv_i2c_callbacks;
static krwlock_t nv_i2c_cb_lock;
static volatile uint64_t nv_i2c_last_gen;

void
nv_i2c_interface_init(void)
{
    rw_init(&nv_i2c_cb_lock, NULL, RW_DRIVER, NULL);
}

void
nv_i2c_interface_fini(void)
{
    rw_destroy(&nv_i2c_cb_lock);
}

void* NV_API_CALL nv_i2c_add_adapter(nv_state_t *nv, NvU32 port)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    if (port >= NV_I2C_NUM_PORTS)
        return NULL;

    nvis->i2c_port_added[port] = NV_TRUE;
    return &nvis->i2c_port_added[port];
}

void NV_API_CALL nv_i2c_del_adapter(nv_state_t *nv, void *data)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    uintptr_t base = (uintptr_t)&nvis->i2c_port_added[0];
    uintptr_t p = (uintptr_t)data;

    if (p >= base && p < base + sizeof (nvis->i2c_port_added))
        nvis->i2c_port_added[(p - base) / sizeof (NvBool)] = NV_FALSE;
}

/* The remaining hooks only serve SoC display I2C. */
void NV_API_CALL nv_i2c_unregister_clients(nv_state_t *nv)
{
}

NV_STATUS NV_API_CALL nv_i2c_transfer(nv_state_t *nv, NvU32 port,
    NvU8 address, nv_i2c_msg_t *msgs, int num_msgs)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_i2c_bus_status(nv_state_t *nv, NvU32 port,
    NvS32 *scl, NvS32 *sda)
{
    return NV_ERR_NOT_SUPPORTED;
}

static int
nvidia_i2c_set_callbacks(const nvidia_i2c_callbacks_t *cb)
{
    int rc = 0;

    rw_enter(&nv_i2c_cb_lock, RW_WRITER);
    if ((nv_i2c_callbacks != NULL && cb != NULL) ||
        (nv_i2c_callbacks == NULL && cb == NULL))
        rc = EINVAL;
    else
        nv_i2c_callbacks = cb;
    rw_exit(&nv_i2c_cb_lock);

    return rc;
}

static void
nvidia_i2c_get_gpu_info(nvidia_i2c_gpu_info_t *info, nv_illumos_state_t *nvis,
    NvU64 gen)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);

    info->gpu_id   = nv->gpu_id;
    info->gen      = gen;
    info->domain   = nv->pci_info.domain;
    info->bus      = nv->pci_info.bus;
    info->slot     = nv->pci_info.slot;
    info->function = nv->pci_info.function;
}

/*
 * Called once per attach, after the GPU is on nv_illumos_devices.  The
 * generation is published before the callback lock is taken, so a GPU whose
 * probe ran before set_callbacks() is found by enumerate_gpus().
 */
void
nvidia_i2c_probe(nv_illumos_state_t *nvis)
{
    nvidia_i2c_gpu_info_t info;
    NvU64 gen = atomic_inc_64_nv(&nv_i2c_last_gen);

    WRITE_ONCE(nvis->i2c_gen, gen);

    rw_enter(&nv_i2c_cb_lock, RW_READER);
    if (nv_i2c_callbacks != NULL)
    {
        nvidia_i2c_get_gpu_info(&info, nvis, gen);
        nv_i2c_callbacks->probe(&info);
    }
    rw_exit(&nv_i2c_cb_lock);
}

static NvU32
nvidia_i2c_enumerate_gpus(nvidia_i2c_gpu_info_t *info, NvU32 max)
{
    nv_illumos_state_t *nvis;
    NvU32 count = 0;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL && count < max;
         nvis = nvis->next)
    {
        NvU64 gen = READ_ONCE(nvis->i2c_gen);

        if (gen == 0)
            continue;

        nvidia_i2c_get_gpu_info(&info[count], nvis, gen);
        count++;
    }
    rw_exit(&nv_illumos_devices_lock);

    return count;
}

static NvBool
nvidia_i2c_cmd_valid(nv_i2c_cmd_t cmd, NvU32 len, const NvU8 *buf)
{
    if (len > NV_I2C_MAX_XFER || (len != 0 && buf == NULL))
        return NV_FALSE;

    switch (cmd)
    {
        case NV_I2C_CMD_SMBUS_QUICK_READ:
        case NV_I2C_CMD_SMBUS_QUICK_WRITE:
            return (len == 0);
        case NV_I2C_CMD_SMBUS_READ:
        case NV_I2C_CMD_SMBUS_WRITE:
            return (len == 1 || len == 2);
        case NV_I2C_CMD_READ:
        case NV_I2C_CMD_WRITE:
        case NV_I2C_CMD_BLOCK_READ:
        case NV_I2C_CMD_BLOCK_WRITE:
            return (len != 0);
        case NV_I2C_CMD_SMBUS_BLOCK_WRITE:
            /* buf[0] is the byte count sent ahead of buf[1..]. */
            return (len >= 2 && buf[0] == len - 1);
        default:
            /* RM's SMBUS_BLOCK_READ result layout is not defined here. */
            return NV_FALSE;
    }
}

static NV_STATUS
nvidia_i2c_transfer(NvU32 gpu_id, NvU64 gen, NvU32 port, nv_i2c_cmd_t cmd,
    NvU8 addr, NvU8 command, NvU32 len, NvU8 *buf)
{
    nv_illumos_state_t *nvis;
    nv_state_t *nv;
    nvidia_stack_t *sp = NULL;
    NV_STATUS status;

    if (gen == 0 || port >= NV_I2C_NUM_PORTS || addr > 0x7f ||
        !nvidia_i2c_cmd_valid(cmd, len, buf))
        return NV_ERR_INVALID_ARGUMENT;

    if (nv_stack_alloc(&sp) != 0)
        return NV_ERR_NO_MEMORY;

    nvis = nv_find_gpu_id_locked(gpu_id);
    if (nvis == NULL)
    {
        nv_stack_free(sp);
        return NV_ERR_OBJECT_NOT_FOUND;
    }
    nv = NV_STATE_PTR(nvis);

    if (nvis->i2c_gen != gen)
        status = NV_ERR_OBJECT_NOT_FOUND;
    else if (!(nv->flags & NV_FLAG_INITIALIZED) ||
             (nv->flags & NV_FLAG_SUSPENDED) || nv->removed ||
             NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv) ||
             !nvis->i2c_port_added[port])
        status = NV_ERR_INVALID_STATE;
    else
        status = rm_i2c_transfer(sp, nv, &nvis->i2c_port_added[port], cmd,
            addr, command, len, buf);

    sema_v(&nvis->ldata_lock);
    nv_stack_free(sp);

    return status;
}

NV_STATUS
nvidia_get_i2c_ops(nvidia_i2c_ops_t *ops)
{
    const nvidia_i2c_ops_t local_ops = {
        .version_string = NV_VERSION_STRING,
        .set_callbacks  = nvidia_i2c_set_callbacks,
        .enumerate_gpus = nvidia_i2c_enumerate_gpus,
        .transfer       = nvidia_i2c_transfer,
    };

    if (ops->version_string == NULL ||
        strcmp(ops->version_string, NV_VERSION_STRING) != 0)
    {
        ops->version_string = NV_VERSION_STRING;
        return NV_ERR_GENERIC;
    }

    *ops = local_ops;
    return NV_OK;
}
