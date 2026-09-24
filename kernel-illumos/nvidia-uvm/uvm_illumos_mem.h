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
 * Memory interfaces shared by the illumos-side sources of nvidia_uvm:
 * locked-memory charges, pinned user pages and the per-thread record of
 * mmap lock holds.
 */

#ifndef _UVM_ILLUMOS_MEM_H_
#define _UVM_ILLUMOS_MEM_H_

#include "uvm_kpi_types.h"

/*
 * A charge against the locked-memory resource controls of a project and
 * its zone, taken in the context of the process that asked for the memory
 * and released from any thread.  Shared by reference count.
 */
typedef struct uvm_charge uvm_charge_t;

/* The project and zone that opened a UVM file, held while the file lives. */
typedef struct uvm_acct_owner uvm_acct_owner_t;

/* uvm_illumos_acct.c */
uvm_charge_t *uvm_charge_take(size_t, int *);
uvm_charge_t *uvm_charge_take_owner(const uvm_acct_owner_t *, size_t, int *);
uvm_acct_owner_t *uvm_acct_owner_create(int *);
void    uvm_acct_owner_free(uvm_acct_owner_t *);
void    uvm_acct_quarantine(size_t);
void    uvm_charge_hold(uvm_charge_t *);
void    uvm_charge_rele(uvm_charge_t *);
void    uvm_charge_rele_deferred(uvm_charge_t *, const void *);
void    uvm_charge_release_owner(const void *);
void    uvm_charge_page_add(uvm_charge_t *, struct page *);
uvm_charge_t *uvm_charge_page_remove(struct page *);
void    uvm_acct_init(void);
void    uvm_acct_fini(void);

/* uvm_illumos_page.c */
boolean_t uvm_page_owned(const struct page *);

/* uvm_illumos_dma.c */
boolean_t uvm_dma_may_free(struct page *, pgcnt_t);
void    uvm_dma_init(void);
void    uvm_dma_fini(void);

/* uvm_illumos_pin.c */
boolean_t uvm_pin_vmap(struct page **, unsigned int, void **);
boolean_t uvm_pin_vunmap(const void *);
void   *uvm_pin_kmap(struct page *);
boolean_t uvm_pin_busy(void);
void    uvm_mmap_note(struct as *, boolean_t, int);
void    uvm_mmap_holds(struct as *, uint_t *, uint_t *);
void   *uvm_memcg_swap(void *);
void   *uvm_memcg_active(void);
void    uvm_pin_init(void);
void    uvm_pin_fini(void);

#endif /* _UVM_ILLUMOS_MEM_H_ */
