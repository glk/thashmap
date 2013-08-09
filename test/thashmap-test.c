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

#include <sys/types.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "thashmap.h"

#if !defined(__unused)
#define __unused
#endif

static const int test_opt_verbose = 0;
static const int test_opt_fragmentation = 0;
static const int test_opt_random = 1;

struct s1 {
	char		pad1[5];
	uint32_t	key;
	struct thm_entry entry;
};

struct s2 {
	char		pad1[5];
	struct thm_entry entry2;
	struct thm_entry entry1;
	uint32_t	key1;
	uint32_t	key2;
};

typedef void test_method_t(int *, int);

THM_DEFINE(s1_map, s1, entry, key);
THM_DEFINE(s2_map1, s2, entry1, key1);
THM_DEFINE(s2_map2, s2, entry2, key2);

static void
test_pool_stats(const char *msg, struct thm_pool *pool)
{
	struct thm_pool_stats ps;
	int i;

	thm_pool_get_stats(pool, &ps);
	printf("%s: %lu pages, %lu slots (%lu free)",
	    msg, ps.tp_pages, ps.tp_slots, ps.tp_slots_free);
	printf("\n    queues:    ");
	for (i = 0; i < THM_POOL_RANK_MAX; i++)
		printf("[%d]:%-4lu ", i, ps.tp_queues[i]);
	printf("\n    fragments: ");
	for (i = 0; i < THM_SLEN_MAX; i++)
		printf("[%d]:%-4lu ", i + 1, ps.tp_fragments[i]);
	printf("\n");
}

static int
key_cmp(const void *xa, const void *xb)
{
	const int *a = xa, *b = xb;

	return (((*a) & THM_KEY_MASK) - ((*b) & THM_KEY_MASK));
}

static const int test_gen_key_seed = 33554467UL;

static __inline int
test_gen_next_key(int cur)
{
	const uint32_t prime = 0x01000193UL;
        const u_int8_t *s = (const u_int8_t *)&cur;

	cur *= prime;
	cur ^= *s++;
	cur *= prime;
	cur ^= *s++;
	cur *= prime;
	cur ^= *s++;
	cur *= prime;
	cur ^= *s++;
        return cur;
}

static __inline int
test_gen_random_key(void)
{
#if defined(__FreeBSD__)
	return (arc4random());
#else
	return (random());
#endif
}

static int *
test_gen_random_keys(int n)
{
	int i, *keys;

	keys = malloc(n * sizeof(int));
	for (i = 0; i < n; i++) {
		keys[i] = test_gen_random_key();
	}

	return keys;
}

static void
test_remove_dup_keys(int *keys, int n)
{
	int i, j, restart, *keys_sorted;

	keys_sorted = malloc(n * sizeof(int));
	do {
		restart = 0;
		memcpy(keys_sorted, keys, n * sizeof(int));
		qsort(keys_sorted, n, sizeof(int), key_cmp);
		for (i = 0; i < n - 1; i++) {
			if (key_cmp(&keys_sorted[i], &keys_sorted[i + 1]) != 0)
				continue;
			while (i < n - 1 &&
			    key_cmp(&keys_sorted[i + 1], &keys_sorted[i]) == 0)
				i++;
			restart = 1;
			for (j = 0; j < n;  j++) {
				if (key_cmp(&keys[j], &keys_sorted[i]) != 0)
					continue;
				keys[j] = test_gen_random_key();
			}
		}
	} while (restart != 0);
	free(keys_sorted);
}

__unused static void
test_2(int key1, int key2)
{
	struct thm_pool pool;
	struct thm_cursor cr;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 e1, e2;

	thm_pool_init(&pool, "thashmap-test");
	THM_HEAD_INIT(s1_map, &head, &pool);

	e1.key = key1;
	e2.key = key2;

	bucket = THM_INSERT(s1_map, &head, &e1);
	assert(bucket != NULL);
	assert(THM_BUCKET_FIRST(s1_map, bucket) == &e1);

	bucket = THM_FIND(s1_map, &head, e1.key, NULL);
	assert(THM_BUCKET_FIRST(s1_map, bucket) == &e1);

	bucket = THM_INSERT(s1_map, &head, &e2);
	assert(bucket != NULL);
	assert(THM_BUCKET_FIRST(s1_map, bucket) == &e2);

	bucket = THM_FIND(s1_map, &head, e1.key, NULL);
	assert(THM_BUCKET_FIRST(s1_map, bucket) == &e1);

	bucket = THM_FIND(s1_map, &head, e2.key, NULL);
	assert(THM_BUCKET_FIRST(s1_map, bucket) == &e2);

	if (test_opt_verbose >= 2) {
		printf("before remove 1\n");
		thm_dump_tree(&head.s1_map_head);
	}

	bucket = THM_FIRST(s1_map, &head, &cr);
	assert(bucket != NULL);
	bucket = THM_NEXT(s1_map, &cr);
	assert(bucket != NULL);
	bucket = THM_NEXT(s1_map, &cr);
	assert(bucket == NULL);

	THM_REMOVE(s1_map, &head, &e1);
	bucket = THM_FIND(s1_map, &head, e1.key, NULL);
	assert(bucket == NULL);
	bucket = THM_FIND(s1_map, &head, e2.key, NULL);
	assert(THM_BUCKET_FIRST(s1_map, bucket) == &e2);

	if (test_opt_verbose >= 2) {
		printf("before remove 2\n");
		thm_dump_tree(&head.s1_map_head);
	}

	bucket = THM_FIRST(s1_map, &head, &cr);
	assert(bucket != NULL);
	bucket = THM_NEXT(s1_map, &cr);
	assert(bucket == NULL);

	THM_REMOVE(s1_map, &head, &e2);
	bucket = THM_FIND(s1_map, &head, e1.key, NULL);
	assert(bucket == NULL);
	bucket = THM_FIND(s1_map, &head, e2.key, NULL);
	assert(bucket == NULL);

	if (test_opt_verbose >= 2) {
		printf("empty\n");
		thm_dump_tree(&head.s1_map_head);
	}

	assert(THM_EMPTY(s1_map, &head));

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);
}

__unused static void
test_dup(int n)
{
	struct thm_pool pool;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 *elist, *ep;
	int i, key = 1;

	thm_pool_init(&pool, "thashmap-test");
	THM_HEAD_INIT(s1_map, &head, &pool);

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = key;
		bucket = THM_INSERT(s1_map, &head, ep);
		assert(bucket != NULL);
	}

	assert(elist[0].entry.te_next == NULL);

	for (i = 0; i < n; i += 3) {
		ep = &elist[i];
		THM_REMOVE(s1_map, &head, ep);
	}

	for (i = 1; i < n; i += 3) {
		ep = &elist[i];
		THM_REMOVE(s1_map, &head, ep);
	}

	for (i = 2; i < n; i += 3) {
		ep = &elist[i];
		THM_REMOVE(s1_map, &head, ep);
	}

	assert(THM_EMPTY(s1_map, &head));

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);
}

