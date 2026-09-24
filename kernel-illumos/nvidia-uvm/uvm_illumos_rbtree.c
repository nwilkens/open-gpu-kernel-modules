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
 * Red-black tree with the Linux rb_* interface, written for illumos from
 * the textbook algorithm.  The node's parent pointer and color share
 * __rb_parent_color; nodes are at least 4-byte aligned.
 */

#include "uvm_illumos_kpi.h"

#define RB_COLOR(n)             ((n)->__rb_parent_color & 1)
#define RB_IS_RED(n)            ((n) != NULL && RB_COLOR(n) == RB_RED)
#define RB_IS_BLACK(n)          ((n) == NULL || RB_COLOR(n) == RB_BLACK)

static inline void
rb_set_parent(struct rb_node *n, struct rb_node *p)
{
    n->__rb_parent_color = (n->__rb_parent_color & 1) | (uintptr_t)p;
}

static inline void
rb_set_color(struct rb_node *n, int color)
{
    n->__rb_parent_color = (n->__rb_parent_color & ~1UL) | (uintptr_t)color;
}

/* Make new take old's place under old's parent. */
static inline void
rb_replace_child(struct rb_node *old, struct rb_node *new,
    struct rb_node *parent, struct rb_root *root)
{
    if (parent == NULL)
        root->rb_node = new;
    else if (parent->rb_left == old)
        parent->rb_left = new;
    else
        parent->rb_right = new;
}

static void
rb_rotate_left(struct rb_node *x, struct rb_root *root)
{
    struct rb_node *y = x->rb_right;
    struct rb_node *parent = rb_parent(x);

    x->rb_right = y->rb_left;
    if (y->rb_left != NULL)
        rb_set_parent(y->rb_left, x);
    rb_set_parent(y, parent);
    rb_replace_child(x, y, parent, root);
    y->rb_left = x;
    rb_set_parent(x, y);
}

static void
rb_rotate_right(struct rb_node *x, struct rb_root *root)
{
    struct rb_node *y = x->rb_left;
    struct rb_node *parent = rb_parent(x);

    x->rb_left = y->rb_right;
    if (y->rb_right != NULL)
        rb_set_parent(y->rb_right, x);
    rb_set_parent(y, parent);
    rb_replace_child(x, y, parent, root);
    y->rb_right = x;
    rb_set_parent(x, y);
}

