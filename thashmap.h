/*-
 * Copyright (c) 2012 Gleb Kurtsou
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _THASHMAP_H
#define	_THASHMAP_H

#if defined(_KERNEL)
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/mutex.h>
#include <vm/uma.h>
#endif

#define	THM_PTR_MASK_VALUE		(~THM_PTR_MASK_RESERVED)
#define	THM_PTR_MASK_RESERVED		((uintptr_t)0x03)
#define	THM_PTR_MASK_SLEN		((uintptr_t)0x02)
#define	THM_PTR_MASK_SLOT		((uintptr_t)0x01)

#define	THM_KEY_MASK			((uint32_t)0x3fffffff)

#define	THM_SLEN_MAX			8

#define	THM_SUBKEY_MAX			6

#define	THM_POOL_RANK_MAX		(THM_SLEN_MAX + 1)

struct thm_bucket;
struct thm_page;

struct thm_entry {
	struct thm_entry *te_next;
};

struct thm_cursor {
	uintptr_t	*tc_path[THM_SUBKEY_MAX + 1];
	u_int		tc_level;
};

struct thm_pool_queue {
	uintptr_t	tpq_first;
	uintptr_t	*tpq_last;
};

struct thm_pool {
#ifdef _KERNEL
	struct mtx	tp_mtx;
	uma_zone_t	tp_zone;
#endif
	struct thm_pool_queue tp_queue[THM_POOL_RANK_MAX];
};

struct thm_head {
	struct thm_pool *th_pool;
	uintptr_t	th_root;
	int		th_keyoffset;
};

struct thm_pool_stats {
	u_long		tp_pages;
	u_long		tp_slots;
	u_long		tp_slots_free;
	u_long		tp_queues[THM_POOL_RANK_MAX];
	u_long		tp_fragments[THM_SLEN_MAX];
};

void thm_pool_init(struct thm_pool *pool, const char *name);

void thm_pool_destroy(struct thm_pool *pool);

void thm_pool_new_block(struct thm_pool *pool);

void thm_pool_get_stats(struct thm_pool *pool, struct thm_pool_stats *stats);

void thm_head_init(struct thm_head *head, struct thm_pool *pool, int keyoffset);

void thm_head_destroy(struct thm_head *head);

int thm_empty(struct thm_head *head);

struct thm_bucket *thm_first(struct thm_head *head, struct thm_cursor *curs);

struct thm_bucket *thm_last(struct thm_head *head, struct thm_cursor *curs);

struct thm_bucket *thm_next(struct thm_cursor *curs);

struct thm_bucket *thm_prev(struct thm_cursor *curs);

struct thm_bucket *thm_find(struct thm_head *head, uint32_t key,
    struct thm_cursor *cr);

struct thm_bucket *thm_nfind(struct thm_head *head, uint32_t key,
    struct thm_cursor *cr);

struct thm_bucket *thm_insert(struct thm_head *head, struct thm_entry *entry);

void thm_remove(struct thm_head *head, struct thm_entry *entry);

void thm_dump_tree(struct thm_head *head);

static __inline void *
thm_ptr_get_value(uintptr_t ptr)
{
	return ((void *)(ptr & THM_PTR_MASK_VALUE));
}

static __inline struct thm_entry *
thm_bucket_first(struct thm_bucket *bucket)
{
	return ((struct thm_entry *)bucket);
}

static __inline struct thm_entry *
thm_bucket_next(struct thm_entry *entry)
{
	return (entry->te_next);
}

#define	THM_DEFINE(name, type, entryfield, keyfield)			\
									\
struct name##_BUCKET;							\
									\
struct name##_HEAD {							\
	struct thm_head name##_head;					\
};									\
									\
static __inline int							\
name##_KEYOFFSET0(struct thm_entry *entryptr, uint32_t *keyptr)		\
{									\
	return ((intptr_t)keyptr - (intptr_t)entryptr);			\
}									\
									\
static __inline int							\
name##_KEYOFFSET(void)							\
{									\
	struct type *ent = NULL;					\
	/* Force type check, emulate offsetof() */			\
	return (name##_KEYOFFSET0(&ent->entryfield, &ent->keyfield));	\
}									\
									\
static __inline struct thm_bucket *					\
name##_BUCKET_CAST(struct name##_BUCKET *bucket)			\
{									\
	return ((struct thm_bucket *)bucket);				\
}									\
									\
static __inline struct type *						\
name##_ENTRY(struct thm_entry *ent)					\
{									\
	if (ent == NULL)						\
		return (NULL);						\
	return ((struct type *)(void *)((char *)(ent) -			\
	    offsetof(struct type, entryfield)));			\
}									\
									\
static __inline struct thm_entry *					\
name##_FIELD(struct type *elm)						\
{									\
	return (&(elm->entryfield));					\
}

#define	THM_HEAD(name)			struct name##_HEAD

#define	THM_BUCKET(name)		struct name##_BUCKET

#define	THM_HEAD_INIT(name, head, pool)					\
	thm_head_init(&(head)->name##_head, (pool), name##_KEYOFFSET())

#define	THM_HEAD_DESTROY(name, head)					\
	thm_head_destroy(&(head)->name##_head)

#define	THM_EMPTY(name, head)						\
	thm_empty(&(head)->name##_head)

#define	THM_FIRST(name, head, cursor)					\
	((struct name##_BUCKET *)thm_first(&(head)->name##_head, (cursor)))

#define	THM_LAST(name, head, cursor)					\
	((struct name##_BUCKET *)thm_last(&(head)->name##_head, (cursor)))

#define	THM_NEXT(name, cursor)						\
	((struct name##_BUCKET *)thm_next((cursor)))

#define	THM_PREV(name, cursor)						\
	((struct name##_BUCKET *)thm_prev((cursor)))

#define	THM_FIND(name, head, key, cursor)				\
	((struct name##_BUCKET *)thm_find(&(head)->name##_head, (key),	\
	    (cursor)))

#define	THM_NFIND(name, head, key, cursor)				\
	((struct name##_BUCKET *)thm_nfind(&(head)->name##_head, (key),	\
	    (cursor)))

#define	THM_INSERT(name, head, entry)					\
	((struct name##_BUCKET *)thm_insert(&(head)->name##_head,	\
	    name##_FIELD((entry))))

#define	THM_REMOVE(name, head, entry)					\
	thm_remove(&(head)->name##_head, name##_FIELD((entry)))

#define	THM_FOREACH(name, var, head, cursor)				\
	for ((var) = THM_LAST(name, head, (cursor));			\
	     (var) != NULL;						\
	     (var) = THM_PREV(name, (cursor)))

#define	THM_BUCKET_FIRST(name, bucket)					\
	name##_ENTRY(thm_bucket_first(name##_BUCKET_CAST((bucket))))

#define	THM_BUCKET_NEXT(name, entry)					\
	name##_ENTRY(thm_bucket_next(name##_FIELD((entry))))

#define	THM_BUCKET_FOREACH(name, var, bucket)				\
	for ((var) = THM_BUCKET_FIRST(name, bucket);			\
	     (var) != NULL;						\
	     (var) = THM_BUCKET_NEXT(name, (var)))

#define	THM_BUCKET_FOREACH_SAFE(name, var, bucket, tvar)		\
	for ((var) = THM_BUCKET_FIRST(name, bucket);			\
	     (var) != NULL && ((tvar) = THM_BUCKET_NEXT(name, (var)), 1); \
	     (var) = (tvar))

#endif
