/*
 * This file is part of dsp.
 *
 * Copyright (c) 2026 Michael Barbour <barbour.michael.0@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#ifndef DSP_LIST_UTIL_H
#define DSP_LIST_UTIL_H

/*
 * Macros for manipulating doubly-linked lists. All macros expect pointers.
 *
 * Example node struct:
 *     struct node {
 *         struct node *prev, *next;
 *         ...
 *     };
 *
 * Example list struct:
 *     struct list {
 *         struct node *head, *tail;
 *         ...
 *     };
*/

#include <stddef.h>

#define LIST_FOREACH(list, node) for ( \
	typeof((list)->head) node = (list)->head; \
	(node); \
	node = (node)->next \
)

#define LIST_PREPEND(list, node) do { \
	(node)->prev = NULL; \
	if ((list)->head) { \
		(node)->next = (list)->head; \
		(node)->next->prev = (node); \
		(list)->head = (node); \
	} \
	else { \
		(node)->next = NULL; \
		(list)->head = (list)->tail = (node); \
	} \
} while(0)

#define LIST_APPEND(list, node) do { \
	(node)->next = NULL; \
	if ((list)->tail) { \
		(node)->prev = (list)->tail; \
		(node)->prev->next = (node); \
		(list)->tail = (node); \
	} \
	else { \
		(node)->prev = NULL; \
		(list)->head = (list)->tail = (node); \
	} \
} while(0)

#define LIST_INSERT(list, node, after) do { \
	if (!(after) || (after) == (list)->tail) \
		LIST_APPEND(list, node); \
	else { \
		(node)->prev = (after); \
		(node)->next = (after)->next; \
		(node)->next->prev = (node); \
		(after)->next = (node); \
	} \
} while(0)

#define LIST_CONCAT(dest, src) do { \
	if ((dest)->head) { \
		(dest)->tail->next = (src)->head; \
		(src)->head->prev = (dest)->tail; \
	} \
	else (dest)->head = (src)->head; \
	(dest)->tail = (src)->tail; \
	(src)->head = (src)->tail = NULL; \
} while(0)

#define LIST_REMOVE(list, node) do { \
	if ((node) == (list)->head) (list)->head = (node)->next; \
	else (node)->prev->next = (node)->next; \
	if ((node) == (list)->tail) (list)->tail = (node)->prev; \
	else (node)->next->prev = (node)->prev; \
} while(0)

#endif
