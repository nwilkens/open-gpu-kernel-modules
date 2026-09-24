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
 * I2C controller provider for NVIDIA GPUs.
 *
 * The nvidia_i2c pseudo node registers one controller per GPU with the
 * illumos I2C framework, named nvgpuBBDDF after the GPU's PCI location.
 * Controller port N is RM I2C port N.  A port works while the GPU is
 * initialized and RM has exported that port; nvidia checks this for every
 * transfer.
 *
 * A controller is bound to one attach of its GPU.  If the GPU detaches, the
 * controller stays registered and its I/O fails.  A GPU that attaches again
 * gets a working controller when this driver next attaches.
 *
 * Ports that RM reaches over DP AUX carry only plain reads and writes; the
 * other operations fail there with I2C_CTRL_E_UNSUP_CMD.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/list.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/i2c/controller.h>

#include "nv-illumos-i2c.h"

#define NVI2C_ADDR_MAX          0x7f
#define NVI2C_SMBUS_BLOCK       SMBUS_V2_MAX_BLOCK

#define NVI2C_SMBUS_OPS         (SMBUS_PROP_OP_QUICK_COMMAND | \
                                 SMBUS_PROP_OP_SEND_BYTE | \
                                 SMBUS_PROP_OP_RECV_BYTE | \
                                 SMBUS_PROP_OP_WRITE_BYTE | \
                                 SMBUS_PROP_OP_READ_BYTE | \
                                 SMBUS_PROP_OP_WRITE_WORD | \
                                 SMBUS_PROP_OP_READ_WORD | \
                                 SMBUS_PROP_OP_WRITE_BLOCK | \
                                 SMBUS_PROP_OP_I2C_WRITE_BLOCK | \
                                 SMBUS_PROP_OP_I2C_READ_BLOCK)

CTASSERT(NV_I2C_MAX_XFER <= I2C_REQ_MAX);

typedef struct nvi2c_ctrl {
    list_node_t         nc_link;
    NvU32               nc_gpu_id;
    NvU64               nc_gen;
    char                nc_name[I2C_NAME_MAX];
    i2c_ctrl_hdl_t     *nc_hdl;
} nvi2c_ctrl_t;

typedef struct nvi2c {
    kmutex_t            ni_lock;        /* ni_dip and ni_ctrls */
    dev_info_t         *ni_dip;
    list_t              ni_ctrls;
    nvidia_i2c_ops_t    ni_ops;         /* set before any controller */
} nvi2c_t;

static nvi2c_t nvi2c;

static NV_STATUS
nvi2c_xfer(const nvi2c_ctrl_t *nc, uint32_t port, nv_i2c_cmd_t cmd,
    uint8_t addr, uint8_t command, uint32_t len, uint8_t *buf)
{
    return (nvi2c.ni_ops.transfer(nc->nc_gpu_id, nc->nc_gen, port, cmd,
        addr, command, len, buf));
}

/*
 * RM reports I2C failures as NV_ERR_I2C_ERROR without saying whether a
 * NACK, arbitration loss or something else happened.
 */
static void
nvi2c_io_status(i2c_error_t *err, NV_STATUS status)
{
    switch (status)
    {
        case NV_OK:
            i2c_ctrl_io_success(err);
            break;
        case NV_ERR_NOT_SUPPORTED:
            i2c_ctrl_io_error(err, I2C_CORE_E_CONTROLLER,
                I2C_CTRL_E_UNSUP_CMD);
            break;
        case NV_ERR_TIMEOUT:
            i2c_ctrl_io_error(err, I2C_CORE_E_CONTROLLER, I2C_CTRL_E_REQ_TO);
            break;
        default:
            i2c_ctrl_io_error(err, I2C_CORE_E_CONTROLLER,
                I2C_CTRL_E_INTERNAL);
            break;
    }
}

static bool
nvi2c_req_ok(uint32_t port, const i2c_addr_t *addr, i2c_error_t *err)
{
    if (port >= NV_I2C_NUM_PORTS)
    {
        i2c_ctrl_io_error(err, I2C_CORE_E_CONTROLLER, I2C_CTRL_E_DRIVER);
        return (false);
    }

    if (addr->ia_type != I2C_ADDR_7BIT)
    {
        i2c_ctrl_io_error(err, I2C_CORE_E_UNSUP_ADDR_TYPE, 0);
        return (false);
    }

    if (addr->ia_addr > NVI2C_ADDR_MAX)
    {
        i2c_ctrl_io_error(err, I2C_CORE_E_BAD_ADDR, 0);
        return (false);
    }

    return (true);
}

