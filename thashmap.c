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

#include <sys/param.h>

#if defined(_KERNEL)
#include <sys/systm.h>

#define	ASSERT(cond)			MPASS(cond)

#define	THM_POOL_LOCK(pool)		mtx_lock(&(pool)->tp_mtx)
#define	THM_POOL_UNLOCK(pool)		mtx_unlock(&(pool)->tp_mtx)

#else

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(THASHMAP_DEBUG)
#define	ASSERT(cond)			assert(cond)
#else
#define	ASSERT(cond)			((void)0)
#endif

#if !defined(__predict_false)
#define	__predict_false(cond)		(cond)
#endif

#if !defined(__unused)
#define	__unused
#endif

#define	THM_POOL_LOCK(pool)		((void)0)
#define	THM_POOL_UNLOCK(pool)		((void)0)

#endif /* !_KERNEL */

#include "thashmap.h"

/* 4096 on 64-bit archs, 2048 on 32-bit archs */
#define	THM_PAGE_SIZE			(128 * THM_SLOT_SIZE)
#define	THM_PAGE_PTRMASK		(~(uintptr_t)(THM_PAGE_SIZE - 1))
#define	THM_PAGE_STRUCT_SLOTS		\
	((1 << howmany(sizeof(struct thm_page), THM_SLOT_SIZE)) - 1)
#define	THM_PAGE_MAP1_EMPTY		(~(uint64_t)THM_PAGE_STRUCT_SLOTS)
#define	THM_PAGE_MAP2_EMPTY		(~(uint64_t)0ULL)

#define	THM_PTR_MASK_PAGE		(~(uintptr_t)(THM_PAGE_SIZE - 1))

#define	THM_SLOT_SIZE			\
	(THM_SLOT_MIN_ENTRIES * (int)sizeof(uintptr_t))
#define	THM_SLOT_MAX_ENTRIES		32
#define	THM_SLOT_MIN_ENTRIES		4

#define	THM_SUBKEY(k, n)		\
	(((k) >> (THM_SUBKEY_SHIFT * (5 - n))) & THM_SUBKEY_MASK)
#define	THM_SUBKEY_MASK			(THM_SLOT_MAX_ENTRIES - 1)
#define	THM_SUBKEY_BITIND(ind)		(((ind) - 2) / 5)
#define	THM_SUBKEY_SHIFT		5

#define	THM_KEY_BIT(ind)		(1ULL << (ind))

#define	THM_COUNT_1BITS_32(a)		__builtin_popcount((a))
#define	THM_COUNT_1BITS_64(a)		__builtin_popcountll((a))
#define	THM_COUNT_LEADING_0BITS_32(a)	__builtin_clz((a))
#define	THM_COUNT_LEADING_0BITS_64(a)	__builtin_clzll((a))
#define	THM_COUNT_TRAILING_0BITS_32(a)	__builtin_ctz((a))
#define	THM_COUNT_TRAILING_0BITS_64(a)	__builtin_ctzll((a))

#define	MASK_01010101			0x5555555555555555ULL
#define	MASK_01000100			0x4444444444444444ULL
#define	MASK_00110011			0x3333333333333333ULL
#define	MASK_11001100			0xccccccccccccccccULL
#define	MASK_00001111			0x0f0f0f0f0f0f0f0fULL
#define	MASK_11110000			0xf0f0f0f0f0f0f0f0ULL
#define	MASK_00010000			0x1010101010101010ULL
#define	MASK_00100000			0x2020202020202020ULL
#define	MASK_01000000			0x4040404040404040ULL


#if !defined(CTASSERT) && defined(_Static_assert)
#define	CTASSERT(cond)			\
	_Static_assert((cond), "compile-time assertion failed")
#elif !defined(CTASSERT)
#define	CTASSERT(x)			_CTASSERT(x, __LINE__)
#define	_CTASSERT(x, y)			__CTASSERT(x, y)
#define	__CTASSERT(x, y)		typedef char __assert ## y[(x) ? 1 : -1]
#endif

struct thm_page {
	uint64_t	tp_map1;
	uint64_t	tp_map2;
	uintptr_t	tp_q_next;
	uintptr_t	*tp_q_prevp;
};

struct thm_slot {
	uintptr_t	ts_map;
	uintptr_t	ts_entry[0];
};

struct thm_slotmax {
	uintptr_t	ts_entry[0];
};

static struct thm_slot *thm_slot_alloc(struct thm_pool *pool, u_int slen,
    void *hint);
static struct thm_slot *thm_slot_alloc_zero(struct thm_pool *pool, u_int slen,
    void *hint);
static __inline void thm_slot_free(struct thm_pool *pool, struct thm_slot *slot,
    u_int slen);
static int thm_slot_tryextend(struct thm_pool *pool, struct thm_slot *slot,
    u_int slen_old, u_int slen_new);
static void thm_slot_shrink(struct thm_pool *pool, struct thm_slot *slot,
    u_int slen_old, u_int slen_new);
static void thm_slotmax_fix_extend(struct thm_slot *slot_old,
    struct thm_slotmax *slotmax);

static __inline u_int thm_slot_get_slen(struct thm_slot *slot);

static struct thm_page *thm_page_alloc(struct thm_pool *pool);
static void thm_page_free(struct thm_pool *pool, struct thm_page *);

static __inline struct thm_page *thm_pool_first(struct thm_pool *pool,
    u_int rank);
static __inline struct thm_page *thm_pool_next(struct thm_page *page);
static __inline void thm_pool_insert_head(struct thm_pool *pool, u_int rank,
    struct thm_page *page);
static __inline void thm_pool_insert_tail(struct thm_pool *pool, u_int rank,
    struct thm_page *page);
static __inline void thm_pool_remove(struct thm_pool *pool, u_int rank,
    struct thm_page *page);

CTASSERT(THM_PAGE_STRUCT_SLOTS == 0x1 || THM_PAGE_STRUCT_SLOTS == 0x3);

void
thm_pool_init(struct thm_pool *pool, const char *name __unused)
{
	struct thm_pool_queue *qhead;
	u_int rank;

#if defined(_KERNEL)
	mtx_init(&pool->tp_mtx, "thm_pool", NULL, MTX_DEF);
	pool->tp_zone = uma_zcreate(__DECONST(char *, name), THM_PAGE_SIZE,
	    NULL, NULL, NULL, NULL, THM_PAGE_SIZE - 1, 0);
#endif

	for (rank = 0; rank < THM_POOL_RANK_MAX; rank++) {
		qhead = &pool->tp_queue[rank];
		qhead->tpq_first = 0;
		qhead->tpq_last = &qhead->tpq_first;
	}

	thm_pool_new_block(pool);
}

void
thm_pool_destroy(struct thm_pool *pool)
{
	struct thm_page *page;
	u_int rank;

	for (rank = 0; rank < THM_POOL_RANK_MAX; rank++) {
		while ((page = thm_pool_first(pool, rank)) != NULL) {
			thm_pool_remove(pool, rank, page);
			thm_page_free(pool, page);
		}
	}

#if defined(_KERNEL)
	mtx_destroy(&pool->tp_mtx);
	uma_zdestroy(pool->tp_zone);
#endif
}

