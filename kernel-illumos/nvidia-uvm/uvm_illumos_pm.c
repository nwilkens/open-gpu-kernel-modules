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
 * UVM suspend and resume.  uvm_suspend() write-locks pm.lock and every VA
 * space lock and uvm_resume() releases them.  They are krwlock_t and
 * kmutex_t, which panic if another thread releases them, so both run on
 * one thread of ours whatever thread nvidia calls them from.  A resume
 * without a matching suspend, or a second suspend, fails without calling
 * UVM.
 *
 * The UVM event table goes to RM through a copy whose suspend and resume
 * entries hand off to that thread; the compatibility headers rename UVM's
 * calls to register and deregister it.
 */

#include "uvm_illumos.h"

#include <sys/callb.h>

#include "nvstatus.h"
#include "nv_uvm_types.h"

/* nvidia.kmod; nv_uvm_interface.h is only for the Linux-side sources. */
extern NV_STATUS nvUvmInterfaceRegisterUvmEvents(struct UvmEventsLinux *);
extern void nvUvmInterfaceDeRegisterUvmEvents(void);

typedef enum {
    UVM_PM_SUSPEND,
    UVM_PM_RESUME,
} uvm_pm_op_t;

typedef struct uvm_pm_req {
    uvm_pm_op_t     upr_op;
    NV_STATUS       upr_status;
    boolean_t       upr_done;
} uvm_pm_req_t;

static kmutex_t         uvm_pm_lock;
static kcondvar_t       uvm_pm_cv;
static uvm_pm_req_t    *uvm_pm_req;         /* the request being served */
static boolean_t        uvm_pm_exit;
static kt_did_t         uvm_pm_did;

/* Changed only by the PM thread, under uvm_pm_lock. */
static boolean_t        uvm_pm_suspended;

/* Written only while RM does not have the table. */
static struct UvmEventsLinux uvm_pm_events;
static uvmEventSuspend_t uvm_pm_uvm_suspend;
static uvmEventResume_t uvm_pm_uvm_resume;
static boolean_t        uvm_pm_registered;

static void
uvm_pm_set_suspended(boolean_t suspended)
{
    mutex_enter(&uvm_pm_lock);
    uvm_pm_suspended = suspended;
    mutex_exit(&uvm_pm_lock);
}

static NV_STATUS
uvm_pm_run(uvm_pm_op_t op)
{
    NV_STATUS status;

    if (op == UVM_PM_SUSPEND) {
        if (uvm_pm_suspended) {
            cmn_err(CE_WARN, "%s: suspend while suspended", UVM_ILLUMOS_NAME);
            return (NV_ERR_INVALID_STATE);
        }
        status = uvm_pm_uvm_suspend();
        if (status == NV_OK)
            uvm_pm_set_suspended(B_TRUE);
    } else {
        if (!uvm_pm_suspended) {
            cmn_err(CE_WARN, "%s: resume without suspend", UVM_ILLUMOS_NAME);
            return (NV_ERR_INVALID_STATE);
        }
        status = uvm_pm_uvm_resume();
        uvm_pm_set_suspended(B_FALSE);
    }

    return (status);
}

static void
uvm_pm_main(void *arg)
{
    callb_cpr_t cpr;
    uvm_pm_req_t *req;
    NV_STATUS status;

    CALLB_CPR_INIT(&cpr, &uvm_pm_lock, callb_generic_cpr, "nvidia_uvm_pm");

    mutex_enter(&uvm_pm_lock);
    for (;;) {
        while (((req = uvm_pm_req) == NULL || req->upr_done) &&
            !uvm_pm_exit) {
            CALLB_CPR_SAFE_BEGIN(&cpr);
            cv_wait(&uvm_pm_cv, &uvm_pm_lock);
            CALLB_CPR_SAFE_END(&cpr, &uvm_pm_lock);
        }
        if (req == NULL || req->upr_done)
            break;
        mutex_exit(&uvm_pm_lock);

        status = uvm_pm_run(req->upr_op);

        mutex_enter(&uvm_pm_lock);
        req->upr_status = status;
        req->upr_done = B_TRUE;
        cv_broadcast(&uvm_pm_cv);
    }
    CALLB_CPR_EXIT(&cpr);

    thread_exit();
}

static NV_STATUS
uvm_pm_request(uvm_pm_op_t op)
{
    uvm_pm_req_t req = { op, NV_OK, B_FALSE };

    mutex_enter(&uvm_pm_lock);
    while (uvm_pm_req != NULL)
        cv_wait(&uvm_pm_cv, &uvm_pm_lock);
    uvm_pm_req = &req;
    cv_broadcast(&uvm_pm_cv);
    while (!req.upr_done)
        cv_wait(&uvm_pm_cv, &uvm_pm_lock);
    uvm_pm_req = NULL;
    cv_broadcast(&uvm_pm_cv);
    mutex_exit(&uvm_pm_lock);

    return (req.upr_status);
}

static NV_STATUS
uvm_pm_suspend_entry(void)
{
    return (uvm_pm_request(UVM_PM_SUSPEND));
}

static NV_STATUS
uvm_pm_resume_entry(void)
{
    return (uvm_pm_request(UVM_PM_RESUME));
}

NV_STATUS
uvm_illumos_register_uvm_events(struct UvmEventsLinux *events)
{
    NV_STATUS status;

    if (events == NULL || events->suspend == NULL || events->resume == NULL)
        return (NV_ERR_INVALID_ARGUMENT);

    VERIFY(!uvm_pm_registered);

    uvm_pm_events = *events;
    uvm_pm_uvm_suspend = events->suspend;
    uvm_pm_uvm_resume = events->resume;
    uvm_pm_events.suspend = uvm_pm_suspend_entry;
    uvm_pm_events.resume = uvm_pm_resume_entry;

    status = nvUvmInterfaceRegisterUvmEvents(&uvm_pm_events);
    if (status == NV_OK)
        uvm_pm_registered = B_TRUE;

    return (status);
}

/* RM has finished every callback when this returns. */
void
uvm_illumos_deregister_uvm_events(void)
{
    VERIFY(uvm_pm_registered);

    nvUvmInterfaceDeRegisterUvmEvents();
    uvm_pm_registered = B_FALSE;
}

/* UVM holds its locks between a suspend and the resume. */
boolean_t
uvm_pm_is_suspended(void)
{
    boolean_t suspended;

    mutex_enter(&uvm_pm_lock);
    suspended = uvm_pm_suspended || uvm_pm_req != NULL;
    mutex_exit(&uvm_pm_lock);

    return (suspended);
}

void
uvm_pm_init(void)
{
    mutex_init(&uvm_pm_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&uvm_pm_cv, NULL, CV_DRIVER, NULL);
    uvm_pm_req = NULL;
    uvm_pm_exit = B_FALSE;
    uvm_pm_suspended = B_FALSE;
    uvm_pm_registered = B_FALSE;
    uvm_pm_did = uvm_thread_create(uvm_pm_main, NULL)->t_did;
}

void
uvm_pm_fini(void)
{
    VERIFY(!uvm_pm_registered);

    mutex_enter(&uvm_pm_lock);
    uvm_pm_exit = B_TRUE;
    cv_broadcast(&uvm_pm_cv);
    mutex_exit(&uvm_pm_lock);
    thread_join(uvm_pm_did);

    cv_destroy(&uvm_pm_cv);
    mutex_destroy(&uvm_pm_lock);
}