__unused static void
test_2heads(int key1, int key2)
{
	struct thm_pool pool;

	THM_HEAD(s2_map1) head1;
	THM_BUCKET(s2_map1) *b1;

	THM_HEAD(s2_map2) head2;
	THM_BUCKET(s2_map2) *b2;

	struct s2 e1, e2, *ep;

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s2_map1, &head1, &pool);
	THM_HEAD_INIT(s2_map2, &head2, &pool);

	e1.key1 = e2.key1 = key1;
	e1.key2 = e2.key2 = key2;

	// e1

	b1 = THM_INSERT(s2_map1, &head1, &e1);
	assert(b1 != NULL);
	ep = THM_BUCKET_FIRST(s2_map1, b1);
	assert(ep == &e1);

	b1 = THM_FIND(s2_map1, &head1, key1, NULL);
	ep = THM_BUCKET_FIRST(s2_map1, b1);
	assert(ep == &e1);

	b2 = THM_INSERT(s2_map2, &head2, &e1);
	assert(b2 != NULL);
	ep = THM_BUCKET_FIRST(s2_map2, b2);
	assert(ep == &e1);

	b2 = THM_FIND(s2_map2, &head2, key2, NULL);
	ep = THM_BUCKET_FIRST(s2_map2, b2);
	assert(ep == &e1);

	// e2

	b1 = THM_INSERT(s2_map1, &head1, &e2);
	assert(b1 != NULL);
	ep = THM_BUCKET_FIRST(s2_map1, b1);
	assert(ep == &e2);

	b1 = THM_FIND(s2_map1, &head1, key1, NULL);
	ep = THM_BUCKET_FIRST(s2_map1, b1);
	assert(ep == &e2);

	b2 = THM_INSERT(s2_map2, &head2, &e2);
	assert(b2 != NULL);
	ep = THM_BUCKET_FIRST(s2_map2, b2);
	assert(ep == &e2);

	b2 = THM_FIND(s2_map2, &head2, key2, NULL);
	ep = THM_BUCKET_FIRST(s2_map2, b2);
	assert(ep == &e2);

	// find

	b1 = THM_FIND(s2_map1, &head1, key1, NULL);
	ep = THM_BUCKET_FIRST(s2_map1, b1);
	assert(ep == &e1 || ep == &e2);

	b2 = THM_FIND(s2_map2, &head2, key2, NULL);
	ep = THM_BUCKET_FIRST(s2_map2, b2);
	assert(ep == &e1 || ep == &e2);
}

__unused static void
test_insert_remove(int *keys, int n)
{
	struct thm_pool pool;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 *ep, *elist;
	int i;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = keys[i];
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (test_opt_verbose >= 2) {
			printf("insert %d/%d", i, n);
			test_pool_stats("", &pool);
		}
		if (bucket == NULL) {
			if (test_opt_verbose >= 1)
				printf("alloc new page: %d/%d\n", i, n);
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	if (test_opt_verbose >= 1) {
		printf("insert %d:", n);
		test_pool_stats("", &pool);
	}

	for (i = 0; i < n; i += 2) {
		ep = &elist[i];
		if (test_opt_verbose >= 2)
			printf("remove %d/%d: entry=%p\n", i, n, &ep->entry);
		THM_REMOVE(s1_map, &head, ep);
	}
	if (test_opt_verbose >= 1) {
		printf("remove %d/%d\n", n/2, n);
		test_pool_stats("", &pool);
	}
	for (i = 1; i < n; i+=2) {
		ep = &elist[i];
		if (test_opt_verbose >= 2)
			printf("remove %d/%d: entry=%p\n", i, n, &ep->entry);
		THM_REMOVE(s1_map, &head, ep);
	}
	if (test_opt_verbose >= 1) {
		printf("remove %d:", n);
		test_pool_stats("", &pool);
	}

	assert(THM_EMPTY(s1_map, &head));

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(elist);
}

__unused static void
test_insert_remove_chunk(int *keys, int n)
{
	int chunk_size;
	int nchunks;
	struct thm_pool pool;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;
	int key;

	struct s1 *ep, *xep;
	int i, j;

	if (n > 1000)
		chunk_size = 100;
	else
		chunk_size = 10;

	n -= n % chunk_size;
	nchunks = n / chunk_size;

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (j = 0; j < nchunks; j++) {
		for (i = 0; i < chunk_size; i++) {
			ep = malloc(sizeof(*ep));
			ep->key = keys[j*chunk_size + i];
			while ((bucket = THM_INSERT(s1_map, &head, ep)) == NULL)
				thm_pool_new_block(&pool);
			bucket = THM_FIND(s1_map, &head, ep->key, NULL);
			assert(bucket != NULL);
			THM_BUCKET_FOREACH(s1_map, xep, bucket) {
				if (xep == ep)
					break;
			}
			assert(xep == ep);
		}
		for (i = 0; i < chunk_size; i += 2) {
			key = keys[j*chunk_size + i];
			bucket = THM_FIND(s1_map, &head, key, NULL);
			assert(bucket != NULL);
			ep = THM_BUCKET_FIRST(s1_map, bucket);
			assert(ep != NULL);
			xep = THM_BUCKET_NEXT(s1_map, ep);
			if (xep != NULL) {
				/* Randomly remove first or second element */
				if (key & 1) {
					ep = xep;
					xep = THM_BUCKET_FIRST(s1_map, bucket);
				}
			}

			THM_REMOVE(s1_map, &head, ep);
			bucket = THM_FIND(s1_map, &head, key, NULL);
			assert(THM_BUCKET_FIRST(s1_map, bucket) == xep);
			free(ep);
		}
		for (i = 1; i < j * chunk_size; i += 2) {
			key = keys[i];
			bucket = THM_FIND(s1_map, &head, key, NULL);
			assert(bucket != NULL);
		}
	}

	while ((bucket = THM_FIRST(s1_map, &head, NULL)) != NULL) {
		THM_BUCKET_FOREACH_SAFE(s1_map, ep, bucket, xep) {
			THM_REMOVE(s1_map, &head, ep);
			free(ep);
		}
	}

	assert(THM_EMPTY(s1_map, &head));

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);
}

__unused static void
test_random(test_method_t *method, int n)
{
	int *keys;

	keys = test_gen_random_keys(n);
	method(keys, n);

	free(keys);
}