void
thm_pool_new_block(struct thm_pool *pool)
{
	struct thm_page *page;

	page = thm_page_alloc(pool);
	THM_POOL_LOCK(pool);
	thm_pool_insert_head(pool, THM_POOL_RANK_MAX - 1, page);
	THM_POOL_UNLOCK(pool);
}

void
thm_head_init(struct thm_head *head, struct thm_pool *pool, int keyoffset)
{
	ASSERT((keyoffset & 0x3) == 0);

	head->th_pool = pool;
	head->th_keyoffset = keyoffset / sizeof(uint32_t);
	head->th_root = (uintptr_t)thm_slot_alloc_zero(pool, 1, NULL);
}

void
thm_head_destroy(struct thm_head *head)
{
	struct thm_slot *slot = thm_ptr_get_value(head->th_root);
	u_int slen;

	slen = thm_slot_get_slen(slot);
	thm_slot_free(head->th_pool, slot, slen);
}

static __inline struct thm_page *
thm_ptr_get_page(uintptr_t ptr)
{
	return ((struct thm_page *)(ptr & THM_PTR_MASK_PAGE));
}

static __inline uintptr_t
thm_ptr_get_pageflag(uintptr_t ptr)
{
	return (ptr & ~THM_PTR_MASK_PAGE);
}

static __inline void
thm_ptr_init_page(uintptr_t *ptr, void *addr, u_int flag)
{
	ASSERT((flag & THM_PTR_MASK_PAGE) == 0);

	*ptr = (((uintptr_t)addr) & THM_PTR_MASK_PAGE) |
	    thm_ptr_get_pageflag(flag);
}

static __inline void
thm_ptr_set_page(uintptr_t *ptr, void *addr)
{
	thm_ptr_init_page(ptr, addr, *ptr & ~THM_PTR_MASK_PAGE);
}

static __inline void
thm_ptr_set_slot(uintptr_t *ptr, struct thm_slot *slot)
{
	ASSERT((((uintptr_t)slot) & THM_PTR_MASK_RESERVED) == 0);

	*ptr = (*ptr & THM_PTR_MASK_SLEN) | THM_PTR_MASK_SLOT |
	    (uintptr_t)slot;
}

static __inline void
thm_ptr_set_value(uintptr_t *ptr, void *value)
{
	ASSERT((((uintptr_t)value) & THM_PTR_MASK_RESERVED) == 0);

	*ptr = (*ptr & THM_PTR_MASK_SLEN) | (uintptr_t)value;
}

static __inline struct thm_page *
thm_addr_get_page(void *addr)
{
	return (thm_ptr_get_page((uintptr_t)addr));
}

static __inline void
thm_bucket_set(uintptr_t *ptr, struct thm_entry *entry)
{
	/* Dont trash entry->te_next */
	thm_ptr_set_value(ptr, entry);
}

static __inline void
thm_bucket_insert(uintptr_t *ptr, struct thm_entry *entry)
{
	entry->te_next = thm_ptr_get_value(*ptr);
	thm_ptr_set_value(ptr, entry);
}

static __inline void
thm_bucket_remove(uintptr_t *ptr, struct thm_entry *entry)
{
	struct thm_entry *i;

	i = thm_ptr_get_value(*ptr);
	if (entry == i) {
		thm_ptr_set_value(ptr, entry->te_next);
		return;
	}

	while (i->te_next != entry)
		i = i->te_next;
	i->te_next = entry->te_next;
}

static __inline uint32_t
thm_entry_get_key(struct thm_head *head, struct thm_entry *entry)
{
	uint32_t *keyp;

	keyp = (uint32_t *)entry + head->th_keyoffset;

	return (*keyp & THM_KEY_MASK);
}

static __inline uintptr_t *
thm_slotmax_entry(struct thm_slot *slot, u_int ind)
{
	struct thm_slotmax *slotmax = (struct thm_slotmax *)slot;

	return (&slotmax->ts_entry[ind]);
}

static __inline u_int
thm_slot_get_slen(struct thm_slot *slot)
{
	u_int slen;

	slen = (((slot->ts_entry[0] & THM_PTR_MASK_SLEN) >> 1) |
	    (slot->ts_entry[1] & THM_PTR_MASK_SLEN) |
	    ((slot->ts_entry[2] & THM_PTR_MASK_SLEN) << 1)) + 1;

	return (slen);
}

static __inline void
thm_slot_set_slen(struct thm_slot *slot, u_int slen)
{
	slen -= 1;

	slot->ts_entry[0] = (slot->ts_entry[0] & ~THM_PTR_MASK_SLEN) |
	    ((slen << 1) & THM_PTR_MASK_SLEN);
	slot->ts_entry[1] = (slot->ts_entry[1] & ~THM_PTR_MASK_SLEN) |
	    (slen & THM_PTR_MASK_SLEN);
	slot->ts_entry[2] = (slot->ts_entry[2] & ~THM_PTR_MASK_SLEN) |
	    ((slen >> 1) & THM_PTR_MASK_SLEN);
}

static __inline void
thm_cursor_push(struct thm_cursor *cr, uintptr_t *entp)
{
	cr->tc_level++;
	ASSERT(cr->tc_level < THM_SUBKEY_MAX + 1);
	cr->tc_path[cr->tc_level] = entp;
}

int
thm_empty(struct thm_head *head)
{
	struct thm_slot *slot;
	uintptr_t *entp;

	slot = thm_ptr_get_value(head->th_root);

	if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
		for (u_int i = 0; i < THM_SLOT_MAX_ENTRIES; i++) {
			entp = thm_slotmax_entry(slot, i);
			if (thm_ptr_get_value(*entp) != NULL)
				return (0);
		}

		return (1);
	}

	return (slot->ts_map == 0);
}

static struct thm_bucket *
thm_first_impl(struct thm_cursor *cr)
{
	struct thm_slot *slot;
	void *entval;
	uintptr_t *entp;

	entval = thm_ptr_get_value(*cr->tc_path[cr->tc_level]);

	do {
		slot = entval;
		if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
			for (int i = 0; i < THM_SLOT_MAX_ENTRIES; i++) {
				entp = thm_slotmax_entry(slot, i);
				if (thm_ptr_get_value(*entp) != NULL)
					goto found;
			}
			return (NULL);
		}

		if (slot->ts_map == 0)
			return (NULL);
		entp = &slot->ts_entry[0];
found:
		thm_cursor_push(cr, entp);
		entval = thm_ptr_get_value(*entp);
	} while ((*entp & THM_PTR_MASK_SLOT) != 0);

	return (entval);
}

struct thm_bucket *
thm_first(struct thm_head *head, struct thm_cursor *cr)
{
	struct thm_cursor xcr;
	struct thm_bucket *bucket;

	if (cr == NULL)
		cr = &xcr;

	cr->tc_level = 0;
	cr->tc_path[0] = &head->th_root;

	bucket = thm_first_impl(cr);
	ASSERT(bucket != NULL || cr->tc_level == 0);

	return (bucket);
}

