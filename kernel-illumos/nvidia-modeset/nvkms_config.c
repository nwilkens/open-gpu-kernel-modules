/*
 * SPDX-FileCopyrightText: Copyright (c) 2015-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * NVKMS configuration file (nvkms_param_config_file) support.
 */

#include "nvidia_modeset_illumos.h"

#include <sys/cred.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/resource.h>

/*
 * Read fname into a buffer from nvkms_alloc() and return its size, or 0 on
 * failure.  This is also the callback nvKmsReadConf() uses for files named
 * in the configuration.
 */
static size_t nvkms_config_file_open
(
    char *fname,
    char ** const buff
)
{
    vnode_t *vp = NULL;
    vattr_t va;
    size_t file_size = 0;
    size_t read_size = 0;
    int i = 0;
    int err;

    *buff = NULL;

    if (fname == NULL) {
        return 0;
    }

    if (rootdir == NULL) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Filesystems not mounted");
        return 0;
    }

    err = vn_open(fname, UIO_SYSSPACE, FREAD | FNONBLOCK, 0, &vp, 0, 0);
    if (err != 0) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Failed to open %s", fname);
        return 0;
    }

    if (vp->v_type != VREG) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "%s is not a regular file", fname);
        goto done;
    }

    bzero(&va, sizeof(va));
    va.va_mask = AT_SIZE;
    if (VOP_GETATTR(vp, &va, 0, kcred, NULL) != 0) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Failed to stat %s", fname);
        goto done;
    }

    if (va.va_size > NVKMS_READ_FILE_MAX_SIZE) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "File exceeds maximum size");
        goto done;
    }

    // Do not alloc a 0 sized buffer
    if (va.va_size == 0) {
        goto done;
    }
    file_size = (size_t)va.va_size;

    *buff = nvkms_alloc(file_size, NV_FALSE);
    if (*buff == NULL) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Out of memory");
        goto done;
    }

    while ((read_size < file_size) && (i++ < NVKMS_READ_FILE_MAX_LOOPS)) {
        ssize_t len = (ssize_t)(file_size - read_size);
        ssize_t resid = 0;

        err = vn_rdwr(UIO_READ, vp, *buff + read_size, len,
                      (offset_t)read_size, UIO_SYSSPACE, 0,
                      RLIM64_INFINITY, kcred, &resid);
        if (err != 0 || resid == len) {
            break;
        }
        read_size += (size_t)(len - resid);
    }

    if (read_size != file_size) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Failed to read %s", fname);
        goto done;
    }

    (void) VOP_CLOSE(vp, FREAD | FNONBLOCK, 1, (offset_t)0, kcred, NULL);
    VN_RELE(vp);

    return file_size;

done:
    nvkms_free(*buff, file_size);
    *buff = NULL;
    (void) VOP_CLOSE(vp, FREAD | FNONBLOCK, 1, (offset_t)0, kcred, NULL);
    VN_RELE(vp);

    return 0;
}

static void nvkms_read_config_file_locked(char *fname)
{
    char *buffer = NULL;
    size_t buf_size = 0;

    /* only read the config file if the tunable is set */
    if (fname == NULL) {
        return;
    }

    buf_size = nvkms_config_file_open(fname, &buffer);

    if (buf_size == 0) {
        return;
    }

    if (nvKmsReadConf(buffer, buf_size, nvkms_config_file_open)) {
        cmn_err(CE_CONT, "!" NVKMS_LOG_PREFIX "Successfully read %s\n", fname);
    }

    nvkms_free(buffer, buf_size);
}

static void nvkms_read_config_file_cb(void *arg)
{
    (void) nvkms_lock_down(NV_FALSE);
    nvkms_read_config_file_locked(nvkms_param_config_file);
    nvkms_lock_up();
}

/*
 * Read the configuration file on nvkms_kthread_q, whose thread belongs to
 * p0, so that paths resolve against the global root with kernel
 * credentials even when a zone process triggered the module load.
 */
void nvkms_read_config_file(void)
{
    nvkms_q_item_t item;

    if (nvkms_param_config_file == NULL) {
        return;
    }

    nvkms_q_item_init(&item, nvkms_read_config_file_cb, NULL);
    nvkms_queue_work(&nvkms_kthread_q, &item);
    nvkms_q_flush(&nvkms_kthread_q);
}
