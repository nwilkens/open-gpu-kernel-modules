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
 * Locks, wait queues, deferred work and the current task.
 */

#ifndef _UVM_KPI_SYNC_H_
#define _UVM_KPI_SYNC_H_

/*
 * Every lock that UVM may take in its interrupt top half is initialized at
 * the nvidia interrupt priority.  That priority is below LOCK_LEVEL, so the
 * mutexes stay adaptive and the interrupt thread may block on them.
 */
#define LINUX_INTR_PRI          DDI_INTR_PRI(nv_intr_pri)

static inline void linux_kmutex_init(kmutex_t *m, void *ipl)
{
    mutex_init(m, NULL, MUTEX_DRIVER, ipl);
}

/* struct mutex is illumos kmutex_t. */
static inline void linux_mutex_init(struct mutex *m)
{
    linux_kmutex_init((kmutex_t *)m, NULL);
}
#undef mutex_init
#define mutex_init(m)           linux_mutex_init(m)
#define mutex_destroy(m)        mutex_destroy((kmutex_t *)(m))
#define mutex_lock(m)           mutex_enter((kmutex_t *)(m))
#define mutex_lock_nested(m, s) mutex_enter((kmutex_t *)(m))
#define mutex_lock_interruptible(m) (mutex_enter((kmutex_t *)(m)), 0)
#define mutex_trylock(m)        mutex_tryenter((kmutex_t *)(m))
#define mutex_unlock(m)         mutex_exit((kmutex_t *)(m))
#define mutex_is_locked(m)      (mutex_owner((kmutex_t *)(m)) != NULL)

/* A writer releasing a krwlock_t must be the thread that took it. */
#define init_rwsem(s)           rw_init(&(s)->rw, NULL, RW_DRIVER, NULL)
#define down_read(s)            rw_enter(&(s)->rw, RW_READER)
#define down_write(s)           rw_enter(&(s)->rw, RW_WRITER)
#define down_read_trylock(s)    rw_tryenter(&(s)->rw, RW_READER)
#define down_write_trylock(s)   rw_tryenter(&(s)->rw, RW_WRITER)
#define up_read(s)              rw_exit(&(s)->rw)
#define up_write(s)             rw_exit(&(s)->rw)
#define downgrade_write(s)      rw_downgrade(&(s)->rw)
#define rwsem_is_locked(s)      RW_LOCK_HELD(&(s)->rw)

#define spin_lock_init(l)       linux_kmutex_init(&(l)->m, LINUX_INTR_PRI)
#define spin_lock(l)            mutex_enter(&(l)->m)
#define spin_unlock(l)          mutex_exit(&(l)->m)
#define spin_trylock(l)         mutex_tryenter(&(l)->m)
#define spin_is_locked(l)       (mutex_owner(&(l)->m) != NULL)
#define spin_lock_irqsave(l, f)                                             \
    do { ASSERT(getpil() <= LOCK_LEVEL); (f) = 0; mutex_enter(&(l)->m); } while (0)
#define spin_unlock_irqrestore(l, f)                                        \
    do { (void)(f); mutex_exit(&(l)->m); } while (0)
#define spin_lock_irq(l)        mutex_enter(&(l)->m)
#define spin_unlock_irq(l)      mutex_exit(&(l)->m)
#define spin_lock_bh(l)         mutex_enter(&(l)->m)
#define spin_unlock_bh(l)       mutex_exit(&(l)->m)
#define raw_spin_lock_init(l)   spin_lock_init(l)
#define raw_spin_lock_irqsave(l, f)         spin_lock_irqsave(l, f)
#define raw_spin_unlock_irqrestore(l, f)    spin_unlock_irqrestore(l, f)

