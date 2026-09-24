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
 * Part 1 of the Linux compatibility layer for nvidia-uvm: every illumos
 * header the layer needs, and the Linux types shared by the UVM objects and
 * the illumos-side sources in kernel-illumos/nvidia-uvm.  Nothing here
 * renames an illumos identifier, so illumos headers may still follow.
 */

#ifndef _UVM_KPI_TYPES_H_
#define _UVM_KPI_TYPES_H_

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/taskq_impl.h>
#include <sys/semaphore.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/avl.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/thread.h>
#include <sys/proc.h>
#include <sys/cpuvar.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_intr.h>
#include <sys/disp.h>
#include <sys/archsystm.h>
#include <sys/signal.h>
#include <vm/page.h>
#include <stdbool.h>
#include <stdarg.h>

#include "nv-illumos-pci-dev.h"
#include "kpi/uvm_kpi_const.h"
#include "uvm_kpi_params.h"

/* UVM threads and split stacks, for UVM paths that reach RM. */
#define UVM_THREAD_STACK_SIZE   (64 * 1024)

/* The Linux 64-bit types are long long, as NvU64 is. */
typedef unsigned char           u8;
typedef unsigned short          u16;
typedef unsigned int            u32;
typedef unsigned long long      u64;
typedef signed char             s8;
typedef short                   s16;
typedef int                     s32;
typedef long long               s64;
typedef u8                      __u8;
typedef u16                     __u16;
typedef u32                     __u32;
typedef u64                     __u64;
typedef s32                     __s32;
typedef s64                     __s64;

typedef long long               loff_t;
typedef u64                     phys_addr_t;
typedef u64                     dma_addr_t;
typedef u64                     resource_size_t;
typedef unsigned int            gfp_t;
typedef unsigned long           pgoff_t;
typedef unsigned int            vm_fault_t;
typedef unsigned long           vm_flags_t;
typedef unsigned int            fmode_t;
typedef unsigned int            __poll_t;
typedef int64_t                 ktime_t;

typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile s64 counter; } atomic64_t;
typedef struct { volatile long counter; } atomic_long_t;

typedef struct { uint_t prot; } pgprot_t;

struct timespec64 {
    int64_t tv_sec;
    long    tv_nsec;
};

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev;
};

struct hlist_head {
    struct hlist_node *first;
};

/* Red-black tree; the parent pointer and color share one word. */
struct rb_node {
    uintptr_t       __rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
    struct rb_node *rb_node;
};

/* struct mutex is the tag of illumos kmutex_t. */

struct rw_semaphore {
    krwlock_t       rw;
};

typedef struct {
    kmutex_t        m;
} spinlock_t;

typedef struct {
    krwlock_t       rw;
} rwlock_t;

struct semaphore {
    ksema_t         s;
};

struct completion {
    kmutex_t        lock;
    kcondvar_t      cv;
    unsigned int    done;
};

struct linux_file;

/*
 * A wait queue that a file's poll routine registered with poll_wait()
 * remembers that file; wake_up() then posts pollwakeup() on the file's
 * pollhead from a worker thread, because UVM calls wake_up() with locks
 * held that its poll routine also takes.
 */
typedef struct wait_queue_head {
    kmutex_t            lock;
    kcondvar_t          cv;
    struct linux_file  *poll_file;
} wait_queue_head_t;

typedef struct poll_table_struct {
    struct pollhead    *ph;
} poll_table;

struct work_struct;
typedef void (*work_func_t)(struct work_struct *);

struct work_struct {
    work_func_t     func;
};

struct delayed_work {
    struct work_struct  work;
    timeout_id_t        tid;
    boolean_t           armed;          /* timeout pending */
    boolean_t           queued;         /* on the worker list */
    boolean_t           running;
    list_node_t         link;
};

/* Per-thread stand-in for the Linux task, from linux_current(). */
struct task_struct {
    struct mm_struct   *mm;
    pid_t               pid;            /* thread id, like Linux */
    pid_t               tgid;
    unsigned int        flags;
    char                comm[MAXCOMLEN + 1];
    kthread_t          *thread;
    kt_did_t            did;
    void               *stack;
    struct vm_area_struct *shadow_vma;  /* find_vma() of foreign segments */
    struct linux_kthread *kthread;      /* from kthread_run() */
};

#define PF_KTHREAD              0x00200000

/* Pointers of this type are really struct as pointers. */
struct mm_struct {
    atomic_t            mm_users;
};

struct vm_area_struct;
struct vm_fault;

struct vm_operations_struct {
    void        (*open)(struct vm_area_struct *);
    void        (*close)(struct vm_area_struct *);
    vm_fault_t  (*fault)(struct vm_fault *);
    vm_fault_t  (*page_mkwrite)(struct vm_fault *);
};

