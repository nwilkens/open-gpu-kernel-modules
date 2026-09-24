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

#ifndef _NV_ILLUMOS_H_
#define _NV_ILLUMOS_H_

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/id_space.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/open.h>
#include <sys/cred.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/taskq.h>
#include <sys/taskq_impl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_intr.h>
#include <sys/ddidevmap.h>
#include <sys/pci.h>
#include <sys/sunndi.h>

/*
 * The NVIDIA headers below expect a freestanding environment and supply their
 * own NV_* helpers; the illumos headers above are all that the interface layer
 * needs from the system.
 */
#include "os-interface.h"
#include "nv.h"
#include "nv-caps.h"
#include "nv-firmware.h"
#include "nv-chardev-numbers.h"
#include "nv-illumos-pci-dev.h"

#define NV_ILLUMOS_DRIVER_NAME          "nvidia"

/* Memory-management debug prints, as in the Linux layer. */
#define NV_DBG_MEMINFO                  NV_DBG_INFO

/*
 * Minor number layout of the nvidia driver.
 *
 * The low 16 bits select the node that was opened; each open(9E) clones a
 * new minor whose upper 16 bits carry a non-zero clone id, so per-open
 * state can be looked up from the dev_t passed to the other entry points.
 *
 *   0 .. NV_MAX_DEVICES-1      /dev/nvidiaN
 *   0xff                       /dev/nvidiactl
 *   0x100                      /dev/nvidia-nvlink
 *   0x101                      /dev/nvidia-nvswitchctl
 *   0x200 ..                   /dev/nvidia-nvswitchN
 *   0x1000 ..                  /dev/nvidia-caps/nvidia-capN
 *   0x4000 ..                  /dev/nvidia-caps-imex-channels/channelN
 */
#define NV_MINOR_NODE_MASK              0xffffU
#define NV_MINOR_CLONE_SHIFT            16
#define NV_MINOR_CTL                    NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE
#define NV_MINOR_NVLINK                 0x100U
#define NV_MINOR_NVSWITCH_CTL           0x101U
#define NV_MINOR_NVSWITCH_BASE          0x200U
#define NV_MINOR_NVSWITCH_COUNT         0x100U
#define NV_MINOR_CAPS_BASE              0x1000U
#define NV_MINOR_CAPS_COUNT             0x2000U
#define NV_MINOR_IMEX_BASE              0x4000U
#define NV_MINOR_IMEX_COUNT             0x4000U
#define NV_MINOR_CLONE_MAX              0xffffU

#define NV_MINOR_NODE(m)                ((m) & NV_MINOR_NODE_MASK)
#define NV_MINOR_CLONE(m)               ((m) >> NV_MINOR_CLONE_SHIFT)
#define NV_MINOR_MAKE(clone, node)      \
    ((minor_t)(((clone) << NV_MINOR_CLONE_SHIFT) | (node)))
#define NV_MINOR_IS_GPU(n)              ((n) < NV_MAX_DEVICES)
#define NV_MINOR_IS_CAP(n)              \
    ((n) >= NV_MINOR_CAPS_BASE && (n) < NV_MINOR_CAPS_BASE + NV_MINOR_CAPS_COUNT)
#define NV_MINOR_IS_IMEX(n)             \
    ((n) >= NV_MINOR_IMEX_BASE && (n) < NV_MINOR_IMEX_BASE + NV_MINOR_IMEX_COUNT)
#define NV_MINOR_IS_NVSWITCH(n)         \
    ((n) >= NV_MINOR_NVSWITCH_BASE && \
     (n) < NV_MINOR_NVSWITCH_BASE + NV_MINOR_NVSWITCH_COUNT)

/* Minor node types matched by devlink.tab. */
#define NV_DDI_NT_NVIDIA                "ddi_pseudo:nvidia"
#define NV_DDI_NT_CAPS                  "ddi_pseudo:nvidia-caps"
#define NV_DDI_NT_IMEX                  "ddi_pseudo:nvidia-caps-imex-channels"

typedef enum {
    NV_NODE_CTL,
    NV_NODE_GPU,
    NV_NODE_CAP,
    NV_NODE_IMEX,
    NV_NODE_NVLINK,
    NV_NODE_NVSWITCH_CTL,
    NV_NODE_NVSWITCH
} nv_node_kind_t;