#define rwlock_init(l)          rw_init(&(l)->rw, NULL, RW_DRIVER, LINUX_INTR_PRI)
#define read_lock(l)            rw_enter(&(l)->rw, RW_READER)
#define read_unlock(l)          rw_exit(&(l)->rw)
#define write_lock(l)           rw_enter(&(l)->rw, RW_WRITER)
#define write_unlock(l)         rw_exit(&(l)->rw)
#define read_lock_irqsave(l, f)         do { (f) = 0; read_lock(l); } while (0)
#define read_unlock_irqrestore(l, f)    do { (void)(f); read_unlock(l); } while (0)
#define write_lock_irqsave(l, f)        do { (f) = 0; write_lock(l); } while (0)
#define write_unlock_irqrestore(l, f)   do { (void)(f); write_unlock(l); } while (0)

/* Linux semaphores may be released by another thread, like ksema_t. */
static inline void linux_sema_init(struct semaphore *sem, int val)
{
    sema_init(&sem->s, (uint_t)val, NULL, SEMA_DRIVER, NULL);
}
#undef sema_init
#define sema_init(sem, v)       linux_sema_init(sem, v)
#define down(sem)               sema_p(&(sem)->s)
#define down_interruptible(sem) (sema_p_sig(&(sem)->s) ? -EINTR : 0)
#define down_trylock(sem)       (sema_tryp(&(sem)->s) == 0)
#define up(sem)                 sema_v(&(sem)->s)

static inline void init_completion(struct completion *c)
{
    linux_kmutex_init(&c->lock, NULL);
    cv_init(&c->cv, NULL, CV_DRIVER, NULL);
    c->done = 0;
}

static inline void complete(struct completion *c)
{
    mutex_enter(&c->lock);
    c->done++;
    cv_signal(&c->cv);
    mutex_exit(&c->lock);
}

static inline void complete_all(struct completion *c)
{
    mutex_enter(&c->lock);
    c->done = UINT_MAX / 2;
    cv_broadcast(&c->cv);
    mutex_exit(&c->lock);
}

static inline void wait_for_completion(struct completion *c)
{
    mutex_enter(&c->lock);
    while (c->done == 0)
        cv_wait(&c->cv, &c->lock);
    c->done--;
    mutex_exit(&c->lock);
}

/*
 * Wait queues.  The condition is rechecked under the queue lock, and
 * wake_up() takes the same lock, so a wakeup cannot be lost between the
 * check and cv_wait().
 */
#define init_waitqueue_head(wq)     linux_init_waitqueue_head(wq)
#define wake_up(wq)                 linux_wake_up_all(wq)
#define wake_up_all(wq)             linux_wake_up_all(wq)
#define wake_up_interruptible(wq)   linux_wake_up_all(wq)
#define wake_up_interruptible_all(wq) linux_wake_up_all(wq)

#define wait_event(wq, cond)                                                \
    do {                                                                    \
        wait_queue_head_t *__wq = &(wq);                                    \
        mutex_enter(&__wq->lock);                                           \
        while (!(cond))                                                     \
            cv_wait(&__wq->cv, &__wq->lock);                                \
        mutex_exit(&__wq->lock);                                            \
    } while (0)

#define wait_event_interruptible(wq, cond)                                  \
    ({                                                                      \
        wait_queue_head_t *__wq = &(wq);                                    \
        int __ret = 0;                                                      \
        mutex_enter(&__wq->lock);                                           \
        while (!(cond)) {                                                   \
            if (cv_wait_sig(&__wq->cv, &__wq->lock) == 0) {                 \
                __ret = -ERESTARTSYS;                                       \
                break;                                                      \
            }                                                               \
        }                                                                   \
        mutex_exit(&__wq->lock);                                            \
        __ret;                                                              \
    })

#define TASK_RUNNING            0
#define TASK_INTERRUPTIBLE      1
#define TASK_UNINTERRUPTIBLE    2

#define wait_on_bit_lock(word, bit, mode)                                   \
    (linux_wait_on_bit_lock((unsigned long *)(word), (int)(bit)), 0)
#define wake_up_bit(word, bit)                                              \
    linux_wake_up_bit((unsigned long *)(word), (int)(bit))