/*
 * A request with both a write and a read needs a repeated start between
 * them.  RM has that only as a one-byte register write followed by the read.
 */
static void
nvi2c_io_i2c(void *arg, uint32_t port, i2c_req_t *req)
{
    const nvi2c_ctrl_t *nc = arg;
    uint8_t addr;
    NV_STATUS status;

    if (!nvi2c_req_ok(port, &req->ir_addr, &req->ir_error))
        return;
    addr = (uint8_t)req->ir_addr.ia_addr;

    if (req->ir_wlen > NV_I2C_MAX_XFER)
    {
        i2c_ctrl_io_error(&req->ir_error, I2C_CORE_E_BAD_I2C_REQ_WRITE_LEN,
            0);
        return;
    }

    if (req->ir_rlen > NV_I2C_MAX_XFER)
    {
        i2c_ctrl_io_error(&req->ir_error, I2C_CORE_E_BAD_I2C_REQ_READ_LEN,
            0);
        return;
    }

    if (req->ir_wlen == 0 && req->ir_rlen == 0)
    {
        i2c_ctrl_io_error(&req->ir_error, I2C_CORE_E_NEED_READ_OR_WRITE, 0);
        return;
    }

    if (req->ir_rlen == 0)
    {
        status = nvi2c_xfer(nc, port, NV_I2C_CMD_WRITE, addr, 0,
            req->ir_wlen, req->ir_wdata);
    }
    else if (req->ir_wlen == 0)
    {
        status = nvi2c_xfer(nc, port, NV_I2C_CMD_READ, addr, 0,
            req->ir_rlen, req->ir_rdata);
    }
    else if (req->ir_wlen == 1)
    {
        status = nvi2c_xfer(nc, port, NV_I2C_CMD_BLOCK_READ, addr,
            req->ir_wdata[0], req->ir_rlen, req->ir_rdata);
    }
    else
    {
        i2c_ctrl_io_error(&req->ir_error, I2C_CORE_E_CONTROLLER,
            I2C_CTRL_E_UNSUP_CMD);
        return;
    }

    nvi2c_io_status(&req->ir_error, status);
}

static void
nvi2c_io_smbus(void *arg, uint32_t port, smbus_req_t *req)
{
    const nvi2c_ctrl_t *nc = arg;
    uint8_t block[NVI2C_SMBUS_BLOCK + 1];
    uint8_t addr, cmd;
    NV_STATUS status;

    if (!nvi2c_req_ok(port, &req->smbr_addr, &req->smbr_error))
        return;
    addr = (uint8_t)req->smbr_addr.ia_addr;
    cmd = req->smbr_cmd;

    switch (req->smbr_op)
    {
        case SMBUS_OP_QUICK_COMMAND:
            status = nvi2c_xfer(nc, port,
                (req->smbr_flags & I2C_IO_REQ_F_QUICK_WRITE) != 0 ?
                NV_I2C_CMD_SMBUS_QUICK_WRITE : NV_I2C_CMD_SMBUS_QUICK_READ,
                addr, 0, 0, NULL);
            break;
        case SMBUS_OP_SEND_BYTE:
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_WRITE, addr, 0, 1,
                req->smbr_wdata);
            break;
        case SMBUS_OP_RECV_BYTE:
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_READ, addr, 0, 1,
                req->smbr_rdata);
            break;
        case SMBUS_OP_WRITE_BYTE:
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_SMBUS_WRITE, addr, cmd,
                1, req->smbr_wdata);
            break;
        case SMBUS_OP_READ_BYTE:
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_SMBUS_READ, addr, cmd,
                1, req->smbr_rdata);
            break;
        case SMBUS_OP_WRITE_WORD:
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_SMBUS_WRITE, addr, cmd,
                2, req->smbr_wdata);
            break;
        case SMBUS_OP_READ_WORD:
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_SMBUS_READ, addr, cmd,
                2, req->smbr_rdata);
            break;
        case SMBUS_OP_WRITE_BLOCK:
            if (req->smbr_wlen == 0 || req->smbr_wlen > NVI2C_SMBUS_BLOCK)
            {
                i2c_ctrl_io_error(&req->smbr_error,
                    I2C_CORE_E_BAD_SMBUS_WRITE_LEN, 0);
                return;
            }
            block[0] = (uint8_t)req->smbr_wlen;
            bcopy(req->smbr_wdata, &block[1], req->smbr_wlen);
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_SMBUS_BLOCK_WRITE, addr,
                cmd, req->smbr_wlen + 1, block);
            break;
        case SMBUS_OP_I2C_WRITE_BLOCK:
            if (req->smbr_wlen == 0 || req->smbr_wlen > NVI2C_SMBUS_BLOCK)
            {
                i2c_ctrl_io_error(&req->smbr_error,
                    I2C_CORE_E_BAD_SMBUS_WRITE_LEN, 0);
                return;
            }
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_BLOCK_WRITE, addr, cmd,
                req->smbr_wlen, req->smbr_wdata);
            break;
        case SMBUS_OP_I2C_READ_BLOCK:
            if (req->smbr_rlen == 0 || req->smbr_rlen > NVI2C_SMBUS_BLOCK)
            {
                i2c_ctrl_io_error(&req->smbr_error,
                    I2C_CORE_E_BAD_SMBUS_READ_LEN, 0);
                return;
            }
            status = nvi2c_xfer(nc, port, NV_I2C_CMD_BLOCK_READ, addr, cmd,
                req->smbr_rlen, req->smbr_rdata);
            break;
        default:
            i2c_ctrl_io_error(&req->smbr_error, I2C_CORE_E_CONTROLLER,
                I2C_CTRL_E_UNSUP_CMD);
            return;
    }

    nvi2c_io_status(&req->smbr_error, status);
}