__unused static void
test_nfind(int *keys, int n)
{
	struct thm_pool pool;
	struct thm_cursor cursor;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 *ep, *elist;
	int i, *skeys;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	skeys = malloc(n * sizeof(int));
	memcpy(skeys, keys, n * sizeof(int));
	test_remove_dup_keys(skeys, n);
	keys = malloc(n * sizeof(int));
	memcpy(keys, skeys, n * sizeof(int));
	qsort(skeys, n, sizeof(int), key_cmp);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = keys[i];
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (bucket == NULL) {
			if (test_opt_verbose >= 1)
				printf("alloc new page: %d/%d\n", i, n);
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	if (test_opt_verbose >= 2) {
		printf("nfind %d\n", n);
		thm_dump_tree(&head.s1_map_head);
	}

	for (i = 0; i < n; i++) {
		int tmp = skeys[i] + 1;
		if (key_cmp(&tmp, &skeys[i]) >= 0) {
			if (test_opt_verbose >= 2)
				printf("nfind %d/%d: skip %d\n", i, n,
				    skeys[i - 1]);
			continue;
		}

		bucket = THM_NFIND(s1_map, &head, tmp, &cursor);
		assert(bucket != NULL);
		ep = THM_BUCKET_FIRST(s1_map, bucket);
		assert(ep != NULL);
		assert(key_cmp(&ep->key, &skeys[i + 1]) == 0);

		for (int j = 1; j < 10 && i + j < n; j++) {
			bucket = THM_NEXT(s1_map, &cursor);
			ep = THM_BUCKET_FIRST(s1_map, bucket);
			assert(key_cmp(&ep->key, &skeys[i + j]) == 0);
		}
	}

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		THM_REMOVE(s1_map, &head, ep);
	}

	assert(THM_EMPTY(s1_map, &head));

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(keys);
	free(skeys);
	free(elist);
}

