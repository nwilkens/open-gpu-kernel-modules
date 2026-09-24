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
 * Doubly linked lists and the red-black tree interface.
 */

#ifndef _UVM_KPI_LIST_H_
#define _UVM_KPI_LIST_H_

#define LIST_HEAD_INIT(name)    { &(name), &(name) }
#define LIST_HEAD(name)         struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    WRITE_ONCE(list->next, list);
    list->prev = list;
}

static inline void __list_add(struct list_head *new, struct list_head *prev,
    struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    WRITE_ONCE(prev->next, new);
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    WRITE_ONCE(prev->next, next);
}

static inline void __list_del_entry(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
}

static inline void list_del(struct list_head *entry)
{
    __list_del_entry(entry);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del_entry(entry);
    INIT_LIST_HEAD(entry);
}

static inline void list_replace(struct list_head *old, struct list_head *new)
{
    new->next = old->next;
    new->next->prev = new;
    new->prev = old->prev;
    new->prev->next = new;
}

static inline void list_replace_init(struct list_head *old,
    struct list_head *new)
{
    list_replace(old, new);
    INIT_LIST_HEAD(old);
}

static inline void list_move(struct list_head *list, struct list_head *head)
{
    __list_del_entry(list);
    list_add(list, head);
}

/* illumos <sys/list.h> owns list_move_tail. */
static inline void linux_list_move_tail(struct list_head *list,
    struct list_head *head)
{
    __list_del_entry(list);
    list_add_tail(list, head);
}
#define list_move_tail          linux_list_move_tail

static inline int list_is_last(const struct list_head *list,
    const struct list_head *head)
{
    return list->next == head;
}

static inline int list_empty(const struct list_head *head)
{
    return READ_ONCE(head->next) == head;
}

static inline int list_is_singular(const struct list_head *head)
{
    return !list_empty(head) && (head->next == head->prev);
}

static inline void __list_splice(const struct list_head *list,
    struct list_head *prev, struct list_head *next)
{
    struct list_head *first = list->next;
    struct list_head *last = list->prev;

    first->prev = prev;
    prev->next = first;
    last->next = next;
    next->prev = last;
}

static inline void list_splice(const struct list_head *list,
    struct list_head *head)
{
    if (!list_empty(list))
        __list_splice(list, head, head->next);
}

static inline void list_splice_tail(struct list_head *list,
    struct list_head *head)
{
    if (!list_empty(list))
        __list_splice(list, head->prev, head);
}

static inline void list_splice_init(struct list_head *list,
    struct list_head *head)
{
    if (!list_empty(list)) {
        __list_splice(list, head, head->next);
        INIT_LIST_HEAD(list);
    }
}

static inline void list_splice_tail_init(struct list_head *list,
    struct list_head *head)
{
    if (!list_empty(list)) {
        __list_splice(list, head->prev, head);
        INIT_LIST_HEAD(list);
    }
}

static inline void list_cut_position(struct list_head *list,
    struct list_head *head, struct list_head *entry)
{
    struct list_head *new_first = entry->next;

    if (list_empty(head) || (head->next == head && entry != head))
        return;
    if (entry == head) {
        INIT_LIST_HEAD(list);
        return;
    }
    list->next = head->next;
    list->next->prev = list;
    list->prev = entry;
    entry->next = list;
    head->next = new_first;
    new_first->prev = head;
}

#define list_entry(ptr, type, member)       container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member)  list_entry((ptr)->prev, type, member)
#define list_first_entry_or_null(ptr, type, member)                         \
    ({                                                                      \
        struct list_head *__head = (ptr);                                   \
        struct list_head *__pos = READ_ONCE(__head->next);                  \
        __pos != __head ? list_entry(__pos, type, member) : NULL;           \
    })
#define list_next_entry(pos, member)                                        \
    list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member)                                        \
    list_entry((pos)->member.prev, typeof(*(pos)), member)
#define list_entry_is_head(pos, head, member)   (&(pos)->member == (head))

#define list_for_each(pos, head)                                            \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)
#define list_for_each_safe(pos, n, head)                                    \
    for ((pos) = (head)->next, (n) = (pos)->next; (pos) != (head);          \
         (pos) = (n), (n) = (pos)->next)
#define list_for_each_entry(pos, head, member)                              \
    for ((pos) = list_first_entry(head, typeof(*(pos)), member);            \
         !list_entry_is_head(pos, head, member);                            \
         (pos) = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member)                      \
    for ((pos) = list_last_entry(head, typeof(*(pos)), member);             \
         !list_entry_is_head(pos, head, member);                            \
         (pos) = list_prev_entry(pos, member))
#define list_for_each_entry_continue(pos, head, member)                     \
    for ((pos) = list_next_entry(pos, member);                              \
         !list_entry_is_head(pos, head, member);                            \
         (pos) = list_next_entry(pos, member))
#define list_for_each_entry_from(pos, head, member)                         \
    for (; !list_entry_is_head(pos, head, member);                          \
         (pos) = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member)                      \
    for ((pos) = list_first_entry(head, typeof(*(pos)), member),            \
         (n) = list_next_entry(pos, member);                                \
         !list_entry_is_head(pos, head, member);                            \
         (pos) = (n), (n) = list_next_entry(n, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member)              \
    for ((pos) = list_last_entry(head, typeof(*(pos)), member),             \
         (n) = list_prev_entry(pos, member);                                \
         !list_entry_is_head(pos, head, member);                            \
         (pos) = (n), (n) = list_prev_entry(n, member))

#define HLIST_HEAD_INIT         { .first = NULL }
#define INIT_HLIST_HEAD(ptr)    ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
    h->next = NULL;
    h->pprev = NULL;
}

static inline int hlist_empty(const struct hlist_head *h)
{
    return READ_ONCE(h->first) == NULL;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;

    n->next = first;
    if (first)
        first->pprev = &n->next;
    WRITE_ONCE(h->first, n);
    n->pprev = &h->first;
}

static inline void hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;

    WRITE_ONCE(*pprev, next);
    if (next)
        next->pprev = pprev;
    n->next = NULL;
    n->pprev = NULL;
}

/*
 * Red-black trees.  UVM descends the tree itself and then calls
 * rb_link_node() and rb_insert_color(), as on Linux.
 */
#define RB_RED                  0
#define RB_BLACK                1
#define RB_ROOT                 ((struct rb_root) { NULL })
#define rb_parent(r)            ((struct rb_node *)((r)->__rb_parent_color & ~3UL))
#define rb_entry(ptr, type, member)     container_of(ptr, type, member)
#define rb_entry_safe(ptr, type, member)                                    \
    ({ typeof(ptr) ____ptr = (ptr); ____ptr ? rb_entry(____ptr, type, member) : NULL; })
#define RB_EMPTY_ROOT(root)     (READ_ONCE((root)->rb_node) == NULL)
#define RB_EMPTY_NODE(node)     ((node)->__rb_parent_color == (uintptr_t)(node))
#define RB_CLEAR_NODE(node)     ((node)->__rb_parent_color = (uintptr_t)(node))

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
    struct rb_node **rb_link)
{
    node->__rb_parent_color = (uintptr_t)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

void rb_insert_color(struct rb_node *, struct rb_root *);
void rb_erase(struct rb_node *, struct rb_root *);
struct rb_node *rb_next(const struct rb_node *);
struct rb_node *rb_prev(const struct rb_node *);
struct rb_node *rb_first(const struct rb_root *);
struct rb_node *rb_last(const struct rb_root *);

#endif /* _UVM_KPI_LIST_H_ */