static struct thm_bucket *
thm_last_impl(struct thm_cursor *cr)
{
	struct thm_slot *slot;
	void *entval;
	uintptr_t *entp;
	int i;

	entval = thm_ptr_get_value(*cr->tc_path[cr->tc_level]);

	do {
		slot = entval;
		if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
			for (i = THM_SLOT_MAX_ENTRIES - 1; i >= 0; i--) {
				entp = thm_slotmax_entry(slot, i);
				if (thm_ptr_get_value(*entp) != NULL)
					goto found;
			}
			return (NULL);
		}

		if (slot->ts_map == 0)
			return (NULL);
		i = THM_COUNT_1BITS_32(slot->ts_map);
		entp = &slot->ts_entry[i - 1];
found:
		thm_cursor_push(cr, entp);
		entval = thm_ptr_get_value(*entp);
	} while ((*entp & THM_PTR_MASK_SLOT) != 0);

	return (entval);
}

struct thm_bucket *
thm_last(struct thm_head *head, struct thm_cursor *cr)
{
	struct thm_cursor xcr;
	struct thm_bucket *bucket;

	if (cr == NULL)
		cr = &xcr;

	cr->tc_level = 0;
	cr->tc_path[0] = &head->th_root;

	bucket = thm_last_impl(cr);
	ASSERT(bucket != NULL || cr->tc_level == 0);

	return (bucket);
}

static uintptr_t *
thm_next_step(struct thm_slot *slot, uintptr_t *entp)
{
	int count, i;

	if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
		i = entp - thm_slotmax_entry(slot, 0);
		ASSERT(i >= 0 && i < THM_SLOT_MAX_ENTRIES);
		for (i += 1; i < THM_SLOT_MAX_ENTRIES; i++) {
			entp = thm_slotmax_entry(slot, i);
			if (thm_ptr_get_value(*entp) != NULL)
				return (entp);
		}
		return (NULL);
	}

	count = THM_COUNT_1BITS_32(slot->ts_map);
	i = entp - slot->ts_entry;
	ASSERT(i >= 0 && i < count);
	if (i + 1 >= count)
		return (NULL);
	entp = &slot->ts_entry[i + 1];

	return (entp);
}

struct thm_bucket *
thm_next(struct thm_cursor *cr)
{
	struct thm_slot *slot;
	uintptr_t *entp;

	ASSERT(cr->tc_level > 0 && cr->tc_level < THM_SUBKEY_MAX + 1);

	for (; cr->tc_level > 0; cr->tc_level--) {
		ASSERT(*cr->tc_path[cr->tc_level - 1] & THM_PTR_MASK_SLOT ||
		    cr->tc_level == 1);
		slot = thm_ptr_get_value(*cr->tc_path[cr->tc_level - 1]);
		entp = thm_next_step(slot, cr->tc_path[cr->tc_level]);
		if (entp == NULL)
			continue;

		cr->tc_path[cr->tc_level] = entp;
		if ((*entp & THM_PTR_MASK_SLOT) == 0)
			return (thm_ptr_get_value(*entp));
		else
			return (thm_first_impl(cr));
	}

	return (NULL);
}

static uintptr_t *
thm_prev_step(struct thm_slot *slot, uintptr_t *entp)
{
	int i;

	if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
		i = entp - thm_slotmax_entry(slot, 0);
		ASSERT(i >= 0 && i < THM_SLOT_MAX_ENTRIES);
		for (i -= 1; i >= 0; i--) {
			entp = thm_slotmax_entry(slot, i);
			if (thm_ptr_get_value(*entp) != NULL)
				return (entp);
		}
		return (NULL);
	}

	i = entp - slot->ts_entry;
	ASSERT(i >= 0);
	if (i == 0)
		return (NULL);
	entp = &slot->ts_entry[i - 1];

	return (entp);
}

struct thm_bucket *
thm_prev(struct thm_cursor *cr)
{
	struct thm_slot *slot;
	uintptr_t *entp;

	ASSERT(cr->tc_level > 0 && cr->tc_level < THM_SUBKEY_MAX + 1);

	for (; cr->tc_level > 0; cr->tc_level--) {
		ASSERT(*cr->tc_path[cr->tc_level - 1] & THM_PTR_MASK_SLOT ||
		    cr->tc_level == 1);
		slot = thm_ptr_get_value(*cr->tc_path[cr->tc_level - 1]);
		entp = thm_prev_step(slot, cr->tc_path[cr->tc_level]);
		if (entp == NULL)
			continue;

		cr->tc_path[cr->tc_level] = entp;
		if ((*entp & THM_PTR_MASK_SLOT) == 0)
			return (thm_ptr_get_value(*entp));
		else
			return (thm_last_impl(cr));
	}

	return (NULL);
}

static uintptr_t *
thm_find_step(struct thm_slot *slot, u_int key)
{
	uintptr_t *entp;
	uint32_t keybit, smap;
	u_int keyind, slen;

	ASSERT(key < THM_SLOT_MAX_ENTRIES);

	slen = thm_slot_get_slen(slot);
	if (slen == THM_SLEN_MAX) {
		 entp = thm_slotmax_entry(slot, key);
		 if (thm_ptr_get_value(*entp) == NULL)
			 return (NULL);
		 return (entp);
	}

	smap = slot->ts_map;

	keybit = THM_KEY_BIT(key);
	if ((smap & keybit) == 0)
		return (NULL);

	smap &= keybit - 1;
	keyind = THM_COUNT_1BITS_32(smap);

	return (&slot->ts_entry[keyind]);
}

static struct thm_bucket *
thm_find_impl(struct thm_head *head, uint32_t key, struct thm_cursor *cr)
{
	uintptr_t *entp;
	void *entval;

	cr->tc_path[0] = &head->th_root;
	entval = thm_ptr_get_value(head->th_root);

	entp = thm_find_step(entval, THM_SUBKEY(key, 0));
	if (entp == NULL) {
		cr->tc_level = 0;
		return (NULL);
	}
	cr->tc_path[1] = entp;
	entval = thm_ptr_get_value(*entp);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		cr->tc_level = 1;
		goto found;
	}

	entp = thm_find_step(entval, THM_SUBKEY(key, 1));
	if (entp == NULL) {
		cr->tc_level = 1;
		return (NULL);
	}
	cr->tc_path[2] = entp;
	entval = thm_ptr_get_value(*entp);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		cr->tc_level = 2;
		goto found;
	}

	entp = thm_find_step(entval, THM_SUBKEY(key, 2));
	if (entp == NULL) {
		cr->tc_level = 2;
		return (NULL);
	}
	cr->tc_path[3] = entp;
	entval = thm_ptr_get_value(*entp);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		cr->tc_level = 3;
		goto found;
	}

	entp = thm_find_step(entval, THM_SUBKEY(key, 3));
	if (entp == NULL) {
		cr->tc_level = 3;
		return (NULL);
	}
	cr->tc_path[4] = entp;
	entval = thm_ptr_get_value(*entp);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		cr->tc_level = 4;
		goto found;
	}

	entp = thm_find_step(entval, THM_SUBKEY(key, 4));
	if (entp == NULL) {
		cr->tc_level = 4;
		return (NULL);
	}
	cr->tc_path[5] = entp;
	entval = thm_ptr_get_value(*entp);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		cr->tc_level = 5;
		goto found;
	}

	entp = thm_find_step(entval, THM_SUBKEY(key, 5));
	if (entp == NULL) {
		cr->tc_level = 5;
		return (NULL);
	}
	cr->tc_path[6] = entp;
	entval = thm_ptr_get_value(*entp);
	ASSERT((*entp & THM_PTR_MASK_SLOT) == 0);
	cr->tc_level = 6;

