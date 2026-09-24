/*
 * SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * Capability and IMEX channel device nodes.
 *
 * Capabilities are opened by path under /dev/nvidia-caps and the resulting
 * fd is handed to RM, which validates it against the capability it guards.
 * Linux assigns each capability path a fixed minor; the same table is used
 * here and published as the "nvidia-caps-minors" property of the control
 * node, the counterpart of the /proc/driver/nvidia-caps minors files.
 *
 * Linux enforces the per-capability mode through the device file that
 * nvidia-modprobe creates.  Here the minor nodes are world accessible and
 * open(9E) checks the mode RM gave the capability.
 */

#include "nv-illumos.h"
#include "nv-reg.h"

#include <sys/vnode.h>
#include <sys/fcntl.h>

#define NV_CAP_NAME_BUF_SIZE    128

typedef struct nv_cap_table_entry {
    const char *name;
    int         minor;
} nv_cap_table_entry_t;

static nv_cap_table_entry_t g_nv_cap_nvlink_table[] =
{
    {"/driver/nvidia-nvlink/capabilities/fabric-mgmt"}
};

static nv_cap_table_entry_t g_nv_cap_mig_table[] =
{
    {"/driver/nvidia/capabilities/mig/config"},
    {"/driver/nvidia/capabilities/mig/monitor"}
};

static nv_cap_table_entry_t g_nv_cap_sys_table[] =
{
    {"/driver/nvidia/capabilities/fabric-imex-mgmt"},
    {"/driver/nvidia/capabilities/profiler-device"},
    {"/driver/nvidia/capabilities/profiler-context"},
    {"/driver/nvidia/capabilities/trace-device"},
    {"/driver/nvidia/capabilities/wpps-access"},
};

#define NV_CAP_MIG_CI_ENTRIES(_gi)  \
    {_gi "/ci0/access"},            \
    {_gi "/ci1/access"},            \
    {_gi "/ci2/access"},            \
    {_gi "/ci3/access"},            \
    {_gi "/ci4/access"},            \
    {_gi "/ci5/access"},            \
    {_gi "/ci6/access"},            \
    {_gi "/ci7/access"}

#define NV_CAP_MIG_GI_ENTRIES(_gpu)       \
    {_gpu "/gi0/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi0"),   \
    {_gpu "/gi1/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi1"),   \
    {_gpu "/gi2/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi2"),   \
    {_gpu "/gi3/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi3"),   \
    {_gpu "/gi4/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi4"),   \
    {_gpu "/gi5/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi5"),   \
    {_gpu "/gi6/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi6"),   \
    {_gpu "/gi7/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi7"),   \
    {_gpu "/gi8/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi8"),   \
    {_gpu "/gi9/access"},                 \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi9"),   \
    {_gpu "/gi10/access"},                \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi10"),  \
    {_gpu "/gi11/access"},                \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi11"),  \
    {_gpu "/gi12/access"},                \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi12"),  \
    {_gpu "/gi13/access"},                \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi13"),  \
    {_gpu "/gi14/access"},                \
    NV_CAP_MIG_CI_ENTRIES(_gpu "/gi14")

static nv_cap_table_entry_t g_nv_cap_mig_gpu_table[] =
{
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu0/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu1/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu2/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu3/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu4/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu5/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu6/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu7/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu8/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu9/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu10/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu11/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu12/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu13/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu14/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu15/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu16/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu17/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu18/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu19/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu20/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu21/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu22/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu23/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu24/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu25/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu26/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu27/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu28/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu29/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu30/mig"),
    NV_CAP_MIG_GI_ENTRIES("/driver/nvidia/capabilities/gpu31/mig")
};

static struct {
    nv_cap_table_entry_t   *table;
    int                     count;
} nv_cap_tables[] = {
    { g_nv_cap_nvlink_table,  NV_ARRAY_ELEMENTS(g_nv_cap_nvlink_table) },
    { g_nv_cap_mig_table,     NV_ARRAY_ELEMENTS(g_nv_cap_mig_table) },
    { g_nv_cap_mig_gpu_table, NV_ARRAY_ELEMENTS(g_nv_cap_mig_gpu_table) },
    { g_nv_cap_sys_table,     NV_ARRAY_ELEMENTS(g_nv_cap_sys_table) },
};

struct nv_cap {
    char       *path;
    char       *name;
    int         minor;          /* -1 for directories */
    int         permissions;
    int         modify;
};

nv_cap_t *nvidia_caps_root;

/* Capability file entries by minor, for open(9E). */
static nv_cap_t *nv_cap_by_minor[NV_MINOR_CAPS_COUNT];
static kmutex_t nv_caps_lock;