/*
 * NVIDIA ioctl commands are _IOWR('F', nr, type).  The direction bits and the
 * type/number bytes of the illumos and Linux encodings agree; the size field
 * is 8 bits wide on illumos (IOCPARM_MASK) and 14 bits on Linux.  Commands
 * whose argument does not fit use NV_ESC_IOCTL_XFER_CMD.
 */
#define NV_PLATFORM_MAX_IOCTL_SIZE      IOCPARM_MASK
#define NV_IOC_INOUT                    0xc0000000U
#define NV_IOC_DIR(cmd)                 ((cmd) & 0xc0000000U)
#define NV_IOC_TYPE(cmd)                (((cmd) >> 8) & 0xffU)
#define NV_IOC_NR(cmd)                  ((cmd) & 0xffU)
#define NV_IOC_SIZE(cmd)                (((cmd) >> 16) & 0x3fffU)

/* Page table entry for a system memory allocation. */
typedef struct nvidia_pte_s {
    NvU64       phys_addr;      /* CPU physical address */
    NvU64       dma_addr;       /* bus address from the DMA bind, if bound */
    caddr_t     virt_addr;      /* kernel virtual address */
} nvidia_pte_t;

/*
 * System memory allocation, handed to RM as the opaque "priv" of
 * nv_alloc_pages() and friends.  The reference count covers RM's own
 * reference plus one per user mapping and per mmap context, so the pages
 * outlive a free that races with munmap().
 */
typedef struct nv_illumos_alloc_s {
    volatile uint64_t   refcnt;
    dev_info_t         *dip;
    struct {
        NvBool contig       : 1;
        NvBool zeroed       : 1;
        NvBool user         : 1;
        NvBool peer_io      : 1;
        NvBool physical     : 1;
        NvBool unencrypted  : 1;
        NvBool node         : 1;
        NvBool sgt          : 1;
    } flags;
    NvU32               cache_type;
    NvU32               num_pages;      /* in OS pages */
    NvU64               size;           /* in bytes */
    nvidia_pte_t       *page_table;
    NvS32               node_id;
    pid_t               pid;

    /* DDI DMA memory backing a driver allocation */
    NvU32               num_chunks;
    struct nv_dma_chunk *chunks;

    /* ddi_umem cookie describing the kernel mapping for devmap */
    ddi_umem_cookie_t   umem_cookie;
    caddr_t             kva;            /* contiguous kernel mapping */

    /* user pages registered through nv_register_user_pages() */
    void               *user_pages;
    void               *import_priv;
} nv_illumos_alloc_t;

typedef struct nv_dma_chunk {
    ddi_dma_handle_t    dma_handle;
    ddi_acc_handle_t    acc_handle;
    caddr_t             kva;
    size_t              len;
} nv_dma_chunk_t;

typedef struct nv_illumos_state_s nv_illumos_state_t;

/* DMA-capable device, opaque to RM. */
struct nv_dma_device {
    dev_info_t         *dip;
    nv_illumos_state_t *nvis;
    struct {
        NvU64 start;
        NvU64 limit;
    } addressable_range;
    NvBool              nvlink;
};

/* Work queue backing os_queue_work_item(), opaque to RM. */
struct os_work_queue {
    taskq_t            *tq;
    volatile NvBool     is_unload_flush_ongoing;
};

/* Wait queue backing os_wait_*(); a one-shot completion like Linux's. */
struct os_wait_queue {
    kmutex_t            lock;
    kcondvar_t          cv;
    NvBool              done;
};

typedef enum {
    NV_DEV_STACK_TIMER,
    NV_DEV_STACK_ISR,
    NV_DEV_STACK_ISR_BH,
    NV_DEV_STACK_ISR_BH_UNLOCKED,
    NV_DEV_STACK_GPU_WAKEUP,
    NV_DEV_STACK_COUNT
} nvidia_dev_stack_t;

/* Queued RM event, delivered through NV_ESC_RM_GET_EVENT_DATA. */
typedef struct nvidia_event_s {
    nv_event_t              event;
    struct nvidia_event_s  *next;
} nvidia_event_t;