static i2c_errno_t
nvi2c_prop_info(void *arg, i2c_prop_t prop, i2c_prop_info_t *info)
{
    switch (prop)
    {
        case SMBUS_PROP_SUP_OPS:
        case I2C_PROP_MAX_READ:
        case I2C_PROP_MAX_WRITE:
        case SMBUS_PROP_MAX_BLOCK:
            break;
        default:
            return (I2C_PROP_E_UNSUP);
    }

    i2c_prop_info_set_perm(info, I2C_PROP_PERM_RO);
    return (I2C_CORE_E_OK);
}

static i2c_errno_t
nvi2c_prop_get(void *arg, i2c_prop_t prop, void *buf, size_t buflen)
{
    uint32_t val;

    switch (prop)
    {
        case SMBUS_PROP_SUP_OPS:
            val = NVI2C_SMBUS_OPS;
            break;
        case I2C_PROP_MAX_READ:
        case I2C_PROP_MAX_WRITE:
            val = NV_I2C_MAX_XFER;
            break;
        case SMBUS_PROP_MAX_BLOCK:
            val = NVI2C_SMBUS_BLOCK;
            break;
        default:
            return (I2C_PROP_E_UNSUP);
    }

    if (buflen < sizeof (val))
        return (I2C_PROP_E_SMALL_BUF);

    bcopy(&val, buf, sizeof (val));
    return (I2C_CORE_E_OK);
}

static const i2c_ctrl_ops_t nvi2c_ctrl_ops = {
    .i2c_port_name_f    = i2c_ctrl_port_name_portno,
    .i2c_io_i2c_f       = nvi2c_io_i2c,
    .i2c_io_smbus_f     = nvi2c_io_smbus,
    .i2c_prop_info_f    = nvi2c_prop_info,
    .i2c_prop_get_f     = nvi2c_prop_get,
};

/*
 * Registers a controller for a GPU.  Called from attach and from nvidia's
 * probe callback; both may report the same GPU.  i2c_ctrl_register() fails
 * on a duplicate name while still holding a framework lock, so names are
 * checked here first.
 */