void
rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent, *gparent, *uncle;

    rb_set_color(node, RB_RED);

    while ((parent = rb_parent(node)) != NULL && RB_IS_RED(parent)) {
        gparent = rb_parent(parent);

        if (parent == gparent->rb_left) {
            uncle = gparent->rb_right;
            if (RB_IS_RED(uncle)) {
                rb_set_color(uncle, RB_BLACK);
                rb_set_color(parent, RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }
            if (node == parent->rb_right) {
                rb_rotate_left(parent, root);
                node = parent;
                parent = rb_parent(node);
            }
            rb_set_color(parent, RB_BLACK);
            rb_set_color(gparent, RB_RED);
            rb_rotate_right(gparent, root);
        } else {
            uncle = gparent->rb_left;
            if (RB_IS_RED(uncle)) {
                rb_set_color(uncle, RB_BLACK);
                rb_set_color(parent, RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }
            if (node == parent->rb_left) {
                rb_rotate_right(parent, root);
                node = parent;
                parent = rb_parent(node);
            }
            rb_set_color(parent, RB_BLACK);
            rb_set_color(gparent, RB_RED);
            rb_rotate_left(gparent, root);
        }
    }

    rb_set_color(root->rb_node, RB_BLACK);
}

/*
 * Restore the black height after removing a black node.  node took the
 * removed node's place under parent and may be NULL.
 */
static void
rb_erase_fixup(struct rb_node *node, struct rb_node *parent,
    struct rb_root *root)
{
    struct rb_node *sib;

    while (node != root->rb_node && RB_IS_BLACK(node)) {
        if (node == parent->rb_left) {
            sib = parent->rb_right;
            if (RB_IS_RED(sib)) {
                rb_set_color(sib, RB_BLACK);
                rb_set_color(parent, RB_RED);
                rb_rotate_left(parent, root);
                sib = parent->rb_right;
            }
            if (RB_IS_BLACK(sib->rb_left) && RB_IS_BLACK(sib->rb_right)) {
                rb_set_color(sib, RB_RED);
                node = parent;
                parent = rb_parent(node);
                continue;
            }
            if (RB_IS_BLACK(sib->rb_right)) {
                rb_set_color(sib->rb_left, RB_BLACK);
                rb_set_color(sib, RB_RED);
                rb_rotate_right(sib, root);
                sib = parent->rb_right;
            }
            rb_set_color(sib, RB_COLOR(parent));
            rb_set_color(parent, RB_BLACK);
            rb_set_color(sib->rb_right, RB_BLACK);
            rb_rotate_left(parent, root);
            node = root->rb_node;
            break;
        } else {
            sib = parent->rb_left;
            if (RB_IS_RED(sib)) {
                rb_set_color(sib, RB_BLACK);
                rb_set_color(parent, RB_RED);
                rb_rotate_right(parent, root);
                sib = parent->rb_left;
            }
            if (RB_IS_BLACK(sib->rb_left) && RB_IS_BLACK(sib->rb_right)) {
                rb_set_color(sib, RB_RED);
                node = parent;
                parent = rb_parent(node);
                continue;
            }
            if (RB_IS_BLACK(sib->rb_left)) {
                rb_set_color(sib->rb_right, RB_BLACK);
                rb_set_color(sib, RB_RED);
                rb_rotate_left(sib, root);
                sib = parent->rb_left;
            }
            rb_set_color(sib, RB_COLOR(parent));
            rb_set_color(parent, RB_BLACK);
            rb_set_color(sib->rb_left, RB_BLACK);
            rb_rotate_right(parent, root);
            node = root->rb_node;
            break;
        }
    }

    if (node != NULL)
        rb_set_color(node, RB_BLACK);
}

void
rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *child, *parent;
    int color;

    if (node->rb_left != NULL && node->rb_right != NULL) {
        /* Replace node with its in-order successor. */
        struct rb_node *succ = node->rb_right;

        while (succ->rb_left != NULL)
            succ = succ->rb_left;

        child = succ->rb_right;
        parent = rb_parent(succ);
        color = RB_COLOR(succ);

        if (parent == node) {
            parent = succ;
        } else {
            if (child != NULL)
                rb_set_parent(child, parent);
            parent->rb_left = child;
            succ->rb_right = node->rb_right;
            rb_set_parent(node->rb_right, succ);
        }

        succ->__rb_parent_color = node->__rb_parent_color;
        succ->rb_left = node->rb_left;
        rb_set_parent(node->rb_left, succ);
        rb_replace_child(node, succ, rb_parent(node), root);
    } else {
        child = (node->rb_left != NULL) ? node->rb_left : node->rb_right;
        parent = rb_parent(node);
        color = RB_COLOR(node);

        if (child != NULL)
            rb_set_parent(child, parent);
        rb_replace_child(node, child, parent, root);
    }

    if (color == RB_BLACK)
        rb_erase_fixup(child, parent, root);
}

struct rb_node *
rb_first(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;

    if (n == NULL)
        return (NULL);
    while (n->rb_left != NULL)
        n = n->rb_left;
    return (n);
}

struct rb_node *
rb_last(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;

    if (n == NULL)
        return (NULL);
    while (n->rb_right != NULL)
        n = n->rb_right;
    return (n);
}

struct rb_node *
rb_next(const struct rb_node *node)
{
    struct rb_node *parent;

    if (RB_EMPTY_NODE(node))
        return (NULL);

    if (node->rb_right != NULL) {
        node = node->rb_right;
        while (node->rb_left != NULL)
            node = node->rb_left;
        return ((struct rb_node *)node);
    }

    while ((parent = rb_parent(node)) != NULL && node == parent->rb_right)
        node = parent;
    return (parent);
}

struct rb_node *
rb_prev(const struct rb_node *node)
{
    struct rb_node *parent;

    if (RB_EMPTY_NODE(node))
        return (NULL);

    if (node->rb_left != NULL) {
        node = node->rb_left;
        while (node->rb_right != NULL)
            node = node->rb_right;
        return ((struct rb_node *)node);
    }

    while ((parent = rb_parent(node)) != NULL && node == parent->rb_left)
        node = parent;
    return (parent);
}