found:
	if (key != thm_entry_get_key(head, entval)) {
		cr->tc_level--;
		return (NULL);
	}

	return (entval);
}

struct thm_bucket *
thm_find(struct thm_head *head, uint32_t key, struct thm_cursor *cr)
{
	struct thm_cursor xcr;

	key &= THM_KEY_MASK;

	if (cr == NULL)
		cr = &xcr;

	return (thm_find_impl(head, key, cr));
}

struct thm_bucket *
thm_nfind(struct thm_head *head, uint32_t key, struct thm_cursor *cr)
{
	struct thm_cursor xcr;
	struct thm_slot *slot;
	uintptr_t *entp;
	void *entval;
	uint32_t keybit, smap;
	u_int subkey;
	int count, i;

	if (cr == NULL)
		cr = &xcr;

	key &= THM_KEY_MASK;

	entval = thm_find_impl(head, key, cr);
	if (entval != NULL)
		return (entval);

restart:
	subkey = THM_SUBKEY(key, cr->tc_level);
	slot = thm_ptr_get_value(*cr->tc_path[cr->tc_level]);

	if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
		entp = thm_slotmax_entry(slot, subkey);
		if (thm_ptr_get_value(*entp) != NULL)
			goto found_eq;
		for (i = subkey + 1; i < THM_SLOT_MAX_ENTRIES; i++) {
			entp = thm_slotmax_entry(slot, i);
			if (thm_ptr_get_value(*entp) != NULL)
				goto found_gt;
		}
	} else {
		smap = slot->ts_map;
		if (smap == 0) {
			ASSERT(cr->tc_level == 0);
			return (NULL);
		}
		count = THM_COUNT_1BITS_32(smap);
		keybit = THM_KEY_BIT(subkey);
		i = THM_COUNT_1BITS_32(smap & (keybit - 1));
		if (i < count) {
			entp = &slot->ts_entry[i];
			if ((smap & keybit) != 0)
				goto found_eq;
			else
				goto found_gt;
		}
	}

	/* back track */
	entval = thm_next(cr);
	goto done;

found_gt:
	if ((*entp & THM_PTR_MASK_SLOT) != 0) {
		thm_cursor_push(cr, entp);
		entval = thm_first_impl(cr);
	} else {
		entval = thm_ptr_get_value(*entp);
		ASSERT(thm_entry_get_key(head, entval) > key);
	}
	goto done;

found_eq:
	thm_cursor_push(cr, entp);
	if ((*entp & THM_PTR_MASK_SLOT) != 0)
		goto restart;
	else {
		entval = thm_ptr_get_value(*entp);
		if (thm_entry_get_key(head, entval) < key)
			entval = thm_next(cr);
	}

done:
	ASSERT(entval == NULL || thm_entry_get_key(head, entval) > key);
	return (entval);
}

static uintptr_t *
thm_insert_step(struct thm_pool *pool, uintptr_t *slotp, u_int key)
{
	struct thm_slot *slot, *oslot;
	uint32_t keyind, smap;
	u_int count, keybit, slen;

	ASSERT(key < THM_SLOT_MAX_ENTRIES);

	slot = thm_ptr_get_value(*slotp);
	slen = thm_slot_get_slen(slot);
	if (slen == THM_SLEN_MAX)
		return (thm_slotmax_entry(slot, key));

	smap = slot->ts_map;
	keybit = THM_KEY_BIT(key);
	keyind = THM_COUNT_1BITS_32(smap & (keybit - 1));

	if ((smap & keybit) != 0)
		return (&slot->ts_entry[keyind]);

	/* Insert new entry */
	count = THM_COUNT_1BITS_32(smap);
	ASSERT(count + 1 <= slen * THM_SLOT_MIN_ENTRIES);

	if (count + 1 + 1 > slen * THM_SLOT_MIN_ENTRIES &&
	    thm_slot_tryextend(pool, slot, slen, slen + 1)) {
		slen += 1;
		if (slen == THM_SLEN_MAX)
			return (thm_slotmax_entry(slot, key));
	}
	if (count + 1 + 1 <= slen * THM_SLOT_MIN_ENTRIES) {
		slot->ts_map |= keybit;
		for (u_int i = count; i > keyind; i--)
			slot->ts_entry[i] = slot->ts_entry[i - 1];
		slot->ts_entry[keyind] = 0;
		if (keyind < 3)
			thm_slot_set_slen(slot, slen);
		ASSERT((u_int)THM_COUNT_1BITS_32(slot->ts_map) + 1 <=
		    slen * THM_SLOT_MIN_ENTRIES);
		return (&slot->ts_entry[keyind]);
	}

	/* Allocate larger slot */
	oslot = slot;
	slot = thm_slot_alloc(pool, slen + 1, oslot);
	if (slot == NULL)
		return (NULL);

	thm_ptr_set_slot(slotp, slot);
	slen += 1;

	if (slen == THM_SLEN_MAX) {
		thm_slotmax_fix_extend(oslot, (struct thm_slotmax *)slot);
		thm_slot_free(pool, oslot, slen - 1);
		return (thm_slotmax_entry(slot, key));
	}

	slot->ts_map = oslot->ts_map | keybit;
	for (u_int i = 0; i < keyind; i++)
		slot->ts_entry[i] = oslot->ts_entry[i];
	slot->ts_entry[keyind] = 0;
	for (u_int i = keyind; i < count; i++)
		slot->ts_entry[i + 1] = oslot->ts_entry[i];
	thm_slot_set_slen(slot, slen);
	thm_slot_free(pool, oslot, slen - 1);

	ASSERT((u_int)THM_COUNT_1BITS_32(slot->ts_map) + 1 <=
	    slen * THM_SLOT_MIN_ENTRIES);

	return (&slot->ts_entry[keyind]);
}

static uintptr_t *
thm_insert_mkslot(struct thm_pool *pool, uintptr_t *slotp_top, u_int subkey_n,
    struct thm_entry *entry1, u_int key1,
    struct thm_entry *entry2, u_int key2)
{
	struct thm_slot *slot;
	uintptr_t *ent1, *ent2, *slotp;
	u_int nslots, subkey1, subkey2;

	slotp = slotp_top;

	nslots = THM_COUNT_LEADING_0BITS_32(key1 ^ key2);
	nslots = THM_SUBKEY_BITIND(nslots) - subkey_n + 1;
	ASSERT(nslots >= 1);

	slot = thm_slot_alloc(pool, nslots, slotp);
	if (slot == NULL)
		return (NULL);
	memset(slot, 0, nslots * THM_SLOT_SIZE);

nested:
	subkey1 = THM_SUBKEY(key1, subkey_n);
	subkey2 = THM_SUBKEY(key2, subkey_n);
	if (subkey1 == subkey2) {
		slot->ts_map = (1 << subkey1);
		thm_ptr_set_slot(slotp, slot);
		slotp = &slot->ts_entry[0];
		slot = (struct thm_slot *)((uintptr_t *)slot +
		    THM_SLOT_MIN_ENTRIES);
		subkey_n++;
		goto nested;
	}