/* Deferred work.  Only delayed work is used, by the thrashing detector. */
#define INIT_DELAYED_WORK(dw, fn)   linux_init_delayed_work(dw, fn)
#define to_delayed_work(w)          container_of(w, struct delayed_work, work)
#define schedule_delayed_work(dw, j) linux_schedule_delayed_work(dw, j)
#define cancel_delayed_work(dw)     linux_cancel_delayed_work(dw)
#define cancel_delayed_work_sync(dw) linux_cancel_delayed_work_sync(dw)

/* The current task. */
#define current                 linux_current()
#define get_current()           linux_current()

static inline pid_t task_pid_vnr(struct task_struct *t)
{
    return t->pid;
}

static inline pid_t task_tgid_vnr(struct task_struct *t)
{
    return t->tgid;
}

#define task_pid_nr(t)          task_pid_vnr(t)
#define task_tgid_nr(t)         task_tgid_vnr(t)
#define KSTK_EIP(t)             0UL
#define TASK_COMM_LEN           (MAXCOMLEN + 1)


static inline bool fatal_signal_pending(struct task_struct *t)
{
    return ISSIG(curthread, JUSTLOOKING) != 0;
}

#define signal_pending(t)       fatal_signal_pending(t)

static inline int set_cpus_allowed_ptr(struct task_struct *t,
    const struct cpumask *mask)
{
    return 0;
}

/* Context queries. */
#define in_interrupt()          (servicing_interrupt() != 0)
#define in_irq()                in_interrupt()
#define in_hardirq()            in_interrupt()
#define in_atomic()             (curthread->t_preempt != 0)
#define irqs_disabled()         (getpil() > LOCK_LEVEL)

#define preempt_disable()       kpreempt_disable()
#define preempt_enable()        kpreempt_enable()
#define smp_processor_id()      ((int)CPU->cpu_id)
#define raw_smp_processor_id()  smp_processor_id()
#define get_cpu()               ({ kpreempt_disable(); smp_processor_id(); })
#define put_cpu()               kpreempt_enable()
#define num_possible_cpus()     ((unsigned int)max_ncpus)
#define num_online_cpus()       ((unsigned int)ncpus_online)
#define nr_cpu_ids              ((unsigned int)max_ncpus)

/* CPU masks. */
static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m)
{
    if (cpu < LINUX_NR_CPUS)
        set_bit(cpu, m->bits);
}

static inline void cpumask_clear(struct cpumask *m)
{
    memset(m, 0, sizeof(*m));
}

static inline bool cpumask_test_cpu(int cpu, const struct cpumask *m)
{
    return cpu >= 0 && cpu < LINUX_NR_CPUS && test_bit(cpu, m->bits);
}

static inline bool cpumask_empty(const struct cpumask *m)
{
    return bitmap_empty(m->bits, LINUX_NR_CPUS);
}

static inline unsigned int cpumask_weight(const struct cpumask *m)
{
    return bitmap_weight(m->bits, LINUX_NR_CPUS);
}

#define cpumask_first(m)        ((unsigned int)find_first_bit((m)->bits, LINUX_NR_CPUS))
#define cpumask_next(n, m)      ((unsigned int)find_next_bit((m)->bits, LINUX_NR_CPUS, (n) + 1))
#define cpumask_bits(m)         ((m)->bits)
#define for_each_cpu(cpu, m)    for_each_set_bit(cpu, (m)->bits, LINUX_NR_CPUS)

extern struct cpumask linux_cpu_all_mask;
#define cpu_online_mask         (&linux_cpu_all_mask)
#define cpu_possible_mask       (&linux_cpu_all_mask)
#define cpumask_of_node(n)      (&linux_cpu_all_mask)

#define DEFINE_PER_CPU(type, name)  __typeof__(type) name[LINUX_NR_CPUS]
#define per_cpu(var, cpu)       ((var)[cpu])
#define get_cpu_var(var)        (*({ kpreempt_disable(); &(var)[CPU->cpu_id]; }))
#define put_cpu_var(var)        kpreempt_enable()
#define this_cpu_ptr(ptr)       (&(*(ptr))[CPU->cpu_id])

#endif /* _UVM_KPI_SYNC_H_ */