static void
nvi2c_add_gpu(const nvidia_i2c_gpu_info_t *gpu)
{
    i2c_ctrl_register_t *reg;
    i2c_ctrl_reg_error_t ret;
    nvi2c_ctrl_t *nc;
    char name[I2C_NAME_MAX];

    if (gpu->domain != 0)
        (void) snprintf(name, sizeof (name), "nvgpu%04x%02x%02x%x",
            gpu->domain, gpu->bus, gpu->slot, gpu->function);
    else
        (void) snprintf(name, sizeof (name), "nvgpu%02x%02x%x",
            gpu->bus, gpu->slot, gpu->function);

    mutex_enter(&nvi2c.ni_lock);
    if (nvi2c.ni_dip == NULL)
        goto out;

    for (nc = list_head(&nvi2c.ni_ctrls); nc != NULL;
         nc = list_next(&nvi2c.ni_ctrls, nc))
    {
        if (nc->nc_gpu_id != gpu->gpu_id && strcmp(nc->nc_name, name) != 0)
            continue;

        if (nc->nc_gpu_id != gpu->gpu_id || nc->nc_gen != gpu->gen)
        {
            dev_err(nvi2c.ni_dip, CE_NOTE, "!%s stays unavailable until "
                "nvidia_i2c attaches again", nc->nc_name);
        }
        goto out;
    }

    if (i2c_ctrl_register_alloc(I2C_CTRL_PROVIDER, &reg) !=
        I2C_CTRL_REG_E_OK)
    {
        dev_err(nvi2c.ni_dip, CE_WARN, "failed to allocate I2C controller "
            "registration for %s", name);
        goto out;
    }

    nc = kmem_zalloc(sizeof (*nc), KM_SLEEP);
    nc->nc_gpu_id = gpu->gpu_id;
    nc->nc_gen = gpu->gen;
    (void) strlcpy(nc->nc_name, name, sizeof (nc->nc_name));

    reg->ic_type = I2C_CTRL_TYPE_I2C;
    reg->ic_nports = NV_I2C_NUM_PORTS;
    reg->ic_name = nc->nc_name;
    reg->ic_dip = nvi2c.ni_dip;
    reg->ic_drv = nc;
    reg->ic_ops = &nvi2c_ctrl_ops;

    ret = i2c_ctrl_register(reg, &nc->nc_hdl);
    i2c_ctrl_register_free(reg);
    if (ret != I2C_CTRL_REG_E_OK)
    {
        dev_err(nvi2c.ni_dip, CE_WARN, "failed to register I2C controller "
            "%s: 0x%x", name, ret);
        kmem_free(nc, sizeof (*nc));
        goto out;
    }

    list_insert_tail(&nvi2c.ni_ctrls, nc);
out:
    mutex_exit(&nvi2c.ni_lock);
}

static const nvidia_i2c_callbacks_t nvi2c_callbacks = {
    .probe = nvi2c_add_gpu,
};

static void
nvi2c_remove_all(void)
{
    nvi2c_ctrl_t *nc;

    mutex_enter(&nvi2c.ni_lock);
    nvi2c.ni_dip = NULL;
    while ((nc = list_remove_head(&nvi2c.ni_ctrls)) != NULL)
    {
        VERIFY3U(i2c_ctrl_unregister(nc->nc_hdl), ==, I2C_CTRL_REG_E_OK);
        kmem_free(nc, sizeof (*nc));
    }
    mutex_exit(&nvi2c.ni_lock);
}

static int
nvi2c_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
    nvidia_i2c_gpu_info_t *gpus;
    NvU32 i, count;

    if (cmd == DDI_RESUME)
        return (DDI_SUCCESS);
    if (cmd != DDI_ATTACH)
        return (DDI_FAILURE);

    if (ddi_get_instance(dip) != 0)
    {
        dev_err(dip, CE_WARN, "only instance 0 is supported");
        return (DDI_FAILURE);
    }

    nvi2c.ni_ops.version_string = NV_VERSION_STRING;
    if (nvidia_get_i2c_ops(&nvi2c.ni_ops) != NV_OK)
    {
        dev_err(dip, CE_WARN, "nvidia is version %s, expected %s",
            nvi2c.ni_ops.version_string, NV_VERSION_STRING);
        return (DDI_FAILURE);
    }

    mutex_enter(&nvi2c.ni_lock);
    VERIFY3P(nvi2c.ni_dip, ==, NULL);
    nvi2c.ni_dip = dip;
    mutex_exit(&nvi2c.ni_lock);

    if (nvi2c.ni_ops.set_callbacks(&nvi2c_callbacks) != 0)
    {
        dev_err(dip, CE_WARN, "nvidia I2C callbacks already registered");
        nvi2c_remove_all();
        return (DDI_FAILURE);
    }

    gpus = kmem_alloc(sizeof (*gpus) * NV_MAX_DEVICES, KM_SLEEP);
    count = nvi2c.ni_ops.enumerate_gpus(gpus, NV_MAX_DEVICES);
    for (i = 0; i < count; i++)
        nvi2c_add_gpu(&gpus[i]);
    kmem_free(gpus, sizeof (*gpus) * NV_MAX_DEVICES);

    ddi_report_dev(dip);
    return (DDI_SUCCESS);
}