static int
nv_cap_find_minor(const char *path)
{
    size_t t;
    int i;

    for (t = 0; t < NV_ARRAY_ELEMENTS(nv_cap_tables); t++)
    {
        for (i = 0; i < nv_cap_tables[t].count; i++)
        {
            if (strcmp(path, nv_cap_tables[t].table[i].name) == 0)
                return nv_cap_tables[t].table[i].minor;
        }
    }

    return -1;
}

static void
nv_cap_tables_init(void)
{
    size_t t;
    int i, minor = 0;

    for (t = 0; t < NV_ARRAY_ELEMENTS(nv_cap_tables); t++)
    {
        for (i = 0; i < nv_cap_tables[t].count; i++)
            nv_cap_tables[t].table[i].minor = minor++;
    }

    ASSERT(minor <= NV_MINOR_CAPS_COUNT);
}

/* Publish "name minor" pairs, like the procfs *-minors files. */
static void
nv_cap_publish_minors(dev_info_t *dip)
{
    char **entries;
    uint_t n = 0, total = 0;
    size_t t;
    int i;

    for (t = 0; t < NV_ARRAY_ELEMENTS(nv_cap_tables); t++)
        total += nv_cap_tables[t].count;

    entries = kmem_zalloc(total * sizeof (char *), KM_SLEEP);

    for (t = 0; t < NV_ARRAY_ELEMENTS(nv_cap_tables); t++)
    {
        for (i = 0; i < nv_cap_tables[t].count; i++)
        {
            entries[n] = kmem_alloc(NV_CAP_NAME_BUF_SIZE, KM_SLEEP);
            (void) snprintf(entries[n], NV_CAP_NAME_BUF_SIZE, "%s %d",
                nv_cap_tables[t].table[i].name, nv_cap_tables[t].table[i].minor);
            n++;
        }
    }

    (void) ddi_prop_update_string_array(DDI_DEV_T_NONE, dip,
        "nvidia-caps-minors", entries, n);

    for (i = 0; i < (int)n; i++)
        kmem_free(entries[i], NV_CAP_NAME_BUF_SIZE);
    kmem_free(entries, total * sizeof (char *));
}

static char *
nv_cap_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *d = kmem_alloc(len, KM_SLEEP);

    bcopy(s, d, len);
    return d;
}

static nv_cap_t *
nv_cap_alloc(nv_cap_t *parent_cap, const char *name)
{
    nv_cap_t *cap;
    size_t len;

    if (parent_cap == NULL || name == NULL)
        return NULL;

    cap = kmem_zalloc(sizeof (*cap), KM_SLEEP);

    len = strlen(parent_cap->path) + strlen(name) + 2;
    cap->path = kmem_alloc(len, KM_SLEEP);
    (void) snprintf(cap->path, len, "%s/%s", parent_cap->path, name);
    cap->name = nv_cap_strdup(name);
    cap->minor = -1;
    cap->modify = NVreg_ModifyDeviceFiles;

    return cap;
}

static void
nv_cap_free(nv_cap_t *cap)
{
    kmem_free(cap->path, strlen(cap->path) + 1);
    kmem_free(cap->name, strlen(cap->name) + 1);
    kmem_free(cap, sizeof (*cap));
}

nv_cap_t* NV_API_CALL os_nv_cap_create_file_entry(nv_cap_t *parent_cap,
    const char *name, int mode)
{
    nv_cap_t *cap;
    char node[32];
    int minor;

    cap = nv_cap_alloc(parent_cap, name);
    if (cap == NULL)
        return NULL;

    cap->permissions = mode;

    minor = nv_cap_find_minor(cap->path);
    if (minor < 0 || minor >= (int)NV_MINOR_CAPS_COUNT || nv_ctl_dip == NULL)
    {
        nv_cap_free(cap);
        return NULL;
    }
    cap->minor = minor;

    mutex_enter(&nv_caps_lock);
    if (nv_cap_by_minor[minor] != NULL)
    {
        mutex_exit(&nv_caps_lock);
        nv_cap_free(cap);
        return NULL;
    }
    nv_cap_by_minor[minor] = cap;
    mutex_exit(&nv_caps_lock);

    (void) snprintf(node, sizeof (node), "nvidia-cap%d", minor);
    if (ddi_create_minor_node(nv_ctl_dip, node, S_IFCHR,
            NV_MINOR_CAPS_BASE + minor, NV_DDI_NT_CAPS, 0) != DDI_SUCCESS)
    {
        mutex_enter(&nv_caps_lock);
        nv_cap_by_minor[minor] = NULL;
        mutex_exit(&nv_caps_lock);
        nv_cap_free(cap);
        return NULL;
    }

    return cap;
}

nv_cap_t* NV_API_CALL os_nv_cap_create_dir_entry(nv_cap_t *parent_cap,
    const char *name, int mode)
{
    nv_cap_t *cap;

    cap = nv_cap_alloc(parent_cap, name);
    if (cap == NULL)
        return NULL;

    cap->permissions = mode;
    return cap;
}