/* Per-open state, keyed by clone minor. */
typedef struct nv_illumos_file_private_s {
    nv_file_private_t       nvfp;

    minor_t                 minor;          /* full clone minor */
    nv_node_kind_t          kind;
    nv_illumos_state_t     *nvis;           /* NULL until the open succeeds */
    void                   *subsys_priv;    /* nvlink/nvswitch open state */
    NvBool                  closing;
    nvidia_stack_t         *sp;
    int                     open_rc;
    NV_STATUS               adapter_status;
    uint32_t                refcnt;         /* open + nv_get_file_private() */
    kmutex_t                ref_lock;
    taskq_ent_t             destroy_ent;

    /* event queue, protected by fp_lock (may be taken from interrupts) */
    kmutex_t                fp_lock;
    nvidia_event_t         *event_head;
    nvidia_event_t         *event_tail;
    NvBool                  dataless_event_pending;
    struct pollhead         pollhead;

    /* mmap context installed by RM_MAP_MEMORY, protected by file_va_lock */
    krwlock_t               file_va_lock;
    nv_alloc_mapping_list_node_t *file_mapping_list;

    /* GPUs attached through NV_ESC_ATTACH_GPUS_TO_FD */
    NvU32                  *attached_gpus;
    size_t                  num_attached_gpus;

    /* capability node opened through this file, if any */
    struct nv_cap          *cap;

    /* devmap handles created through this file, for revocation */
    list_node_t             open_link;      /* nvis->open_files */
    list_t                  mappings;       /* nv_devmap_priv_t */
} nv_illumos_file_private_t;

static inline nv_illumos_file_private_t *
nv_get_nvifp_from_nvfp(nv_file_private_t *nvfp)
{
    return ((nv_illumos_file_private_t *)
        ((uintptr_t)nvfp - offsetof(nv_illumos_file_private_t, nvfp)));
}

/*
 * One devmap(9E) mapping.  The driver private of each devmap handle points
 * here; dup and partial unmap create additional instances.
 */
typedef struct nv_devmap_priv_s {
    list_node_t                     link;       /* nvifp->mappings */
    nv_illumos_file_private_t      *nvifp;
    devmap_cookie_t                 dhp;
    nv_illumos_alloc_t                     *at;         /* sysmem mappings only */
    offset_t                        off;
    size_t                          len;
    NvBool                          device_memory;
} nv_devmap_priv_t;

/* Per-GPU (and control device) state. */
struct nv_illumos_state_s {
    nv_state_t              nv_state;

    dev_info_t             *dip;
    struct pci_dev          pci_dev;        /* handed to nvidia-uvm */
    int                     instance;
    NvU32                   minor_num;      /* index for /dev/nvidiaN */
    nv_illumos_state_t     *next;

    ddi_acc_handle_t        pci_cfg;
    int                     bar_rnumber[NV_GPU_NUM_BARS];

    /* interrupts */
    ddi_intr_handle_t      *intr_htable;
    size_t                  intr_htable_size;
    int                     intr_type;
    int                     intr_count;
    uint_t                  intr_pri;
    int                     intr_cap;
    NvBool                  intr_enabled;
    kmutex_t                isr_lock;       /* serializes MSI-X top halves */
    struct nv_intr_priv_s  *intr_priv;      /* bottom-half taskqs */
    volatile uint32_t       bh_pending;
    volatile uint32_t       bh_unlocked_pending;

    nvidia_stack_t         *sp[NV_DEV_STACK_COUNT];

    /* serializes open/close/start/stop, like Linux's ldata_lock */
    ksema_t                 ldata_lock;
    volatile uint64_t       usage_count;

    /* per-GPU work queue */
    struct os_work_queue    queue;

    /* RC watchdog timer */
    kmutex_t                timer_lock;
    timeout_id_t            rc_timer;
    NvBool                  rc_timer_armed;

    /* user mapping revocation; protected by mmap_lock */
    ksema_t                 mmap_lock;
    list_t                  open_files;
    NvBool                  all_mappings_revoked;
    NvBool                  safe_to_mmap;
    NvBool                  gpu_wakeup_callback_needed;

    struct nv_dma_device    dma_dev;
    struct nv_dma_device    niso_dma_dev;
    NvBool                  dma_remap;      /* an IOMMU translates DMA */
    void                   *acpi_object;    /* ACPI notify registration */

    NvU64                   numa_memblock_size;

    char                    registry_keys[512];
};

#define NV_STATE_PTR(nvis)          (&(nvis)->nv_state)
#define NV_GET_NVIS(nv)             ((nv_illumos_state_t *)(nv)->os_state)

