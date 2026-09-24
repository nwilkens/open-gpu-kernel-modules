/*
 * SPDX-FileCopyrightText: Copyright (c) 2016-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * Definitions shared by the NVSwitch and NVLink files of the illumos layer.
 */

#ifndef _NVIDIA_NVSWITCH_H_
#define _NVIDIA_NVSWITCH_H_

#include "nv-illumos.h"
#include "nvlink_errors.h"
#include "export_nvswitch.h"

#define NVSWITCH_SHORT_NAME             "nvswi"
#define NVSWITCH_CTL_NAME               "nvidia-nvswitchctl"
#define NVSWITCH_DEVICE_FILE_MODE       0666
#define NVSWITCH_DEVICE_FILE_MODE_PROP  "nvidia-nvswitch-device-file-mode"

#define NVSWITCH_IRQ_NONE               0
#define NVSWITCH_IRQ_MSIX               1
#define NVSWITCH_IRQ_MSI                2
#define NVSWITCH_IRQ_PIN                3

/*
 * The library keeps one entry per REGISTER_EVENTS call and does not reject
 * duplicates; bound them per open.
 */
#define NVSWITCH_MAX_CLIENT_EVENTS      64

typedef struct nvswitch_dev
{
    char                name[sizeof (NVSWITCH_DRIVER_NAME) + 4];
    char                sname[sizeof (NVSWITCH_SHORT_NAME) + 4];
    int                 minor;
    dev_info_t         *dip;
    NvU16               device_id;
    NvU8                bus;
    NvU8                slot;
    NvU8                func;
    NvUuid              uuid;
    NvU32               phys_id;
    NvU64               bios_ver;

    ksema_t             device_lock;
    nvswitch_device    *lib_device;
    void               *os_ctx;
    NvBool              unusable;
    uint32_t            ref_count;      /* opens, under driver_lock */

    ddi_acc_handle_t    pci_cfg;
    ddi_acc_handle_t    bar0_hdl;
    caddr_t             bar0;
    off_t               bar0_size;

    ddi_intr_handle_t   intr_hdl;
    int                 intr_type;
    int                 intr_cap;
    NvU8                irq_mechanism;
    volatile NvBool     intr_enabled;
    NvBool              intr_stopping;  /* under device_lock */
    taskq_t            *bh_tq;          /* interrupt bottom half, wakeups */
    taskq_ent_t         bh_ent;
    volatile uint32_t   bh_pending;
    taskq_ent_t         wake_ent;
    volatile uint32_t   wake_pending;

    taskq_t            *task_tq;
    taskq_ent_t         task_ent;
    kmutex_t            task_lock;
    kcondvar_t          task_cv;
    NvBool              task_q_ready;

    kmutex_t            files_lock;
    list_t              files;
} nvswitch_dev_t;

typedef struct nvswitch_file_private
{
    nvswitch_dev_t     *dev;            /* NULL for nvidia-nvswitchctl */
    list_node_t         link;
    kmutex_t            lock;
    NvBool              event_pending;
    NvBool              wake_pending;
    NvU32               num_events;
    struct pollhead     pollhead;
    file_t *volatile    fabric_mgmt;
} nvswitch_file_private_t;

typedef struct nvswitch_drv
{
    NvBool              initialized;
    ksema_t             driver_lock;
    nvswitch_dev_t     *devs[NVSWITCH_DEVICE_INSTANCE_MAX];
    /* written under driver_lock, read without it by getinfo and quiesce */
    dev_info_t *volatile dips[NVSWITCH_DEVICE_INSTANCE_MAX];
    uint32_t            count;
    uint32_t            ctl_opens;
    dev_info_t         *ctl_dip;
} nvswitch_drv_t;

extern nvswitch_drv_t nvswitch_drv;

/* nvidia_nvlink.c */
int     nvlink_ioc_param_size(uint32_t, size_t *);
NvBool  nvlink_log_ratelimit(void);
file_t *nvlink_fabric_mgmt_hold(int);
void    nvlink_fabric_mgmt_rele(file_t *);

/* nvidia_nvswitch.c */
int     nvswitch_ctl_attach(dev_info_t *);
int     nvswitch_ctl_detach(dev_info_t *);
int     nvswitch_map_status(NvlStatus);
NvBool  nvswitch_is_device_blacklisted(nvswitch_dev_t *);

/* nvidia_nvswitch_os.c */
void   *nvswitch_os_ctx_create(dev_info_t *);
void    nvswitch_os_ctx_destroy(void *);
extern char *NvSwitchRegDwords;
extern char *NvSwitchBlacklist;

#endif /* _NVIDIA_NVSWITCH_H_ */