struct vm_area_struct {
    unsigned long                       vm_start;
    unsigned long                       vm_end;
    struct mm_struct                   *vm_mm;
    pgprot_t                            vm_page_prot;
    vm_flags_t                          vm_flags;
    unsigned long                       vm_pgoff;
    struct linux_file                  *vm_file;
    void                               *vm_private_data;
    const struct vm_operations_struct  *vm_ops;
    void                               *anon_vma;
    struct vm_area_struct              *vm_next;
    struct vm_area_struct              *vm_prev;
};

struct vm_fault {
    struct vm_area_struct  *vma;
    unsigned int            flags;
    pgoff_t                 pgoff;
    unsigned long           address;
    struct page            *page;
};

/*
 * One per UVM open file.  am_segs lists the seg_nvuvm segments mapping it,
 * for unmap_mapping_range().
 */
struct address_space {
    struct inode       *host;
    const void         *a_ops;
    kmutex_t            am_lock;
    list_t              am_segs;
    boolean_t           am_initialized;
};

struct inode {
    struct address_space   *i_mapping;
    loff_t                  i_size;
};

struct file_operations;

struct linux_file {
    void                           *private_data;
    struct address_space           *f_mapping;
    const struct file_operations   *f_op;
    struct inode                   *f_inode;
    minor_t                         f_minor;    /* clone minor */
    uint_t                          f_node;     /* base minor */
    kmutex_t                        f_lock;
    uint_t                          f_count;    /* open + segments + holds */
    boolean_t                       f_open;     /* between open and close */
    boolean_t                       f_poll_queued;
    struct pollhead                 f_pollhead;
    list_node_t                     f_release_link;
    list_node_t                     f_poll_link;
} __attribute__((aligned(8)));

typedef struct linux_file linux_file_t;

struct module;

struct file_operations {
    struct module  *owner;
    int         (*open)(struct inode *, struct linux_file *);
    int         (*release)(struct inode *, struct linux_file *);
    int         (*mmap)(struct linux_file *, struct vm_area_struct *);
    long        (*unlocked_ioctl)(struct linux_file *, unsigned int,
                    unsigned long);
    long        (*compat_ioctl)(struct linux_file *, unsigned int,
                    unsigned long);
    unsigned    (*poll)(struct linux_file *, poll_table *);
};

struct cdev {
    const struct file_operations   *ops;
    struct module                  *owner;
    dev_t                           dev;
    unsigned int                    count;
};

/* Types named by code that the illumos build compiles out. */
struct seq_file { void *private; };
struct proc_dir_entry;
struct dev_pagemap_ops;
struct dev_pagemap {
    const struct dev_pagemap_ops   *ops;
    void                           *owner;
    int                             type;
    int                             nr_range;
    struct { phys_addr_t start, end; } range;
};
struct dma_buf;
struct dma_buf_attachment;
struct dma_buf_attach_ops;
struct dma_resv;
struct dma_iova_state { dma_addr_t addr; size_t size; };
struct mempolicy;
struct mmu_interval_notifier { unsigned long invalidate_seq; };
struct mmu_interval_notifier_ops;
struct mmu_notifier_range;
struct mmu_notifier { void *ops; };
struct hmm_range;
struct migrate_vma;
struct iommu_domain;
struct iommu_sva;
struct folio;
struct mem_cgroup;
struct resource { resource_size_t start, end; };
struct acpi_iort_node;
struct acpi_iort_smmu_v3;
/* NCPU is not a constant outside machine-dependent code. */
#define LINUX_NR_CPUS           1024

struct cpumask { unsigned long bits[LINUX_NR_CPUS / 64]; };
typedef struct cpumask cpumask_t;
typedef struct { unsigned long bits[1]; } nodemask_t;

struct scatterlist {
    unsigned long       page_link;
    unsigned int        offset;
    unsigned int        length;
    dma_addr_t          dma_address;
    unsigned int        dma_length;
};

struct sg_table {
    struct scatterlist *sgl;
    unsigned int        nents;
    unsigned int        orig_nents;
};

struct sg_page_iter {
    struct scatterlist *sg;
    unsigned int        sg_pgoffset;
    unsigned int        __nents;
    int                 __pg_advance;
};

struct sg_dma_page_iter {
    struct sg_page_iter base;
};

/* Radix trees are AVL trees keyed by index, for the builtin tests. */
struct radix_tree_root {
    avl_tree_t      rt_tree;
    gfp_t           rt_gfp;
};

struct radix_tree_iter {
    unsigned long   index;
};

struct ratelimit_state {
    hrtime_t        begin;
    int             printed;
};

/* uvm_illumos_kpi.c */
extern uint_t nv_intr_pri;

struct task_struct *linux_current(void);

void   *linux_kmalloc(size_t, gfp_t);
void   *linux_krealloc(const void *, size_t, gfp_t);
void    linux_kfree(const void *);
size_t  linux_ksize(const void *);
void   *linux_vmalloc(size_t, boolean_t);
void    linux_vfree(const void *);
boolean_t linux_is_vmalloc_addr(const void *);
struct kmem_cache *linux_kmem_cache_create(const char *, size_t, size_t);
void   *linux_kmem_cache_alloc(struct kmem_cache *, gfp_t);