	slot->ts_map = (1 << subkey1) | (1 << subkey2);
	thm_ptr_set_slot(slotp, slot);
	if (key1 < key2) {
		ent1 = &slot->ts_entry[0];
		ent2 = &slot->ts_entry[1];
	} else {
		ent1 = &slot->ts_entry[1];
		ent2 = &slot->ts_entry[0];
	}
	thm_bucket_insert(ent1, entry1);
	thm_bucket_set(ent2, entry2);

	return (ent1);
}

struct thm_bucket *
thm_insert(struct thm_head *head, struct thm_entry *entry)
{
	struct thm_pool *pool;
	struct thm_entry *xentry;
	uintptr_t *parentp, *entp;
	uint32_t key, xkey;
	u_int subkey_n;

	pool = head->th_pool;
	key = thm_entry_get_key(head, entry);

	parentp = &head->th_root;
	entp = thm_insert_step(pool, parentp, THM_SUBKEY(key, 0));
	if (entp == NULL)
		return (NULL);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		subkey_n = 0;
		goto found;
	}

	parentp = entp;
	entp = thm_insert_step(pool, parentp, THM_SUBKEY(key, 1));
	if (entp == NULL)
		return (NULL);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		subkey_n = 1;
		goto found;
	}

	parentp = entp;
	entp = thm_insert_step(pool, parentp, THM_SUBKEY(key, 2));
	if (entp == NULL)
		return (NULL);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		subkey_n = 2;
		goto found;
	}

	parentp = entp;
	entp = thm_insert_step(pool, parentp, THM_SUBKEY(key, 3));
	if (entp == NULL)
		return (NULL);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		subkey_n = 3;
		goto found;
	}

	parentp = entp;
	entp = thm_insert_step(pool, parentp, THM_SUBKEY(key, 4));
	if (entp == NULL)
		return (NULL);
	if ((*entp & THM_PTR_MASK_SLOT) == 0) {
		subkey_n = 4;
		goto found;
	}

	parentp = entp;
	entp = thm_insert_step(pool, parentp, THM_SUBKEY(key, 5));
	if (entp == NULL)
		return (NULL);
	ASSERT((*entp & THM_PTR_MASK_SLOT) == 0);
	subkey_n = 5;

found:
	if ((xentry = thm_ptr_get_value(*entp)) != NULL &&
	    (xkey = thm_entry_get_key(head, xentry)) != key) {
		entp = thm_insert_mkslot(pool, entp, subkey_n + 1,
		    entry, key, xentry, xkey);
		if (entp == NULL)
			return (NULL);
		return (thm_ptr_get_value(*entp));
	}

	thm_bucket_insert(entp, entry);

	return (thm_ptr_get_value(*entp));
}

static int
thm_remove_step(struct thm_pool *pool, struct thm_slot *slot, uintptr_t *entp,
    u_int key)
{
	uint32_t keybit;
	u_int count, keyind, slen;

	slen = thm_slot_get_slen(slot);
	if (slen == THM_SLEN_MAX) {
		struct thm_slotmax *slotmax = (struct thm_slotmax *)slot;

		ASSERT(entp >= slotmax->ts_entry &&
		    entp < slotmax->ts_entry + THM_SLOT_MAX_ENTRIES);

		thm_ptr_set_value(entp, NULL);
		for (count = 0, entp = slotmax->ts_entry;
		    entp < slotmax->ts_entry + THM_SLOT_MAX_ENTRIES; entp++) {
			if (thm_ptr_get_value(*entp) != NULL)
				count++;
		}
	} else {
		keyind = entp - slot->ts_entry;
		keybit = THM_KEY_BIT(key);

		ASSERT(entp >= slot->ts_entry &&
		    keyind < slen * THM_SLOT_MIN_ENTRIES);
		ASSERT((slot->ts_map & keybit) != 0);

		slot->ts_map &= ~keybit;
		count = THM_COUNT_1BITS_32(slot->ts_map);

		for (u_int i = keyind; i < count; i++)
			slot->ts_entry[i] = slot->ts_entry[i + 1];
		if (keyind < 3)
			thm_slot_set_slen(slot, slen);
	}

	if (count == 0)
		return (1);

	if (count + 1 + 2 <= (slen - 1) * THM_SLOT_MIN_ENTRIES &&
	    (THM_SLOT_MIN_ENTRIES > 4 && slen > 1)) {
		ASSERT(slen > 1);
		thm_slot_shrink(pool, slot, slen, slen - 1);
	}

	return (0);
}

void
thm_remove(struct thm_head *head, struct thm_entry *entry)
{
	struct thm_cursor cr;
	uintptr_t *entp;
	void *entval;
	uint32_t key;
	int subkey_n;

	key = thm_entry_get_key(head, entry);

	entval = thm_find_impl(head, key, &cr);
	ASSERT(entval != NULL);

	subkey_n = cr.tc_level - 1;
	entp = cr.tc_path[cr.tc_level];

	thm_bucket_remove(entp, entry);

	if (thm_ptr_get_value(*entp) != NULL)
		return;

	/* Remove slot */
	for (; subkey_n >= 0; subkey_n--) {
		/* slot for entp */
		entval = thm_ptr_get_value(*cr.tc_path[subkey_n]);
		if (thm_remove_step(head->th_pool, entval, entp,
		    THM_SUBKEY(key, subkey_n)) == 0)
			break;
		if (subkey_n > 0) {
			thm_slot_free(head->th_pool, entval,
			    thm_slot_get_slen(entval));
		}
		entp = cr.tc_path[subkey_n];
	}
}

static __inline u_int
thm_page_get_rank(struct thm_page *page)
{
	u_int rank;

	rank = page->tp_q_next & ~THM_PTR_MASK_PAGE;
	ASSERT(rank < THM_POOL_RANK_MAX);

	return (rank);
}

static void
thm_pagemap_count_fragments(uint64_t map, u_int *frags, u_int maxfrag)
{
	u_int bit, count, left;

	map = ~map;
	left = 64;

	do {
		bit = map & 1;
		if (bit) {
			if (~map == 0LL)
				count = left;
			else
				count = THM_COUNT_TRAILING_0BITS_64(~map);
		} else {
			if (map == 0LL)
				count = left;
			else
				count = THM_COUNT_TRAILING_0BITS_64(map);
		}
		ASSERT(count > 0);
		map >>= count;
		left -= count;
		if (bit != 0)
			continue;
		if (count > maxfrag)
			frags[maxfrag - 1] += count / maxfrag;
		else
			frags[count - 1] += 1;

	} while (left > 0);
}