extern nv_illumos_state_t   nv_ctl_device;
extern nv_illumos_state_t  *nv_illumos_devices;
extern krwlock_t            nv_illumos_devices_lock;
extern krwlock_t            nv_adapter_state_lock;
extern dev_info_t          *nv_ctl_dip;
extern void                *nv_softstate;
extern kmem_cache_t        *nv_stack_cache;
extern struct os_work_queue nv_global_queue;
extern uint_t               nv_intr_pri;
extern NvU32                cur_debuglevel;

/* nvidia_ddi.c */
nv_illumos_state_t *nv_find_minor(NvU32 minor);
nv_illumos_state_t *nv_find_minor_locked(NvU32 minor);
int  nv_dev_alloc_stacks(nv_illumos_state_t *);
void nv_dev_free_stacks(nv_illumos_state_t *);
void nv_shutdown_adapter(nvidia_stack_t *, nv_state_t *);
int  nv_start_device(nv_state_t *, nvidia_stack_t *);
void nv_stop_device(nv_state_t *, nvidia_stack_t *);
int  nv_open_device(nv_state_t *, nvidia_stack_t *);
void nv_close_device(nv_state_t *, nvidia_stack_t *);
int  nvidia_dev_get(NvU32 gpu_id, nvidia_stack_t *sp, NvBool reset_aware);
void nvidia_dev_put(NvU32 gpu_id, nvidia_stack_t *sp, NvBool reset_aware);
#define READ_ONCE(x)            (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)        (*(volatile __typeof__(x) *)&(x) = (v))

extern NvBool nv_ats_supported;
extern NvBool nv_non_ats_device_present;

void      nv_pm_init(void);
void      nv_pm_fini(void);
int       nv_suspend_gpu(nv_illumos_state_t *);
int       nv_resume_gpu(nv_illumos_state_t *);
void      nv_intr_init(void);
void      nv_intr_fini(void);
void      nv_drain_isr_top_halves(void);
int       nv_uvm_init(void);
void      nv_uvm_exit(void);
NV_STATUS nv_uvm_suspend(void);
NV_STATUS nv_uvm_resume(void);
NV_STATUS nv_uvm_event_interrupt(const NvU8 *uuid);
NV_STATUS nv_uvm_drain_P2P(const NvU8 *uuid);
NV_STATUS nv_uvm_resume_P2P(const NvU8 *uuid);

int  nvidia_dev_get_uuid(const NvU8 *uuid, nvidia_stack_t *sp);
void nvidia_dev_put_uuid(const NvU8 *uuid, nvidia_stack_t *sp);
int  nvidia_dev_block_gc6(const NvU8 *uuid, nvidia_stack_t *sp);
int  nvidia_dev_unblock_gc6(const NvU8 *uuid, nvidia_stack_t *sp);
int  nvidia_dev_get_pci_info(const NvU8 *uuid, struct pci_dev **pci_dev_out,
    NvU64 *dma_start, NvU64 *dma_limit);

/* nvidia_cb.c */
extern struct cb_ops nvidia_cb_ops;
int  nv_clone_init(void);
void nv_clone_fini(void);
nv_illumos_file_private_t *nv_clone_lookup(minor_t minor);
nv_illumos_file_private_t *nv_file_private_hold(dev_t dev);
void nv_file_private_rele(nv_illumos_file_private_t *);
int  nvidia_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);
file_t *nv_cap_hold_file(const nv_cap_t *, int);
void    nv_cap_rele_file(file_t *);

extern char *NvSwitchRegDwords;
extern char *NvSwitchBlacklist;

/* nvidia_intr.c */
int  nv_intr_setup(nv_illumos_state_t *);
void nv_intr_teardown(nv_illumos_state_t *);

/* nvidia_os.c */
void nv_os_init(void);
int  nv_stack_alloc(nvidia_stack_t **);
void nv_stack_free(nvidia_stack_t *);
int  nv_queue_init(struct os_work_queue *, const char *);
void nv_queue_fini(struct os_work_queue *);
NvBool nv_may_sleep(void);

/* nvidia_mem.c */
extern struct devmap_callback_ctl nv_devmap_callbacks;
void nv_dma_init(void);
void nv_dma_fini(void);
NvBool nv_dma_detect_remap(dev_info_t *);
int  nv_devmap_sysmem(nv_illumos_file_private_t *, devmap_cookie_t,
         nv_alloc_mapping_context_t *, offset_t, size_t, size_t *);
int  nv_devmap_devmem(nv_illumos_file_private_t *, devmap_cookie_t,
         nv_alloc_mapping_context_t *, offset_t, size_t, size_t *);