static nv_cap_t *
nv_caps_root_init(const char *path)
{
    nv_cap_t parent_cap;
    char *name;
    size_t len;
    nv_cap_t *cap;

    len = strlen(path) + strlen("/capabilities") + 1;
    name = kmem_alloc(len, KM_SLEEP);
    (void) snprintf(name, len, "%s/capabilities", path);

    bzero(&parent_cap, sizeof (parent_cap));
    parent_cap.path = "";
    parent_cap.name = "";
    cap = os_nv_cap_create_dir_entry(&parent_cap, name, 0555);

    kmem_free(name, len);
    return cap;
}

void NV_API_CALL os_nv_cap_destroy_entry(nv_cap_t *cap)
{
    char node[32];

    if (cap == NULL)
        return;

    if (cap->minor >= 0)
    {
        mutex_enter(&nv_caps_lock);
        if (nv_cap_by_minor[cap->minor] == cap)
            nv_cap_by_minor[cap->minor] = NULL;
        mutex_exit(&nv_caps_lock);

        if (nv_ctl_dip != NULL)
        {
            (void) snprintf(node, sizeof (node), "nvidia-cap%d", cap->minor);
            ddi_remove_minor_node(nv_ctl_dip, node);
        }
    }

    nv_cap_free(cap);
}

/*
 * Apply the capability's mode against NVreg_DeviceFileUID/GID, the owner and
 * group Linux gives the device files.  Capabilities are opened read-only.
 */
int
nv_caps_open(nv_illumos_file_private_t *nvifp, NvU32 node, cred_t *credp)
{
    int minor = (int)(node - NV_MINOR_CAPS_BASE);
    nv_cap_t *cap;
    int mode, bit;

    mutex_enter(&nv_caps_lock);
    cap = nv_cap_by_minor[minor];
    mode = (cap != NULL) ? cap->permissions : 0;
    mutex_exit(&nv_caps_lock);

    if (cap == NULL)
        return (ENXIO);

    if (crgetuid(credp) == (uid_t)nv_reg_device_file_uid())
        bit = S_IRUSR;
    else if (groupmember((gid_t)nv_reg_device_file_gid(), credp))
        bit = S_IRGRP;
    else
        bit = S_IROTH;

    if ((mode & bit) == 0 && drv_priv(credp) != 0)
        return (EACCES);

    return (0);
}

void
nv_caps_close(nv_illumos_file_private_t *nvifp)
{
}

/*
 * Checks that fd is an open capability node for cap and duplicates it into
 * the calling process with close-on-exec set, as fcntl(F_DUPFD_CLOEXEC).
 */
int NV_API_CALL os_nv_cap_validate_and_dup_fd(const nv_cap_t *cap, int fd)
{
    file_t *fp;
    vnode_t *vp;
    int dup_fd;

    if (cap == NULL || cap->minor < 0 || nv_ctl_dip == NULL || curproc == &p0)
        return -1;

    if ((fp = getf(fd)) == NULL)
        return -1;

    vp = fp->f_vnode;
    if (vp == NULL || vp->v_type != VCHR ||
        getmajor(vp->v_rdev) != ddi_driver_major(nv_ctl_dip) ||
        NV_MINOR_NODE(getminor(vp->v_rdev)) != NV_MINOR_CAPS_BASE + cap->minor)
    {
        releasef(fd);
        return -1;
    }

    mutex_enter(&fp->f_tlock);
    fp->f_count++;
    mutex_exit(&fp->f_tlock);

    dup_fd = ufalloc_file(0, fp);
    if (dup_fd == -1)
    {
        mutex_enter(&fp->f_tlock);
        fp->f_count--;
        mutex_exit(&fp->f_tlock);
        releasef(fd);
        return -1;
    }

    f_setfd_or(dup_fd, FD_CLOEXEC);
    releasef(fd);

    return dup_fd;
}

/*
 * RM may drop a duplicated capability fd from an unrelated context during
 * teardown; only close it when it is still our capability node in the
 * calling process, and leave the rest to process exit.
 */
void NV_API_CALL os_nv_cap_close_fd(int fd)
{
    file_t *fp;
    vnode_t *vp;
    NvBool is_cap;

    if (fd < 0 || curproc == &p0 || nv_ctl_dip == NULL ||
        (curproc->p_flag & SEXITING))
        return;

    if ((fp = getf(fd)) == NULL)
        return;

    vp = fp->f_vnode;
    is_cap = (vp != NULL && vp->v_type == VCHR &&
              getmajor(vp->v_rdev) == ddi_driver_major(nv_ctl_dip) &&
              NV_MINOR_IS_CAP(NV_MINOR_NODE(getminor(vp->v_rdev))));
    releasef(fd);

    if (is_cap)
        (void) closeandsetf(fd, NULL);
}