int     linux_printk(const char *, ...)
            __attribute__((format(printf, 1, 2)));
int     linux_vprintk(const char *, va_list);
int     linux_vsnprintf(char *, size_t, const char *, va_list);
int     linux_snprintf(char *, size_t, const char *, ...)
            __attribute__((format(printf, 3, 4)));
int     linux_sprintf(char *, const char *, ...)
            __attribute__((format(printf, 2, 3)));
boolean_t linux_ratelimit(struct ratelimit_state *);
void    linux_dump_stack(void);

void    linux_usleep_range(unsigned long, unsigned long);
void    linux_msleep(unsigned int);
void    linux_schedule(void);

void    linux_init_waitqueue_head(wait_queue_head_t *);
void    linux_wake_up_all(wait_queue_head_t *);
void    linux_poll_wait(struct linux_file *, wait_queue_head_t *, poll_table *);

void    linux_init_delayed_work(struct delayed_work *, work_func_t);
boolean_t linux_schedule_delayed_work(struct delayed_work *, unsigned long);
boolean_t linux_cancel_delayed_work(struct delayed_work *);
boolean_t linux_cancel_delayed_work_sync(struct delayed_work *);

void    linux_wait_on_bit_lock(unsigned long *, int);
void    linux_wake_up_bit(unsigned long *, int);

void    linux_sort(void *, size_t, size_t, int (*)(const void *, const void *));
void    linux_get_random_bytes(void *, size_t);

void    linux_task_adopt(struct task_struct *);
void    linux_task_disown(void);
void    linux_task_destroy(struct task_struct *);

/* uvm_illumos_test.c */
void    linux_radix_tree_init(struct radix_tree_root *, gfp_t);
void   *linux_radix_tree_lookup(struct radix_tree_root *, unsigned long);
int     linux_radix_tree_insert(struct radix_tree_root *, unsigned long,
            void *);
void   *linux_radix_tree_delete(struct radix_tree_root *, unsigned long);
void  **linux_radix_tree_iter_first(struct radix_tree_root *,
            struct radix_tree_iter *, unsigned long);
void  **linux_radix_tree_iter_next(struct radix_tree_root *,
            struct radix_tree_iter *);
int     linux_remap_pfn_range(struct vm_area_struct *, unsigned long,
            unsigned long, unsigned long, pgprot_t);
void   *linux_phys_to_virt(phys_addr_t);
struct task_struct *linux_kthread_run(int (*)(void *), void *);
int     linux_kthread_stop(struct task_struct *);
bool    linux_kthread_should_stop(void);


/* uvm_illumos.c */
struct linux_file *linux_fget(unsigned int);
void    linux_fput(struct linux_file *);
int     linux_cdev_add(struct cdev *, dev_t, unsigned int);
void    linux_cdev_del(struct cdev *);

/* uvm_illumos_page.c */
struct page *linux_alloc_pages(gfp_t, unsigned int);
void    linux_free_pages(struct page *, unsigned int);
void    linux_put_page(struct page *);
void   *linux_page_address(struct page *);
unsigned long linux_page_to_pfn(struct page *);
struct page *linux_pfn_to_page(unsigned long);
void    linux_set_page_dirty(struct page *);
void   *linux_vmap(struct page **, unsigned int);
void    linux_vunmap(const void *);
dma_addr_t linux_dma_map_page(struct device *, struct page *, size_t, size_t);
void    linux_dma_unmap_page(struct device *, dma_addr_t, size_t);
void   *linux_dma_alloc_coherent(struct device *, size_t, dma_addr_t *, gfp_t);
void    linux_dma_free_coherent(struct device *, size_t, void *, dma_addr_t);
long    linux_pin_user_pages(unsigned long, unsigned long, unsigned int,
            struct page **);
void    linux_unpin_user_page(struct page *);

/* uvm_seg.c */
void    linux_address_space_init_once(struct address_space *);
void    linux_unmap_mapping_range(struct address_space *, loff_t, loff_t, int);
int     linux_vm_insert_page(struct vm_area_struct *, unsigned long,
            struct page *);
struct vm_area_struct *linux_find_vma(struct mm_struct *, unsigned long);
struct vm_area_struct *linux_find_vma_intersection(struct mm_struct *,
            unsigned long, unsigned long);
void    linux_mmap_read_lock(struct mm_struct *);
void    linux_mmap_read_unlock(struct mm_struct *);
void    linux_mmap_write_lock(struct mm_struct *);
void    linux_mmap_write_unlock(struct mm_struct *);
boolean_t linux_mmap_is_locked(struct mm_struct *);
void   *linux_mmap_get_lock(struct mm_struct *);

#endif /* _UVM_KPI_TYPES_H_ */