void nv_revoke_mappings_locked(nv_illumos_state_t *);
void nv_alloc_hold(nv_illumos_alloc_t *);
void nv_alloc_rele(nv_illumos_alloc_t *);

/* nvidia_pci.c */
int  nv_pci_map_bars(nv_illumos_state_t *);
int  nv_pci_dip_bdf(dev_info_t *, NvU8 *, NvU8 *, NvU8 *);

/* nvidia_registry.c */
void nv_registry_load(dev_info_t *);
void nv_registry_unload(void);
NV_STATUS nv_parse_per_device_option_string(nvidia_stack_t *, nv_state_t *);
NvBool nv_is_uuid_in_gpu_exclusion_list(const char *uuid);
extern char *NVreg_RegistryDwordsPerDevice;
extern char *NVreg_GpuBlacklist;
extern char *NVreg_ExcludedGpus;
extern char *NVreg_TemporaryFilePath;
extern NvU32 NVreg_EnableDbgBreakpoint;
extern NvU32 NVreg_ModifyDeviceFiles;
extern NvU32 NVreg_GpuInitOnProbe;
extern NvU32 NVreg_EnableUserNUMAManagement;
extern NvU32 NVreg_ImexChannelCount;
extern NvU32 NVreg_CreateImexChannel0;
extern NvU32 nv_dma_remap_peer_mmio;

/* nvidia_caps.c */
int  nv_caps_init(void);
void nv_caps_fini(void);
void nv_caps_attach(dev_info_t *);
int  nv_caps_open(nv_illumos_file_private_t *, NvU32 node, cred_t *);
void nv_caps_close(nv_illumos_file_private_t *);
int  nv_caps_imex_init(void);
void nv_caps_imex_fini(void);
NvBool nv_caps_imex_minor_valid(NvU32 node);

/* nvidia_registry.c accessors for per-module registry keys */
NvU32 nv_reg_device_file_uid(void);
NvU32 nv_reg_device_file_gid(void);
NvU32 nv_reg_enable_msi(void);

/* nvidia_acpi.c */
void nv_acpi_register_notifier(nv_illumos_state_t *);
void nv_acpi_unregister_notifier(nv_illumos_state_t *);

/* nvidia_modeset_interface.c */
void nvidia_modeset_suspend(NvU32 gpu_id);
void nvidia_modeset_resume(NvU32 gpu_id);

/* nvidia_nvlink.c / nvidia_nvswitch.c */
int  nvlink_drivers_init(void);
void nvlink_drivers_exit(void);
int  nvlink_node_open(nv_illumos_file_private_t *, cred_t *);
void nvlink_node_close(nv_illumos_file_private_t *);
int  nvlink_node_ioctl(nv_illumos_file_private_t *, int, intptr_t, int, cred_t *);
int  nvswitch_node_open(nv_illumos_file_private_t *, NvU32 node, cred_t *);
void nvswitch_node_close(nv_illumos_file_private_t *);
int  nvswitch_node_ioctl(nv_illumos_file_private_t *, int, intptr_t, int, cred_t *);
int  nvswitch_node_chpoll(nv_illumos_file_private_t *, short, int, short *,
         struct pollhead **);
NvBool nvswitch_dip_is_nvswitch(dev_info_t *);
int  nvswitch_attach(dev_info_t *);
int  nvswitch_detach(dev_info_t *);
int  nvswitch_suspend(dev_info_t *);
int  nvswitch_resume(dev_info_t *);
int  nvswitch_quiesce(dev_info_t *);
void nvlink_ctl_attach(dev_info_t *);
int  nvlink_ctl_detach(dev_info_t *);
dev_info_t *nvswitch_minor_to_dip(NvU32 node);

/* Errno conversion; RM wants Linux-style negative errno from some hooks. */
static inline int nv_status_to_errno(NV_STATUS status)
{
    switch (status)
    {
        case NV_OK:                     return (0);
        case NV_ERR_NO_MEMORY:          return (ENOMEM);
        case NV_ERR_INSUFFICIENT_PERMISSIONS: return (EPERM);
        case NV_ERR_NOT_SUPPORTED:      return (ENOTSUP);
        case NV_ERR_BUSY_RETRY:         return (EAGAIN);
        case NV_ERR_INVALID_ADDRESS:    return (EFAULT);
        default:                        return (EINVAL);
    }
}

#endif /* _NV_ILLUMOS_H_ */