__unused static void
test_first(int *keys, int n)
{
	struct thm_pool pool;
	struct thm_cursor cursor, xcursor;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket, *xbucket;

	struct s1 *ep, *xep, *elist;
	int i, *skeys;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = keys[i];
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (bucket == NULL) {
			if (test_opt_verbose >= 1)
				printf("alloc new page: %d/%d\n", i, n);
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	if (test_opt_verbose >= 2) {
		printf("first %d\n", n);
		thm_dump_tree(&head.s1_map_head);
	}


	skeys = malloc(n * sizeof(int));
	memcpy(skeys, keys, n * sizeof(int));
	qsort(skeys, n, sizeof(int), key_cmp);

	for (i = 0; i < n; i++) {
		bucket = THM_FIRST(s1_map, &head, &cursor);
		assert(bucket != NULL);
		ep = THM_BUCKET_FIRST(s1_map, bucket);
		assert(key_cmp(&ep->key, &skeys[i]) == 0);

		xbucket = THM_FIND(s1_map, &head, ep->key, &xcursor);
		assert(xbucket == bucket);
		assert(cursor.tc_level = xcursor.tc_level);
		assert(cursor.tc_path[cursor.tc_level] ==
		    xcursor.tc_path[xcursor.tc_level]);

		xbucket = THM_NEXT(s1_map, &cursor);
		xep = THM_BUCKET_FIRST(s1_map, xbucket);
		if (i + 1 == n)
			assert(xbucket == NULL);
		else if (key_cmp(&ep->key, &skeys[i + 1]) != 0)
			assert(key_cmp(&xep->key, &skeys[i + 1]) == 0);

		THM_REMOVE(s1_map, &head, ep);
	}

	assert(THM_EMPTY(s1_map, &head));
	assert(THM_FIRST(s1_map, &head, NULL) == NULL);
	assert(THM_LAST(s1_map, &head, NULL) == NULL);

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(skeys);
	free(elist);
}

__unused static void
test_last(int *keys, int n)
{
	struct thm_pool pool;
	struct thm_cursor cursor, xcursor;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket, *xbucket;

	struct s1 *ep, *xep, *elist;
	int i, *skeys;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = keys[i];
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (bucket == NULL) {
			if (test_opt_verbose >= 1)
				printf("alloc new page: %d/%d\n", i, n);
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	skeys = malloc(n * sizeof(int));
	memcpy(skeys, keys, n * sizeof(int));
	qsort(skeys, n, sizeof(int), key_cmp);

	for (i = n - 1; i >= 0; i--) {
		bucket = THM_LAST(s1_map, &head, &cursor);
		assert(bucket != NULL);
		ep = THM_BUCKET_FIRST(s1_map, bucket);
		assert(key_cmp(&ep->key, &skeys[i]) == 0);

		xbucket = THM_FIND(s1_map, &head, ep->key, &xcursor);
		assert(xbucket == bucket);
		assert(cursor.tc_level = xcursor.tc_level);
		assert(cursor.tc_path[cursor.tc_level] ==
		    xcursor.tc_path[xcursor.tc_level]);

		xbucket = THM_PREV(s1_map, &cursor);
		xep = THM_BUCKET_FIRST(s1_map, xbucket);
		if (i == 0)
			assert(xbucket == NULL);
		else if (key_cmp(&ep->key, &skeys[i - 1]) != 0)
			assert(key_cmp(&xep->key, &skeys[i - 1]) == 0);

		THM_REMOVE(s1_map, &head, ep);
	}

	assert(THM_EMPTY(s1_map, &head));
	assert(THM_FIRST(s1_map, &head, NULL) == NULL);
	assert(THM_LAST(s1_map, &head, NULL) == NULL);

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(skeys);
	free(elist);
}

__unused static void
test_next(int *keys, int n)
{
	struct thm_pool pool;
	struct thm_cursor cursor;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 *ep, *xep, *elist;
	int i, *skeys;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = keys[i];
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (bucket == NULL) {
			if (test_opt_verbose >= 1)
				printf("alloc new page: %d/%d\n", i, n);
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	if (test_opt_verbose >= 2) {
		printf("next %d\n", n);
		thm_dump_tree(&head.s1_map_head);
	}

	skeys = malloc(n * sizeof(int));
	memcpy(skeys, keys, n * sizeof(int));
	qsort(skeys, n, sizeof(int), key_cmp);

	i = 0;
	for (bucket = THM_FIRST(s1_map, &head, &cursor); bucket != NULL;
	    bucket = THM_NEXT(s1_map, &cursor)) {
		ep = THM_BUCKET_FIRST(s1_map, bucket);
		assert(key_cmp(&ep->key, &skeys[i]) == 0);
		for (; i < n; i++) {
			if (key_cmp(&ep->key, &skeys[i]) != 0)
				break;
		}
	}

	while ((bucket = THM_FIRST(s1_map, &head, NULL)) != NULL) {
		THM_BUCKET_FOREACH_SAFE(s1_map, ep, bucket, xep) {
			THM_REMOVE(s1_map, &head, ep);
		}
	}

	assert(THM_EMPTY(s1_map, &head));
	assert(THM_FIRST(s1_map, &head, NULL) == NULL);
	assert(THM_LAST(s1_map, &head, NULL) == NULL);

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(skeys);
	free(elist);
}

__unused static void
test_prev(int *keys, int n)
{
	struct thm_pool pool;
	struct thm_cursor cursor;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 *ep, *xep, *elist;
	int i, *skeys;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		ep->key = keys[i];
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (bucket == NULL) {
			if (test_opt_verbose >= 1)
				printf("alloc new page: %d/%d\n", i, n);
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	if (test_opt_verbose >= 2) {
		printf("next %d\n", n);
		thm_dump_tree(&head.s1_map_head);
	}

	skeys = malloc(n * sizeof(int));
	memcpy(skeys, keys, n * sizeof(int));
	qsort(skeys, n, sizeof(int), key_cmp);

	i = n - 1;
	for (bucket = THM_LAST(s1_map, &head, &cursor); bucket != NULL;
	    bucket = THM_PREV(s1_map, &cursor)) {
		ep = THM_BUCKET_FIRST(s1_map, bucket);
		assert(key_cmp(&ep->key, &skeys[i]) == 0);
		for (; i >= 0; i--) {
			if (key_cmp(&ep->key, &skeys[i]) != 0)
				break;
		}
	}

	while ((bucket = THM_LAST(s1_map, &head, NULL)) != NULL) {
		THM_BUCKET_FOREACH_SAFE(s1_map, ep, bucket, xep) {
			THM_REMOVE(s1_map, &head, ep);
		}
	}

	assert(THM_EMPTY(s1_map, &head));
	assert(THM_FIRST(s1_map, &head, NULL) == NULL);
	assert(THM_LAST(s1_map, &head, NULL) == NULL);

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(skeys);
	free(elist);
}

static void
test_pool_fragmentation(int n, int chunk, int seedkey)
{
	struct thm_pool pool;
	THM_HEAD(s1_map) head;
	THM_BUCKET(s1_map) *bucket;

	struct s1 *ep, *elist;
	int i;

	elist = malloc(sizeof(struct s1) * n);

	thm_pool_init(&pool, "thashmap-test");

	THM_HEAD_INIT(s1_map, &head, &pool);

	int curkey = seedkey;
	for (i = 0; i < n; i++) {
		ep = &elist[i];
		curkey = test_gen_next_key(curkey + i);
		ep->key = curkey & THM_KEY_MASK;
again:
		bucket = THM_INSERT(s1_map, &head, ep);
		if (bucket == NULL) {
			if (test_opt_verbose >= 1) {
				printf("alloc new page: %d/%d", i, n);
				test_pool_stats("", &pool);
			}
			thm_pool_new_block(&pool);
			goto again;
		}
	}

	test_pool_stats("full", &pool);

	for (i = 0; i < n; i++) {
		ep = &elist[i];
		THM_REMOVE(s1_map, &head, ep);
		if (i != 0 && (i % chunk == 0 || i + 1 == n)) {
			char buf[32];
			snprintf(buf, sizeof(buf), "%d-%d removed",
			    i - chunk + 1, i + 1);
			test_pool_stats(buf, &pool);
		}
	}

	assert(THM_EMPTY(s1_map, &head));

	THM_HEAD_DESTROY(s1_map, &head);

	thm_pool_destroy(&pool);

	free(elist);
}

/* {{{ test data */

static int test_data_1[] __unused = {
	0x8788881a, 0x75836a21, 0x7be16b2c, 0x959c0574,
	0x7c747d09, 0x9f904ddc, 0x56279d36, 0x5ecef872,
	0x64b5d381, 0xbaf7ada8, 0xc1be62c4, 0x51728f3c,
	0xf3f0d848, 0xdee51a82, 0x61b58a9b, 0xf6e10bc2,
	0x792f1b6f, 0x779a0e17, 0xcb6dd9a9, 0xa19cd62d,
	0xa0d08534, 0x37da03c1, 0x23cd8f77, 0xfe601356,
	0x45e77e95, 0x53f5bd18, 0x96947c2d, 0xb9d1d71e,
	0xc7356984, 0x5129dd05, 0x37297bf3, 0xa1a6653d,
	0xe2cb55b1, 0xe7b8af51, 0x60c87e79, 0x3bf80cd9,
	0xeed136d6, 0xade5e85e, 0x5251d58d, 0x0157c012,
	0xd8da265d, 0xf1875753, 0x39f86e57, 0x1ea74343,
	0x52f18780, 0x9f605326, 0xffde6e97, 0x5fbbd50a,
	0x55996eea, 0x1bc39c80, 0x371d6056, 0x89597f08,
	0x0ae8af5a, 0xc5469ee9, 0x0a26c689, 0x7269466e,
	0xba990510, 0x0d7e4345, 0xabc7d6e2, 0x7556cb71,
	0x739e73db, 0x323ca5ef, 0x195ee524, 0xf4d81e78,
	0x56458c21, 0xd9abca4a, 0x231c63d0, 0x4687ddff,
	0x432f9a9d, 0xe53ffe04, 0xc22b5a1c, 0x3a74d84a,
	0x2e64b55a, 0x849516da, 0xe94a6dba, 0xbd8a0f87,
	0x86352395, 0xf9914338, 0xf2eee350, 0x06357306,
	0xff42aad8, 0x63b4ac3c, 0x5aec401e, 0x51cfde95,
	0x379733bb, 0x977ecc11, 0x6c0c2978, 0x6b22d80d,
	0xb376beec, 0x0ef517e4, 0x9a203973, 0xbdacdfc7,
	0x74206c40, 0x1bf040eb, 0x2bc30f9c, 0x49463402,
	0x1b38aec8, 0x20fa1a56, 0x8e22163b, 0x75479096
};

static int test_data_2[] __unused = {
	0x9eef8ac2, 0xc80fb15c, 0x5262ae08, 0xd143df15,
	0x68d2ebe2, 0x8db00e78, 0xf271588b, 0xbf0d541e,
	0x2f8ea246, 0x5dc91c66, 0xed16118a, 0xf08c1890,
	0xfedf71dd, 0xa5408f7b, 0xce6bb2d3, 0x8ce4d630,
	0xf8e60ccf, 0xb5265418, 0x501264f2, 0xc4ec1143,
	0x473a7b6b, 0x1bb38d9c, 0xf5763a09, 0x9a72dfea,
	0x77ef3e58, 0xae1d5d56, 0x44eee44c, 0x4b2ed9e8,
	0x448b692e, 0x8b96e28d, 0xb3a66fa3, 0x604e9ff3,
	0xababd9c6, 0x8a7f038f, 0x6cbdcf8d, 0x5c854eb1,
	0xc265a183, 0x3108228f, 0x34bf7dd3, 0x8cb19265,
	0x54d51c5d, 0x90941dfb, 0x30483497, 0x69ec9ff6,
	0x8032078d, 0x584cbeb3, 0xf917d17d, 0xab6e7d73,
	0x4232bc5f, 0xfe625ec8, 0xc4914187, 0xe7ac289c,
	0x9dd926e7, 0xb4314c70, 0xe4d56abf, 0xb201c805,
	0x799d30b1, 0x43583307, 0x556e29c4, 0x8e537178,
	0xea6e3776, 0x7d856bd0, 0x35b1fc86, 0x4ab00799,
	0x6bd541fb, 0x5e2b088e, 0xd463faaa, 0xeba08321,
	0xf8823f64, 0xff46755f, 0x56c4846f, 0x80c9cd89,
	0x8ebfae8a, 0xd4eed1aa, 0x19143749, 0x9c5f5ea1,
	0xf2e973a9, 0x8cc7ea95, 0xdca7faaf, 0xf91decf4,
	0x9ad10624, 0x17d1dee2, 0x2ce33729, 0xbe9edaa2,
	0x40bbc862, 0x3add4124, 0x70ff5df4, 0x31e0f26d,
	0xd670a2a3, 0x8c624722, 0xa92d9918, 0x3a716970,
	0x168dcc17, 0xd59b7710, 0xfae4e083, 0xb712e7b3,
	0x8741e279, 0xd09ae676, 0x9716455f, 0x6d1bf7d0
};

static int test_data_3[] __unused = {
	0x9106c86f, 0x0e6c5c2b, 0xef847605, 0xc5a4c574,
	0x7a9a8be4, 0xda9d2d87, 0x447d7900, 0xbed71cd7,
	0x16ade88f, 0x614bcd97, 0x122a29dc, 0xa92c385a,
	0x80db10e1, 0x63d3900f, 0x28df2fc6, 0x9d072a2f,
	0x39c2e7e7, 0x560e2398, 0x9be3af89, 0xbeb7c287,
	0x60573a20, 0x8530fb1a, 0x90aba6e3, 0x5f932bb5,
	0x6ce237, 0xbe5f94df, 0x574bb571, 0x3b098a0,
	0xe53dc84b, 0x85d07f1d, 0x3f601b28, 0x7dfd6144,
	0x8b1f03ab, 0x1b4b8b26, 0x6d20e400, 0xb1f51b07,
	0xa0e6bce5, 0xdaeca65, 0xc2f5345b, 0xfef3a38d,
	0x934a36dc, 0x9d1d3b04, 0xe7811061, 0x3f9c673d,
	0xa41804c, 0x34eec339, 0x4a07670c, 0x161fd9a5,
	0x383ceb36, 0xf1f91965, 0x6a9b32c7, 0x5c9837cb,
	0x39b69a4b, 0x8926848, 0x30082309, 0x6e722703,
	0xd669fe24, 0x7c7991cb, 0x2366f142, 0x328bf354,
	0x48b74f73, 0x308923, 0xcfeeff13, 0x8042545b,
	0x596f136c, 0xf0685aff, 0xda2e4924, 0xf6fcaaf8,
	0x35be0637, 0x338be9c8, 0xa0573964, 0xcd3cb589,
	0x63b3858d, 0x5fb5b5f4, 0x3661fc27, 0x3cf5ae4,
	0x9efe9b65, 0x69ed695a, 0xb1374dd6, 0x24ec9001,
	0x58127677, 0xf6786136, 0xefc94f1, 0x3b7554ca,
	0x4497557b, 0x8886282a, 0x30f47aa2, 0xd1018c8a,
	0xd37b6081, 0xe56a7040, 0x94e1a201, 0xa03c742e,
	0xea2f429, 0x7abd6969, 0x11519421, 0x2c5c4022,
	0x7d3bcb7a, 0xe86db6af, 0x860ef4b1, 0xa9048aa1,
	0xc201fe27, 0x86a66e95, 0x3dec8457, 0xa083727a,
	0x649b1e7f, 0xe86b3d8b, 0xcfb16662, 0xc3c5ea77,
	0xaf4239, 0xc82d425b, 0x7ea43a67, 0xd0102ce3,
	0xbde9b498, 0xb0425de5, 0x58153b9, 0xc10635cd,
	0x1e3bc014, 0x5299729c, 0x1e984254, 0x8bb1816,
	0x8019590e, 0xb2ae548f, 0x6e11098b, 0x1f073d28,
	0x1cb2a16, 0x24554d20, 0x629c74c, 0xb4209cc6,
	0xee4b4c0b, 0x3a89a25c, 0x31062f52, 0x58157d37,
	0xa835a8e, 0x34f62cd2, 0x2736e3e, 0x20ac5f55,
	0xc683dce9, 0xfcb7a297, 0x101b1626, 0x36b6e3a0,
	0xdaf4a2dc, 0xcc952344, 0x676e9798, 0x5b17e1c8,
	0x1f8fe562, 0x3cde27bd, 0x908866ac, 0x26b195d5,
	0x46615951, 0xd5359883, 0x3b36fe79, 0xae785fda,
	0x4469b71e, 0x15f3f540, 0x10bb889e, 0x61d081c6,
	0x95477071, 0xb0dc30db, 0xdb793421, 0x2c83d529,
	0x9f7b0886, 0x51eba3a, 0x53f26bad, 0x6b0667db,
	0x6f7626b, 0xe65c340e, 0x3993a0c4, 0x9c6c0a98,
	0xcecf3485, 0x1c68974c, 0xd18709dd, 0x46f7838d,
	0x713dc77e, 0xf8e1bc37, 0xbb20dced, 0x8bf78572,
	0xa759c315, 0x34523cd2, 0xc5e1438f, 0x4047c206,
	0xda5fad51, 0xb61a4a62, 0x5730d9f0, 0xc731fb60,
	0xacde4ba5, 0xd4a91358, 0xd116acef, 0xaa745c0c,
	0xefa9c4d0, 0x3ddd508a, 0x130a9558, 0xf275ffab,
	0xd1244718, 0x1b95ad09, 0x2825ab22, 0x1005e480,
	0x340617c1, 0xc1770bff, 0xac2e09df, 0x23dacbe7
};

static int test_data_4[] __unused = {
	0xf25b202e, 0x3ab9dbf3, 0x3296a9e7, 0xd5bcae5d,
	0xc646fc50, 0xcea03e07, 0xb4b04e9, 0xb9eb06b,
	0x52f13333, 0x4548d747, 0x5cc1bd02, 0x620479bb,
	0x5f25c539, 0x26388179, 0x2f8680d7, 0xcaed720d,
	0x96bc8f92, 0xf9b0960e, 0xd08822b1, 0x9f2fea79,
	0xd2d78fed, 0xb58c0faf, 0x7ab9dbf3, 0xf30897da,
	0x18e898a3, 0xe9b70000, 0xaa5866ed, 0xf6e80dae,
	0x8ec6ccc9, 0xad4c657, 0xe1918e2f, 0xf0081003,
	0x7f3f36dd, 0x3881edae, 0x75b23245, 0xe2366b53,
	0x768b8fa6, 0x3bef4815, 0xb48fd37d, 0xa8114318,
	0xd0461d91, 0xcb2fbae4, 0x3f5a08f, 0x93fab8a1,
	0xad67b55, 0xd1a56a00, 0xc3f5944a, 0xfea22103,
	0x2176252f, 0x718e87c7, 0x99c1d8d2, 0x468881bf,
	0xe4b917ed, 0x367c9300, 0xb41c0c1c, 0x305c4e4d,
	0xcb98e807, 0x8968d792, 0x1e07d712, 0x6a1aa11c,
	0xed237623, 0x3361ee7b, 0x7738b22, 0x55297f44,
	0xe201f118, 0xbb8469e4, 0xa30d626e, 0x303797e9,
	0x8273e2bb, 0x5fc67b49, 0xb3a364a3, 0x1307fc31,
	0x9dccc9cd, 0x66a073b, 0x831577fa, 0xdfa3ba2f,
	0x113d3a75, 0x8b468ad1, 0x2a933a3e, 0xa09c5db5,
	0x13940182, 0xbf81cde5, 0xc8fe5eb3, 0x6d8deb33,
	0x3814c32d, 0x7d354aa5, 0xc2721bfc, 0x8d2d7071,
	0x55415700, 0x5b0791a7, 0x2b2e116d, 0x7d97a2b2,
	0x78b42035, 0xdc6e0580, 0x377e65ab, 0xa66329b1,
	0xcfa77c83, 0x64de1472, 0x8a92cca0, 0x212b38e4,
	0xf7f4d159, 0x8e31de3e, 0x8539b689, 0x65ef6ee6,
	0xdf754a5e, 0xb4afbdbb, 0x5af03880, 0xba8cac05,
	0x8186baf4, 0x60a6f8b5, 0x8c24237b, 0x1cb9eea7,
	0x2bca315a, 0xe3fce2ef, 0x6426490e, 0x2684f54e,
	0x9492325e, 0xc5140971, 0x69369200, 0xa98b364b,
	0xeab3a646, 0xed05a929, 0xb53e05fc, 0x887a244d,
	0x22feea1, 0xf080aeef, 0x231d95a7, 0xf05171e2,
	0x77fdc9a0, 0x425fb743, 0x8a9c085e, 0x2460f21a,
	0x62ddf665, 0x2eb87ed7, 0x56754336, 0xe6301156,
	0xdc450a2, 0xc24a517a, 0xc0b8a4a3, 0x3046297b,
	0xc12098e0, 0x32dd8e6e, 0x14b7d417, 0x56e3b6be,
	0xd897d081, 0xdabff5c3, 0xcf8bb085, 0xfd06d9b5,
	0xf086583c, 0x48092961, 0x1d394ed9, 0xb669f18f,
	0xf328bb7d, 0x35997b5b, 0x28921850, 0xe423e502,
	0x9da57f62, 0xc9b6cd6f, 0x6b4b98a5, 0x812730f9,
	0x6258bb0b, 0x69139814, 0xb8c18d88, 0x5c34fe7f,
	0x55af1a6b, 0x94804b79, 0xde941ee5, 0xa0509ff3,
	0x249d8b6, 0x8b40ae2c, 0xe20c2a8b, 0x5e1040d4,
	0xff6853b2, 0x55467307, 0x70209e7b, 0xb6b5f533,
	0x5b5487a7, 0xd8a64639, 0x29ade11f, 0xdcf02c97,
	0x2aa673e1, 0xdf5d4df4, 0x37cef382, 0xa4ca58e1,
	0xf436163d, 0x66a74eb3, 0x5bfb5029, 0x5a8db8a3,
	0xa6c3fafa, 0xc2263e44, 0xb001bdae, 0xd5032c98,
	0x5b7aa97f, 0x954c4ef2, 0x3b82f012, 0x3d0dd1d5,
	0xc8a0f330, 0xe482b21c, 0x15a863a2, 0x739282c
};

static int test_data_5[] __unused =
{
	0x60a127, 0xebf1501e, 0x8b91add2, 0x3ecf5c4b,
	0xed2d50b1, 0x3f70cd0d, 0x669f9b, 0xf90a1f1f,
	0x8fd11114, 0x1b14a785, 0xbdd6fcdb, 0x8267491b,
	0x2d63067b, 0x100db860, 0x1e793049, 0x6bc381be,
	0x7e6a1be0, 0xc69e3325, 0x7c147c02, 0x69bff88a,
	0x246017cb, 0xf3bf1407, 0xbd087942, 0x1d95751a,
	0xcea8e85e, 0xb346e589, 0x4fa96b, 0x2136f9c6,
	0xac2cd90c, 0xd0ca8da5, 0x9ad920b7, 0xfb2cc023,
	0x8c598b15, 0x4ec68a4e, 0x7ae1d909, 0x6f306d68,
	0x2a46439b, 0x49cc2d6d, 0x9199ccd7, 0x131c5903,
	0x86b7a06e, 0xdd26179, 0x2f52108a, 0xe9ef596c,
	0x4c78e80c, 0xe53b54f1, 0x1ca2d915, 0x41b09429,
	0xa11080b2, 0xb0d1369d, 0xd6e863ff, 0x46605b24,
	0x409a262c, 0x7cf4babd, 0x115d0e11, 0x9d5bfa11,
	0x68952af9, 0x69d569bd, 0x22b73cdc, 0x8d05b96f,
	0x686fbd9f, 0x38f3393, 0xa5bab60c, 0x75db2068,
	0x124f6cb5, 0xda3b5b0b, 0x489728ba, 0x487940c7,
	0xb6674b44, 0xdcaacf94, 0x3bb6d091, 0xf49858b7,
	0x92262ac4, 0x38e4ecd8, 0xd920b753, 0x11d0fcb7,
	0x31fcfa2e, 0xcae30bf2, 0x2019a55c, 0x432bee5b,
	0xf592d4d0, 0x93b2fabf, 0x7be42dac, 0xe7bf3862,
	0xecb14ad8, 0x25e42b12, 0xded046cd, 0x4ad01d26,
	0x854df864, 0xa5b9801a, 0x5752c158, 0x592e4c6c,
	0xd79798f0, 0x74b953a6, 0xe6023afc, 0x376bf36d,
	0xa509e2de, 0x6c25e80f, 0xb906874b, 0xddf34c85,
	0x8a56366d, 0xe549ea53, 0xcbb268eb, 0x1d17c737,
	0xded393ae, 0xac6f5182, 0x4f176546, 0x1b9ceee4,
	0x13c5d5be, 0x90002959, 0x535fd53d, 0x1839d0bd,
	0xa17c8146, 0x6445e81d, 0x8b9cc190, 0x5163330a,
	0xbe10602f, 0xa9eb315d, 0xe2afc0c2, 0xc5e563c9,
	0x63de7504, 0x11634448, 0x9a82d1c0, 0xf3aa2949,
	0xce730e14, 0xd341cea9, 0x1e23af71, 0x21323198,
	0xf86475af, 0x8c390230, 0xd4b06bf3, 0x558f53ab,
	0x8dd82343, 0xa42c522d, 0x4ab6db20, 0x3360fa96,
	0xb747e80c, 0xc9998681, 0x3ef39470, 0x29e217ce,
	0xe1ffd7eb, 0xed7b24a1, 0xa086914f, 0x6775f747,
	0x86845f1b, 0xab53f1b0, 0x736f8fd9, 0xaf08b8fd,
	0x733928ad, 0xf1c70c77, 0xd36e9040, 0x676e2163,
	0x29802bd1, 0x95fd5321, 0xfde2259f, 0x96e866fa,
	0x2ac071b0, 0x89d96d4d, 0x1bd4e4b6, 0x3d0754a9,
	0x25728e4a, 0xf79582cc, 0xf917283d, 0xa5a85603,
	0x6abbc68b, 0x1717238d, 0x499373cc, 0xc7d47807,
	0x5079ae80, 0x9296827e, 0xd2fb682c, 0xf66d6841,
	0x5ace92e0, 0x6d77aded, 0xf15b4657, 0x6d2ed45d,
	0x64422d70, 0x9ba777d8, 0x246d48ec, 0x2cfcfd67,
	0xc5abf1b1, 0xb3598dbb, 0xa66534d6, 0x78e2d9d0,
	0x270fe88b, 0xbbd3cd2b, 0x5a2edc02, 0xeb8b4883,
	0x3716c7ca, 0x3e2b3382, 0x52395d13, 0x9ea4fad1,
	0x15e0335d, 0xb1ff20cf, 0xc5e05494, 0x6af17a00,
	0x5e83edfb, 0xb54f0f4f, 0x408d709e, 0x417132d7
};

static int test_data_6[] __unused =
{
	0xe9c747ce, 0x85c9da67, 0xded7ac4f, 0x4ca436da,
	0x44d29dff, 0x262d7cfb, 0x61491a4b, 0x2d3c7fbe,
	0x124fb1b9, 0xae092dc9, 0xf2b69ab0, 0xf6d30bca,
	0x536fb2d4, 0x586fd5de, 0x42412587, 0xbed352b0,
	0x7bb0fd07, 0xde56ab99, 0xcd452a28, 0x5b767a48,
	0x8b152721, 0x5b775b75, 0xa3ef9b68, 0xa5ced8fd,
	0xfba7908b, 0xafefc724, 0x2dadff58, 0xd53d50b0,
	0x7a0694bb, 0x4e06ffb1, 0xd1899bd2, 0x568e4855,
	0x4fb0d7f7, 0x7b1030a1, 0xc2daf8d5, 0x21c06f99,
	0x7b520afd, 0xfa929b31, 0xc4655b1d, 0xc5cea43e,
	0xe289e7fb, 0x47b1e873, 0xbb96b1cb, 0xd6a24493,
	0x15b712f, 0x61ebc222, 0xe6c82534, 0x180437e2,
	0x51ee8513, 0xc0d3f69, 0x9aa5accd, 0x97553013,
	0xa790625e, 0xe0e2097a, 0x3246c565, 0xdf82c975,
	0xf153fe77, 0xf6c04aed, 0xe8f21247, 0xffad6c9b,
	0x286b40f6, 0x56ea46cd, 0x37df2814, 0x14e2a4fc,
	0xcb994a62, 0xfeb3e7ac, 0x2e4043d6, 0xe576a169,
	0x9fb0a85, 0xe426708c, 0x5d03f6fa, 0x6999eaad,
	0xcc6be2c1, 0xda0f7fcb, 0x1415c0d9, 0xea8c2686,
	0x1cfdabda, 0x40655ab9, 0xb89ed80c, 0x29b6dd90,
	0xb2379c56, 0xc323552, 0x62836ad7, 0xa6d52fcc,
	0xcc1ace31, 0x6cdb6985, 0x232c18f5, 0xb58dd07b,
	0x2ae10593, 0x808de171, 0x8992c1ec, 0x81407f1a,
	0x3af8c280, 0xe147ae3f, 0x18fdcd94, 0x5d3687c2,
	0x4c0d364a, 0xad33eb2c, 0xd2e433bb, 0x13e898b0,
	0xee771595, 0x736ce827, 0xb987b28f, 0x9069b42c,
	0x773ca272, 0xd092d75d, 0xe2e96061, 0x5ec1ed32,
	0xa0cef44f, 0xb96f3139, 0x289eeb23, 0xdd03b223,
	0x6844792d, 0x8569d563, 0xb263089a, 0x59ad8514,
	0x8fc62026, 0xbb8fb81b, 0x29b729bc, 0x2b990b44,
	0x439a37db, 0x75346165, 0x77a3c987, 0x66b4f47c,
	0x11892953, 0xd63d7b76, 0x90a3ffc2, 0x6691572f,
	0x90d35ee5, 0xd2daf9ff, 0xd9c1ca16, 0xe0346d7f,
	0x506f9f9f, 0x1786f5ee, 0xa1906cec, 0x242a1d15,
	0xa788674c, 0xe1af16ec, 0xf5d654a8, 0x4bb691a3,
	0xd7e9cf83, 0x7d409e79, 0xbb2e3e63, 0xefd04fa,
	0x15a1f335, 0xf11dad22, 0x4e4bab54, 0x62357edd,
	0x3c48419b, 0xf5d95a57, 0x4f410f8, 0xea3cd13e,
	0xab99e715, 0x3f4cd4d2, 0xc730ee9a, 0x21bb6e5c,
	0x48cf204e, 0x3d2e9cd, 0x990b1d71, 0x83015da,
	0xc4edae3b, 0x47ccd90e, 0xf8f7fd71, 0x9341ab40,
	0x2a219b39, 0xbe36da48, 0x8c9bbf82, 0x32b60862,
	0xc6a29e7f, 0x1f6d36d7, 0x74cc0f07, 0xb648d31a,
	0x72de5219, 0x314b0ef6, 0x32b84e74, 0x40a9bdc1,
	0xa54b548e, 0x23d230c5, 0xff4c5ffe, 0x3b756900,
	0xffe47563, 0x8a730c47, 0x8dea14e6, 0xba73fe7b,
	0xa8028eae, 0x35f03077, 0xd4a21bf, 0x908faea4,
	0x1b73e80, 0xfc261de3, 0x2287f16b, 0x5e09c2e,
	0xdb12f3fb, 0x5d422e7a, 0xf25b1a66, 0x89307f10,
	0xd477ef2c, 0x94988a7e, 0x8e4917ca, 0x5e928051,
	0x62e07eb4, 0x2e893d6c, 0xd8569278, 0xe5680dc5,
	0x7561b1ea, 0x63d67eeb, 0x8a3c6065, 0xce26efe3,
	0x4cd9d630, 0x72bd6106, 0x3cfb5e6a, 0x582ad4fc,
	0x8fbd3a3c, 0xb8879005, 0xde971c47, 0x392561e2,
	0x590ac024, 0x39f7fa0c, 0x3734275e, 0xd8c80a9c,
	0x587f0e23, 0x58642ac7, 0xde14e192, 0xb10c6498,
	0xfab7bfca, 0xa719baee, 0xe7f50e8a, 0x59d01aa8,
	0xe3e6bd17, 0x2927bb64, 0xdfb5e4dc, 0xd2906169,
	0x33459096, 0x11219816, 0xf6add184, 0x9f4c9faa,
	0x9ca9c0a9, 0x3119605c, 0x2a9b2f74, 0x4e288fa8,
	0x88234246, 0xbf203bfd, 0x205fdcb1, 0x61ad9582,
	0x3880af56, 0x6e1c12ef, 0xa3958851, 0x332d7a55,
	0xcd8df0e3, 0x4d8d119e, 0x205a0c3c, 0xde0c902f,
	0xf1db8bb2, 0xf84dbd39, 0xe2116887, 0xbed6ea21,
	0x969123c6, 0x3d87ba71, 0xb07a20be, 0x9c2d3b7,
	0x7a8a7f9f, 0xda37ac3b, 0xfe44c4e7, 0x5b88b75c,
	0x8c3b83d2, 0xac2b0254, 0x48af64c1, 0x6faaf864,
	0xf2a7349, 0x8e2c6a84, 0x8a1cccc1, 0x8f77c111,
	0x887cf1ef, 0x3e8cb2b, 0xc90a625, 0x99ddf38b,
	0xa522c690, 0xf7de4d29, 0xb542d426, 0xcac23ee7,
	0x8f2883c9, 0x6ea62f65, 0x2c9e1d6e, 0x63b4639a,
	0xb331fd8b, 0x2c5e68b8, 0xa50b1e63, 0xc52b5f1,
	0xfd51ae7f, 0x8090f02d, 0x4d1a6bb, 0x3f356c30,
	0x9c35e639, 0x319cc02d, 0x9b2f3555, 0x6dc87ce2,
	0x3d785f, 0x4c87a81c, 0x607f786f, 0x6a58b9ca,
	0x43be24e5, 0xb9b45f1d, 0xdb460504, 0x1244b194,
	0xe843780c, 0x35841e6e, 0xe5886943, 0x2aff3898,
	0xf1307701, 0x90d73470, 0x4ebf111, 0x82a3c659,
	0xba9d0184, 0xaca5c21e, 0x8f7c5353, 0xa4aa4103,
	0x370ac1d0, 0xc98a3371, 0xc4598623, 0x66f7d826,
	0x689295c2, 0xa745dfb, 0x32ca8a3c, 0xc9ec512d,
	0x651afd11, 0xbed044aa, 0x6c3082d4, 0x89c392c2,
	0x8c0858c3, 0xa29117a8, 0x679c9cc8, 0x161c870c,
	0xa50cb9fa, 0x2607777b, 0x50f00798, 0x2c7a3f5e,
	0x131a8504, 0xe38c1ce, 0xb4f782d6, 0x38c99aa,
	0xe9d4b00e, 0x9edc3b2e, 0x64b95760, 0x70655bae,
	0x4e7dfb8, 0xade34c41, 0x1dcf68f7, 0x9310e30,
	0x11c88322, 0xf5e59fce, 0xe719b95d, 0xef6ada0f,
	0xc1d00dd9, 0xe05e0c3b, 0xb93ddbe3, 0x39212e32,
	0xdbc189a7, 0x75227d50, 0x8fb20e30, 0x4889f12f,
	0xfaa5f64e, 0x73e4ea79, 0xcecc84ba, 0x70339490,
	0xfe825eac, 0x8a169b5d, 0x11071ff8, 0xff3a19f2,
	0x4843c5d2, 0x4ca6f505, 0x63355612, 0xe3bc5da9,
	0x9576b2ce, 0x6ccbcfe5, 0x991b8608, 0x1635a89e,
	0xabc809b7, 0xd9cf454f, 0x30102337, 0xe21da84e,
	0x6ceb6503, 0x789b6f79, 0x918509fd, 0x16517db7,
	0xd09fe0e2, 0x99560a99, 0xf98d1588, 0xa3775a05,
	0x8b674d8, 0xdbbf1615, 0x20babb00, 0x891535fe,
	0x4a811d8a, 0x55a9fbe1, 0x1065b4ec, 0xf0c96a68,
	0xb94e3952, 0xbb18f579, 0x54ed916d, 0x737c3d3e
};
/* }}} */

__unused static void
test_basic(void)
{
	test_2(1, 2);
	test_2(2, 1);

	test_2(0x00ff, 0xff00);
	test_2(0xff00, 0x00ff);

	test_2(0, -1);
	test_2(-1, 0);

	test_2heads(0x00ff, 0xff00);
	test_2heads(0xff00, 0x00ff);

	test_dup(100);
}

int
main(int argc __unused, char **argv __unused)
{
	const int iter = 100, elems = 50000;
	int i;

	struct test_descr {
		test_method_t *method;
		const char *name;
	} *ti, tests[] = {
		{ test_insert_remove, "insert-remove" },
		{ test_insert_remove_chunk, "insert-remove by chunk" },
		{ test_first, "first" },
		{ test_last, "last", },
		{ test_next, "next", },
		{ test_prev, "prev", },
		{ test_nfind, "nfind", },
		{ NULL, NULL },
	};

	test_basic();

	for (ti = tests; ti->method != NULL; ti++) {
		printf("test: %s\n", ti->name);
		ti->method(test_data_1, sizeof(test_data_1)/4);
		ti->method(test_data_2, sizeof(test_data_2)/4);
		ti->method(test_data_3, sizeof(test_data_3)/4);
		ti->method(test_data_4, sizeof(test_data_4)/4);
		ti->method(test_data_5, sizeof(test_data_5)/4);
		ti->method(test_data_6, sizeof(test_data_6)/4);
	}

	if (test_opt_fragmentation != 0) {
		test_pool_fragmentation(100, 20, test_gen_key_seed);
		test_pool_fragmentation(10000, 2000, test_gen_key_seed);
		test_pool_fragmentation(100000, 20000, test_gen_key_seed);
	}

	for (i = 0; test_opt_random != 0 && i < iter; i++) {
		if (i % 10 == 0)
			printf("random test %d/%d: %d elements\n",
			    i, iter, elems);
		for (ti = tests; ti->method != NULL; ti++)
			test_random(ti->method, elems);
	}

	return (0);
}