nv_cap_t* NV_API_CALL os_nv_cap_init(const char *path)
{
    return nv_caps_root_init(path);
}

/*
 * Holds the file behind fd if it is an open of cap's device node.  NVLink
 * and NVSwitch keep the hold rather than the descriptor number, which the
 * process can close and reuse.
 */
file_t *
nv_cap_hold_file(const nv_cap_t *cap, int fd)
{
    file_t *fp;
    vnode_t *vp;

    if (cap == NULL || cap->minor < 0 || nv_ctl_dip == NULL || fd < 0)
        return (NULL);
    if ((fp = getf(fd)) == NULL)
        return (NULL);

    vp = fp->f_vnode;
    if (vp == NULL || vp->v_type != VCHR ||
        getmajor(vp->v_rdev) != ddi_driver_major(nv_ctl_dip) ||
        NV_MINOR_NODE(getminor(vp->v_rdev)) != NV_MINOR_CAPS_BASE + cap->minor)
    {
        releasef(fd);
        return (NULL);
    }

    mutex_enter(&fp->f_tlock);
    fp->f_count++;
    mutex_exit(&fp->f_tlock);
    releasef(fd);

    return (fp);
}

void
nv_cap_rele_file(file_t *fp)
{
    if (fp != NULL)
        (void) closef(fp);
}

int
nv_caps_init(void)
{
    mutex_init(&nv_caps_lock, NULL, MUTEX_DRIVER, NULL);
    nv_cap_tables_init();

    nvidia_caps_root = nv_caps_root_init("driver/" NV_ILLUMOS_DRIVER_NAME);
    if (nvidia_caps_root == NULL)
    {
        mutex_destroy(&nv_caps_lock);
        return (ENOENT);
    }

    return (0);
}

void
nv_caps_fini(void)
{
    if (nvidia_caps_root != NULL)
    {
        os_nv_cap_destroy_entry(nvidia_caps_root);
        nvidia_caps_root = NULL;
    }
    mutex_destroy(&nv_caps_lock);
}

/* Called when the control node attaches, so the table can be published. */
void
nv_caps_attach(dev_info_t *dip)
{
    nv_cap_publish_minors(dip);

    if (NVreg_ImexChannelCount != 0 && NVreg_CreateImexChannel0 == 1)
    {
        if (ddi_create_minor_node(dip, "channel0", S_IFCHR,
                NV_MINOR_IMEX_BASE, NV_DDI_NT_IMEX, 0) == DDI_SUCCESS)
        {
            nv_printf(NV_DBG_ERRORS, "nv-caps-imex channel0 created. "
                "Make sure you are aware of the IMEX security model.\n");
        }
    }
}

/*
 * IMEX channels.  Channel N is minor NV_MINOR_IMEX_BASE + N.  Only channel0
 * gets a node from the driver (world accessible, when requested); other
 * channels are created by the administrator with mknod(8), whose
 * permissions then govern access, as on Linux.
 */
int
nv_caps_imex_init(void)
{
    if (NVreg_ImexChannelCount > NV_MINOR_IMEX_COUNT)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: NVreg_ImexChannelCount %u too large, using %u\n",
            NVreg_ImexChannelCount, NV_MINOR_IMEX_COUNT);
        NVreg_ImexChannelCount = NV_MINOR_IMEX_COUNT;
    }

    if (NVreg_ImexChannelCount == 0)
        nv_printf(NV_DBG_INFO, "nv-caps-imex is disabled.\n");

    return (0);
}

void
nv_caps_imex_fini(void)
{
}

NvBool
nv_caps_imex_minor_valid(NvU32 node)
{
    return (node >= NV_MINOR_IMEX_BASE &&
            node < NV_MINOR_IMEX_BASE + NVreg_ImexChannelCount);
}

NvS32 NV_API_CALL os_imex_channel_count(void)
{
    return (NvS32)NVreg_ImexChannelCount;
}

NvS32 NV_API_CALL os_imex_channel_get(NvU64 descriptor)
{
    int fd = (int)descriptor;
    file_t *fp;
    vnode_t *vp;
    int channel = -1;

    if (nv_ctl_dip == NULL || (fp = getf(fd)) == NULL)
        return -1;

    vp = fp->f_vnode;
    if (vp != NULL && vp->v_type == VCHR &&
        getmajor(vp->v_rdev) == ddi_driver_major(nv_ctl_dip))
    {
        NvU32 node = NV_MINOR_NODE(getminor(vp->v_rdev));

        if (nv_caps_imex_minor_valid(node))
            channel = (int)(node - NV_MINOR_IMEX_BASE);
    }

    releasef(fd);
    return channel;
}
