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

#ifndef _NVIDIA_MODESET_ILLUMOS_H_
#define _NVIDIA_MODESET_ILLUMOS_H_

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/disp.h>
#include <sys/archsystm.h>
#include <sys/stack.h>
#include <sys/ioccom.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "nv-modeset-interface.h"
#include "nvidia-modeset-os-interface.h"
#include "nvkms.h"

#define NVKMS_LOG_PREFIX            "nvidia-modeset: "
#define NVKMS_MINOR_NAME            "nvidia-modeset"
#define NVKMS_NVIDIA_DRIVER_NAME    "nvidia"

/*
 * Single-threaded FIFO work queue, the illumos port of nv_kthread_q.
 * Items are embedded in their owners, so scheduling never allocates and may
 * be done from timeout(9F) callbacks and low-level interrupt threads.
 */
typedef void (*nvkms_q_func_t)(void *args);

typedef struct nvkms_q_item {
    list_node_t     q_list_node;
    nvkms_q_func_t  function_to_run;
    void           *function_args;
} nvkms_q_item_t;

typedef struct nvkms_q {
    kmutex_t        q_lock;
    kcondvar_t      q_cv;
    list_t          q_list;
    ddi_taskq_t    *q_taskq;
    NvBool          main_loop_should_exit;
} nvkms_q_t;

int  nvkms_q_init(nvkms_q_t *q, const char *qname);
void nvkms_q_stop(nvkms_q_t *q);
void nvkms_q_item_init(nvkms_q_item_t *q_item, nvkms_q_func_t function_to_run,
                       void *function_args);
int  nvkms_q_schedule(nvkms_q_t *q, nvkms_q_item_t *q_item);
void nvkms_q_flush(nvkms_q_t *q);

/*
 * Reader/writer semaphore without an owner: unlike krwlock_t, the write
 * side may be released by a thread other than the one that took it, which
 * the suspend/resume callbacks need.
 */
typedef struct nvkms_rwsem {
    kmutex_t        lock;
    kcondvar_t      cv;
    uint32_t        readers;
    uint32_t        writers_waiting;
    NvBool          writer;
} nvkms_rwsem_t;

void   nvkms_rwsem_init(nvkms_rwsem_t *sem);
void   nvkms_rwsem_destroy(nvkms_rwsem_t *sem);
void   nvkms_rwsem_down_read(nvkms_rwsem_t *sem);
int    nvkms_rwsem_down_read_sig(nvkms_rwsem_t *sem);
NvBool nvkms_rwsem_down_read_trylock(nvkms_rwsem_t *sem);
void   nvkms_rwsem_up_read(nvkms_rwsem_t *sem);
void   nvkms_rwsem_down_write(nvkms_rwsem_t *sem);
void   nvkms_rwsem_up_write(nvkms_rwsem_t *sem);

/* nvidia_modeset_illumos.c */
extern nvkms_rwsem_t nvkms_pm_lock;
extern nvkms_q_t nvkms_kthread_q;
extern nvidia_modeset_rm_ops_t nvkms_rm_ops;
int  nvkms_lock_down(NvBool interruptible);
void nvkms_lock_up(void);

/* nvkms_os.c */
extern int nvkms_param_malloc_verbose;
extern char *nvkms_param_config_file;
extern volatile uint32_t nvkms_alloc_called_count;
void nvkms_copy_mode_enter(int mode);
void nvkms_copy_mode_exit(void);
void nvkms_queue_work(nvkms_q_t *q, nvkms_q_item_t *q_item);
void nvkms_timers_init(void);
void nvkms_timers_fini(void);
void nvkms_cancel_timers(void);

/* nvkms_config.c */
void nvkms_read_config_file(void);

#endif /* _NVIDIA_MODESET_ILLUMOS_H_ */