/*
 * The framework's controller nodes are our children and hold every open of
 * the I2C minors, so none are left once the DDI calls detach.  Holding our
 * own node keeps a bus_config from creating one while controllers go away.
 */
static int
nvi2c_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
    if (cmd == DDI_SUSPEND)
        return (DDI_SUCCESS);
    if (cmd != DDI_DETACH || dip != nvi2c.ni_dip)
        return (DDI_FAILURE);

    ndi_devi_enter(dip);
    if (ddi_get_child(dip) != NULL)
    {
        ndi_devi_exit(dip);
        return (DDI_FAILURE);
    }

    (void) nvi2c.ni_ops.set_callbacks(NULL);
    nvi2c_remove_all();
    ndi_devi_exit(dip);

    return (DDI_SUCCESS);
}

static int
nvi2c_getinfo(dev_info_t *unused, ddi_info_cmd_t cmd, void *arg,
    void **resultp)
{
    switch (cmd)
    {
        case DDI_INFO_DEVT2DEVINFO:
            if (nvi2c.ni_dip == NULL)
                return (DDI_FAILURE);
            *resultp = nvi2c.ni_dip;
            return (DDI_SUCCESS);
        case DDI_INFO_DEVT2INSTANCE:
            *resultp = (void *)0;
            return (DDI_SUCCESS);
        default:
            return (DDI_FAILURE);
    }
}

static struct cb_ops nvi2c_cb_ops = {
    .cb_open        = nodev,
    .cb_close       = nodev,
    .cb_strategy    = nodev,
    .cb_print       = nodev,
    .cb_dump        = nodev,
    .cb_read        = nodev,
    .cb_write       = nodev,
    .cb_ioctl       = nodev,
    .cb_devmap      = nodev,
    .cb_mmap        = nodev,
    .cb_segmap      = nodev,
    .cb_chpoll      = nochpoll,
    .cb_prop_op     = ddi_prop_op,
    .cb_str         = NULL,
    .cb_flag        = D_MP,
    .cb_rev         = CB_REV,
    .cb_aread       = nodev,
    .cb_awrite      = nodev,
};

static struct dev_ops nvi2c_dev_ops = {
    .devo_rev       = DEVO_REV,
    .devo_refcnt    = 0,
    .devo_getinfo   = nvi2c_getinfo,
    .devo_identify  = nulldev,
    .devo_probe     = nulldev,
    .devo_attach    = nvi2c_attach,
    .devo_detach    = nvi2c_detach,
    .devo_reset     = nodev,
    .devo_cb_ops    = &nvi2c_cb_ops,
    .devo_bus_ops   = NULL,         /* set by i2c_ctrl_mod_init() */
    .devo_power     = NULL,
    .devo_quiesce   = ddi_quiesce_not_needed,
};

static struct modldrv nvi2c_modldrv = {
    &mod_driverops,
    "NVIDIA GPU I2C " NV_VERSION_STRING,
    &nvi2c_dev_ops
};

static struct modlinkage nvi2c_modlinkage = {
    MODREV_1,
    { &nvi2c_modldrv, NULL }
};

int
_init(void)
{
    int rc;

    mutex_init(&nvi2c.ni_lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&nvi2c.ni_ctrls, sizeof (nvi2c_ctrl_t),
        offsetof(nvi2c_ctrl_t, nc_link));

    i2c_ctrl_mod_init(&nvi2c_dev_ops);
    rc = mod_install(&nvi2c_modlinkage);
    if (rc != 0)
    {
        i2c_ctrl_mod_fini(&nvi2c_dev_ops);
        list_destroy(&nvi2c.ni_ctrls);
        mutex_destroy(&nvi2c.ni_lock);
    }

    return (rc);
}

int
_fini(void)
{
    int rc;

    rc = mod_remove(&nvi2c_modlinkage);
    if (rc != 0)
        return (rc);

    i2c_ctrl_mod_fini(&nvi2c_dev_ops);
    list_destroy(&nvi2c.ni_ctrls);
    mutex_destroy(&nvi2c.ni_lock);

    return (0);
}

int
_info(struct modinfo *modinfop)
{
    return (mod_info(&nvi2c_modlinkage, modinfop));
}