static struct thm_slot *
thm_page_alloc_slot(struct thm_page *page, u_int slen)
{
	uint64_t m1, m2, mt, mask;
	u_int off, ioff;

	ioff = 1;
	m1 = page->tp_map1;
	m2 = page->tp_map2;

again:
	switch (slen) {
	case 8:
		m1 = m1 & (m1 >> 1) & MASK_01010101;
		m1 = (m1 & MASK_00110011) &
		    ((m1 & MASK_11001100) >> 2);
		m1 = (m1 & MASK_00001111) &
		    ((m1 & MASK_11110000) >> 4);
		m2 = m2 & (m2 >> 1) & MASK_01010101;
		m2 = (m2 & MASK_00110011) &
		    ((m2 & MASK_11001100) >> 2);
		m2 = (m2 & MASK_00001111) &
		    ((m2 & MASK_11110000) >> 4);
		break;
	case 7:
		mt = (m1 & MASK_01000000) >> 6;
		m1 = m1 & (m1 >> 1) & MASK_01010101;
		mt = mt & ((m1 & MASK_00010000) >> 4);
		m1 = mt & (m1 & MASK_00110011) &
		    ((m1 & MASK_11001100) >> 2);
		mt = (m2 & MASK_01000000) >> 6;
		m2 = m2 & (m2 >> 1) & MASK_01010101;
		mt = mt & ((m2 & MASK_00010000) >> 4);
		m2 = mt & (m2 & MASK_00110011) &
		    ((m2 & MASK_11001100) >> 2);
		break;
	case 6:
		m1 = m1 & (m1 >> 1) & MASK_01010101;
		mt = (m1 & MASK_00010000) >> 4;
		m1 = mt & (m1 & MASK_00110011) &
		    ((m1 & MASK_11001100) >> 2);
		m2 = m2 & (m2 >> 1) & MASK_01010101;
		mt = (m2 & MASK_00010000) >> 4;
		m2 = mt & (m2 & MASK_00110011) &
		    ((m2 & MASK_11001100) >> 2);
		break;
	case 5:
		mt = ((m1 & MASK_00010000) &
		    ((m1 & MASK_00100000) >> 1)) >> 4;
		m1 = m1 & (m1 >> 1) & MASK_01010101;
		m1 = mt & (m1 & MASK_00110011) &
		    ((m1 & MASK_11001100) >> 2);
		mt = ((m2 & MASK_00010000) &
		    ((m2 & MASK_00100000) >> 1)) >> 4;
		m2 = m2 & (m2 >> 1) & MASK_01010101;
		m2 = mt & (m2 & MASK_00110011) &
		    ((m2 & MASK_11001100) >> 2);
		break;
	case 4:
		m1 = m1 & (m1 >> 1) & MASK_01010101;
		m1 = (m1 & MASK_00110011) &
		    ((m1 & MASK_11001100) >> 2);
		m2 = m2 & (m2 >> 1) & MASK_01010101;
		m2 = (m2 & MASK_00110011) &
		    ((m2 & MASK_11001100) >> 2);
		break;
	case 3:
		mt = (m1 & MASK_01000100) >> 2;
		m1 = mt & m1 & (m1 >> 1) & MASK_01010101;
		mt = (m2 & MASK_01000100) >> 2;
		m2 = mt & m2 & (m2 >> 1) & MASK_01010101;
		break;
	case 2:
		m1 = m1 & (m1 >> 1) & MASK_01010101;
		m2 = m2 & (m2 >> 1) & MASK_01010101;
		break;
	case 1:
		break;
	}

	mask = (1ULL << slen) - 1;
	if (m1 != 0) {
		off = THM_COUNT_TRAILING_0BITS_64(m1) + ioff - 1;
		mask <<= off;
		ASSERT((mask & page->tp_map1) == mask);
		page->tp_map1 &= ~mask;
		return ((struct thm_slot *)((uintptr_t *)page +
		    (off * THM_SLOT_MIN_ENTRIES)));
	} else if (m2 != 0) {
		off = THM_COUNT_TRAILING_0BITS_64(m2) + ioff - 1;
		mask <<= off;
		ASSERT((mask & page->tp_map2) == mask);
		page->tp_map2 &= ~mask;
		return ((struct thm_slot *)((uintptr_t *)page +
		    ((off + 64) * THM_SLOT_MIN_ENTRIES)));
	}

	if (ioff * 2 < slen) {
		m1 = page->tp_map1 >> ioff;
		m2 = page->tp_map2 >> ioff;
		ioff++;
		goto again;
	}

	return (NULL);
}

static __inline u_int
thm_page_promote_rank(struct thm_page *page, u_int rank)
{
	uint64_t m1, m2;
	u_int count;

	ASSERT(rank < THM_POOL_RANK_MAX);

	count = THM_COUNT_1BITS_64(page->tp_map1) +
	    THM_COUNT_1BITS_64(page->tp_map2);

	if (count <= rank * 3)
		return (0);

	m1 = page->tp_map1;
	m2 = page->tp_map2;
	m1 = m1 & (m1 >> 1) & MASK_01010101;
	m2 = m2 & (m2 >> 1) & MASK_01010101;
	if (rank >= 4) {
		m1 = (m1 & MASK_00110011) &
		    ((m1 & MASK_11001100) >> 2);
		m2 = (m2 & MASK_00110011) &
		    ((m2 & MASK_11001100) >> 2);
		if (rank >= 8) {
			m1 = (m1 & MASK_00001111) &
			    ((m1 & MASK_11110000) >> 4);
			m2 = (m2 & MASK_00001111) &
			    ((m2 & MASK_11110000) >> 4);
		}
	}

	if (m1 != 0 || m2 != 0)
		return (rank);

	return (0);
}

static void
thm_page_promote(struct thm_pool *pool, struct thm_page *page)
{
	u_int rank, rank_new;

	rank = thm_page_get_rank(page);

	if (rank == THM_POOL_RANK_MAX - 1) {
		if (page->tp_map1 == THM_PAGE_MAP1_EMPTY &&
		    page->tp_map2 == THM_PAGE_MAP2_EMPTY) {
			thm_pool_remove(pool, rank, page);
			THM_POOL_UNLOCK(pool);
			thm_page_free(pool, page);
		} else
			THM_POOL_UNLOCK(pool);
		return;
	}

	rank_new = thm_page_promote_rank(page, rank + 1);
	if (rank_new > rank) {
		thm_pool_remove(pool, rank, page);
		thm_pool_insert_tail(pool, rank_new, page);
	}

	THM_POOL_UNLOCK(pool);
}

static __inline u_int
thm_page_demote_rank(struct thm_page *page, u_int rank)
{
	u_int frags[THM_SLEN_MAX] = { 0 };
	u_int acc;

	if (rank == 0)
		return (0);

	thm_pagemap_count_fragments(page->tp_map1, frags, rank);
	thm_pagemap_count_fragments(page->tp_map2, frags, rank);

	for (acc = 0; rank > 0; rank--) {
		if (frags[rank - 1] + acc < 2) {
			acc += frags[rank - 1];
			continue;
		}
		return (rank);
	}

	return (acc);
}

static void
thm_page_demote(struct thm_pool *pool, struct thm_page *page,
    u_int rank, u_int slen)
{
	u_int rank_new;

	rank_new = thm_page_demote_rank(page, slen - 1);

	if (rank_new != rank) {
		thm_pool_remove(pool, rank, page);
		thm_pool_insert_head(pool, rank_new, page);
	}
}

static __inline u_int
thm_slot_get_offset(struct thm_slot *slot)
{
	u_int off;

	off = ((uintptr_t)slot) & ~THM_PTR_MASK_PAGE;
	return (off / THM_SLOT_SIZE);
}

static void
thm_slotmax_fix_shrink(struct thm_slotmax *slotmax, struct thm_slot *slot_new,
    u_int slen_new)
{
	uintptr_t xbuf[THM_SLOT_MAX_ENTRIES], *buf;
	uintptr_t map = 0, keybit = 1;
	u_int i, keyind;

	for (i = 0, keyind = 0; i < THM_SLOT_MAX_ENTRIES; i++) {
		if (thm_ptr_get_value(slotmax->ts_entry[i]) != NULL)
			map |= keybit;
		keybit <<= 1;
	}

	if (slot_new != (void *)slotmax)
		buf = slot_new->ts_entry;
	else
		buf = xbuf;
	for (i = 0, keyind = 0; i < THM_SLOT_MAX_ENTRIES; i++) {
		if (thm_ptr_get_value(slotmax->ts_entry[i]) == NULL)
			continue;
		buf[keyind] = slotmax->ts_entry[i];
		keyind++;
	}
	ASSERT(keyind > 0 && keyind < slen_new * THM_SLOT_MIN_ENTRIES);
	if (slot_new == (void *)slotmax)
		memcpy(slot_new->ts_entry, buf, keyind * sizeof(uintptr_t));

	slot_new->ts_map = map;
	thm_slot_set_slen(slot_new, slen_new);
}

static void
thm_slotmax_fix_extend(struct thm_slot *slot_old, struct thm_slotmax *slotmax)
{
	uintptr_t xbuf[THM_SLOT_MAX_ENTRIES], *buf;
	uint32_t smap, keybit;
	u_int i, keyind;

	smap = slot_old->ts_map;
	keyind = 0;

	ASSERT(smap != 0);
	ASSERT((u_int)THM_COUNT_1BITS_32(smap) + 1 <=
	    thm_slot_get_slen(slot_old) * THM_SLOT_MIN_ENTRIES);

	if (slot_old != (void *)slotmax)
		buf = slotmax->ts_entry;
	else
		buf = xbuf;

	memset(buf, 0, THM_SLOT_MAX_ENTRIES * sizeof(uintptr_t));
	while (smap != 0) {
		i = THM_COUNT_TRAILING_0BITS_32(smap);
		keybit = THM_KEY_BIT(i);
		smap &= ~keybit;
		buf[i] = slot_old->ts_entry[keyind];
		keyind++;
	}

	if (slot_old == (void *)slotmax)
		memcpy(slotmax->ts_entry, buf,
		    THM_SLOT_MAX_ENTRIES * sizeof(uintptr_t));

	thm_slot_set_slen((struct thm_slot *)slotmax, THM_SLEN_MAX);
}

static void
thm_slot_shrink(struct thm_pool *pool, struct thm_slot *slot, u_int slen_old,
    u_int slen_new)
{
	struct thm_page *page;
	uint64_t mask, *map;
	u_int off, maskoff;

	ASSERT(slen_old > slen_new);

	if (slen_new != 0) {
		if (slen_old == THM_SLEN_MAX)
			thm_slotmax_fix_shrink((struct thm_slotmax *)slot, slot,
			    slen_new);
		else
			thm_slot_set_slen(slot, slen_new);
	}

	page = thm_addr_get_page(slot);
	off = thm_slot_get_offset(slot);
	if (off < 64) {
		maskoff = off;
		map = &page->tp_map1;
	} else {
		maskoff = off - 64;
		map = &page->tp_map2;
	}

	/* 0x00XXXNNN */
	mask = ((1LL << slen_old) - 1) - ((1LL << slen_new) - 1);
	mask <<= maskoff;

	THM_POOL_LOCK(pool);

	ASSERT((*map & mask) == 0);
	*map |= mask;

	/* Unlocks pool */
	thm_page_promote(pool, page);
}

static int
thm_slot_tryextend(struct thm_pool *pool __unused, struct thm_slot *slot,
    u_int slen_old, u_int slen_new)
{
	struct thm_page *page;
	uint64_t mask, *map;
	u_int off, maskoff;

	page = thm_addr_get_page(slot);
	off = thm_slot_get_offset(slot);

	/* Forbid crossing map boundary */
	if (((off ^ (off + slen_new)) & (64 | 128)) != 0)
		return (0);

	if (off < 64) {
		maskoff = off;
		map = &page->tp_map1;
	} else {
		maskoff = off - 64;
		map = &page->tp_map2;
	}

	/* 0x00NNNPPP */
	mask = ((1LL << slen_new) - 1) - ((1LL << slen_old) - 1);
	mask <<= maskoff;

	THM_POOL_LOCK(pool);
	if ((*map & mask) == mask) {
		*map &= ~mask;
		THM_POOL_UNLOCK(pool);
		if (slen_new == THM_SLEN_MAX)
			thm_slotmax_fix_extend(slot,
			    (struct thm_slotmax *)slot);
		else
			thm_slot_set_slen(slot, slen_new);
		return (1);
	} else
		THM_POOL_UNLOCK(pool);

	return (0);
}

static __inline struct thm_slot *
thm_slot_alloc_step(struct thm_pool *pool, struct thm_page *page, u_int slen)
{
	struct thm_slot *slot;
	u_int rank;

	rank = thm_page_get_rank(page);
	if (rank < slen)
		return (NULL);
	slot = thm_page_alloc_slot(page, slen);
	if (slot != NULL)
		return (slot);

	thm_page_demote(pool, page, rank, slen);

	return (NULL);
}

static struct thm_slot *
thm_slot_alloc(struct thm_pool *pool, u_int slen, void *hint)
{
	struct thm_page *page, *npage;
	struct thm_slot *slot = NULL;
	u_int rank;

	ASSERT(slen <= THM_SLEN_MAX);

	THM_POOL_LOCK(pool);

	if (hint != NULL) {
		page = thm_addr_get_page(hint);
		slot = thm_slot_alloc_step(pool, page, slen);
		if (slot != NULL)
			goto out;
	}

	for (rank = slen; rank < THM_POOL_RANK_MAX; rank++) {
		page = thm_pool_first(pool, rank);
		while (page != NULL) {
			npage = thm_pool_next(page);
			slot = thm_slot_alloc_step(pool, page, slen);
			if (slot != NULL)
				goto out;
			page = npage;
		}
	}

out:
	THM_POOL_UNLOCK(pool);

	return (slot);
}

static struct thm_slot *
thm_slot_alloc_zero(struct thm_pool *pool, u_int slen, void *hint)
{
	struct thm_slot *slot;

	slot = thm_slot_alloc(pool, slen, hint);
	if (slot == NULL)
		return (NULL);

	memset(slot, 0, slen * THM_SLOT_SIZE);
	thm_slot_set_slen(slot, slen);

	return (slot);
}

static __inline void
thm_slot_free(struct thm_pool *pool, struct thm_slot *slot, u_int slen)
{
	thm_slot_shrink(pool, slot, slen, 0);
}

/* TAILQ_* equivalents */

static __inline struct thm_page *
thm_pool_first(struct thm_pool *pool, u_int rank)
{
	return (thm_ptr_get_page(pool->tp_queue[rank].tpq_first));
}

static __inline struct thm_page *
thm_pool_next(struct thm_page *page)
{
	return (thm_ptr_get_page(page->tp_q_next));
}

static __inline void
thm_pool_insert_head(struct thm_pool *pool, u_int rank,
    struct thm_page *page)
{
	struct thm_pool_queue *qhead;
	struct thm_page *first;

	qhead = &pool->tp_queue[rank];
	first = thm_ptr_get_page(qhead->tpq_first);

	ASSERT(rank < THM_POOL_RANK_MAX);
	ASSERT(first == NULL || first->tp_q_prevp == &qhead->tpq_first);

	thm_ptr_init_page(&page->tp_q_next, first, rank);
	if (first != NULL)
		first->tp_q_prevp = &page->tp_q_next;
	else
		qhead->tpq_last = &page->tp_q_next;
	qhead->tpq_first = (uintptr_t)page;
	page->tp_q_prevp = &qhead->tpq_first;
}

static __inline void
thm_pool_insert_tail(struct thm_pool *pool, u_int rank,
    struct thm_page *page)
{
	struct thm_pool_queue *qhead;

	qhead = &pool->tp_queue[rank];

	ASSERT(rank < THM_POOL_RANK_MAX);
	ASSERT(qhead->tpq_last != NULL);

	thm_ptr_init_page(&page->tp_q_next, NULL, rank);
	page->tp_q_prevp = qhead->tpq_last;
	thm_ptr_set_page(qhead->tpq_last, page);
	qhead->tpq_last = &page->tp_q_next;
}

static __inline void
thm_pool_remove(struct thm_pool *pool, u_int rank, struct thm_page *page)
{
	struct thm_pool_queue *qhead;
	struct thm_page *next;

	qhead = &pool->tp_queue[rank];
	next = thm_ptr_get_page(page->tp_q_next);

	ASSERT(rank == thm_page_get_rank(page));
	ASSERT(rank < THM_POOL_RANK_MAX);
	ASSERT(next == NULL || next->tp_q_prevp == &page->tp_q_next);
	ASSERT(thm_ptr_get_page(*page->tp_q_prevp) == page);

#if defined(THASHMAP_DEBUG)
	void **oldnext = (void *)&page->tp_q_next;
	void **oldprev = (void *)&page->tp_q_prevp;
#endif

	if (next != NULL)
		next->tp_q_prevp = page->tp_q_prevp;
	else
		qhead->tpq_last = page->tp_q_prevp;

	thm_ptr_set_page(page->tp_q_prevp, next);

#if defined(THASHMAP_DEBUG)
	*oldnext = (void *)-1;
	*oldprev = (void *)-1;
#endif
}

static struct thm_page *
thm_page_alloc(struct thm_pool *pool __unused)
{
	struct thm_page *page;

#if defined(_KERNEL)
	page = uma_zalloc(pool->tp_zone, M_WAITOK);
#else
	if (posix_memalign((void *)&page, THM_PAGE_SIZE, THM_PAGE_SIZE) != 0) {
		abort();
		return (NULL);
	}
#endif

	ASSERT(page == thm_addr_get_page(page));

	page->tp_map1 = THM_PAGE_MAP1_EMPTY;
	page->tp_map2 = THM_PAGE_MAP2_EMPTY;

	return (page);
}

static void
thm_page_free(struct thm_pool *pool __unused, struct thm_page *page)
{
	ASSERT(page->tp_map1 == THM_PAGE_MAP1_EMPTY);
	ASSERT(page->tp_map2 == THM_PAGE_MAP2_EMPTY);

#if defined(_KERNEL)
	uma_zfree(pool->tp_zone, page);
#else
	free(page);
#endif
}

void
thm_pool_get_stats(struct thm_pool *pool, struct thm_pool_stats *stats)
{
	u_int frags[THM_SLEN_MAX] = { 0 };
	struct thm_page *page;
	u_long used_total = 0, used;
	u_int rank;

	if (stats != NULL)
		memset(stats, 0, sizeof(*stats));

	for (rank = 0; rank < THM_POOL_RANK_MAX; rank++) {
		for (page = thm_pool_first(pool, rank); page != NULL;
		    page = thm_pool_next(page)) {
			used = THM_COUNT_1BITS_64(~page->tp_map1) +
			    THM_COUNT_1BITS_64(~page->tp_map2);
			used_total += used;

			if (stats != NULL) {
				stats->tp_pages += 1;
				stats->tp_slots += 128;
				stats->tp_queues[rank] += 1;
			}

			thm_pagemap_count_fragments(page->tp_map1, frags,
			    THM_SLEN_MAX);
			thm_pagemap_count_fragments(page->tp_map2, frags,
			    THM_SLEN_MAX);
		}
	}

	if (stats != NULL) {
		stats->tp_slots_free = stats->tp_slots - used_total;
		for (u_int i = 0; i < THM_SLEN_MAX; i++)
			stats->tp_fragments[i] = frags[i];
	}
}

#if !defined(_KERNEL)

static void
thm_dump_tree_step(struct thm_head *head, struct thm_slot *slot)
{
	uintptr_t buf[THM_SLOT_MAX_ENTRIES], *ents;

	if (thm_slot_get_slen(slot) == THM_SLEN_MAX) {
		ents = ((struct thm_slotmax *)slot)->ts_entry;
	} else {
		uint32_t keybit, smap;
		u_int keyind;

		ents = buf;
		memset(buf, 0, THM_SLOT_MAX_ENTRIES * sizeof(uintptr_t));
		smap = slot->ts_map;
		keyind = 0;
		while (smap != 0) {
			u_int i = THM_COUNT_TRAILING_0BITS_32(smap);
			keybit = THM_KEY_BIT(i);
			smap &= ~keybit;
			ents[i] = slot->ts_entry[keyind];
			keyind++;
		}
	}

	printf("S:%p:%d: ", slot,
	    thm_slot_get_slen(slot) * THM_SLOT_MIN_ENTRIES);
	for (int i = 0; i < THM_SLOT_MAX_ENTRIES; i++) {
		void *entval = thm_ptr_get_value(ents[i]);
		if (entval == NULL)
			continue;
		if ((ents[i] & THM_PTR_MASK_SLOT) == 0)
		printf("%d:D:%p:K%08x ", i, entval,
		    thm_entry_get_key(head, entval));
		else
		printf("%d:S:%p ", i, entval);
	}
	printf("\n");

	for (int i = 0; i < THM_SLOT_MAX_ENTRIES; i++) {
		void *entval = thm_ptr_get_value(ents[i]);
		if (entval == NULL || (ents[i] & THM_PTR_MASK_SLOT) == 0)
			continue;
		thm_dump_tree_step(head, entval);
	}
}

void
thm_dump_tree(struct thm_head *head)
{
	thm_dump_tree_step(head, thm_ptr_get_value(head->th_root));
}

#endif /* !_KERNEL */
