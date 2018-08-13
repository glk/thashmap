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
#include <sys/time.h>
#include <sys/queue.h>

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "thashmap.h"

#include "tree.h"
#define RB_COMPAT
#include "rb.h"

#if !defined(__unused)
#define __unused
#endif

struct s_thm {
	struct thm_entry entry;
	uint32_t	key;
};

struct s_rb {
	RB_ENTRY(s_rb)	entry;
	uint32_t	key;
};

struct s_llrb {
	LLRB_ENTRY(s_llrb) entry;
	uint32_t	key;
};

struct s_hashtbl {
	LIST_ENTRY(s_hashtbl) entry;
	uint32_t	key;
};

THM_DEFINE(s_thm, s_thm, entry, key);

static __inline int
s_rbtree_cmp(struct s_rb *a, struct s_rb *b)
{
        return a->key - b->key;
}

RB_HEAD(s_rbtree, s_rb);
RB_GENERATE_STATIC(s_rbtree, s_rb, entry, s_rbtree_cmp);

static __inline int
s_llrbtree_cmp(struct s_llrb *a, struct s_llrb *b)
{
        return a->key - b->key;
}

LLRB_HEAD(s_llrbtree, s_llrb);
LLRB_PROTOTYPE_STATIC(s_llrbtree, s_llrb);
LLRB_GENERATE_STATIC(s_llrbtree, s_llrb, entry, s_llrbtree_cmp);

LIST_HEAD(s_hashtbl_head, s_hashtbl);

static void
benchmark_result(const char *name, intmax_t n,
    struct timeval *tstart, struct timeval *tend)
{
	double t;

	t = tend->tv_sec - tstart->tv_sec;
	t += (double)(tend->tv_usec - tstart->tv_usec) / (double)1000000;
	printf("%16s: %jd elements in %lf seconds; %lf elements/s\n",
	    name, n, t, (double)n/t);
}

#define TEST(insert_subr, insert_check, setkey, find_subr, remove_subr) \
	for (i = 0; i < n; i++) {					\
		elm = &elm_list[i];					\
		elm->key = keys[i];					\
		insert_subr;						\
		if (!(insert_check)) {					\
			printf("insert failed: %d/%d\n", i, n);		\
			abort();					\
		}							\
	}								\
	for (i = 0; i < n; i += 2) {					\
		elm = &elm_list[i];					\
		setkey = elm->key;					\
		r = find_subr;						\
		if (r != elm)						\
			abort();					\
	}								\
	for (i = 1; i < n; i += 2) {					\
		elm = &elm_list[i];					\
		setkey = elm->key;					\
		r = find_subr;						\
		if (r != elm)						\
			abort();					\
	}								\
	for (i = 0; i < n; i += 4) {					\
		elm = &elm_list[i];					\
		setkey = elm->key + 1;					\
		find_subr;						\
	}								\
	for (i = n - 1; i >= 0; i--) {					\
		elm = &elm_list[i];					\
		setkey = elm->key;					\
		r = find_subr;						\
		if (r != elm)						\
			abort();					\
	}								\
	for (i = 0; i < n; i++) {					\
		elm = &elm_list[i];					\
		remove_subr;						\
	}

static void
test_thm(int *keys, int n)
{
	struct timeval tstart, tend;
	struct thm_pool pool;
	THM_HEAD(s_thm) head;
	THM_BUCKET(s_thm) *bucket;

	thm_pool_init(&pool, "thashmap-bench");

	THM_HEAD_INIT(s_thm, &head, &pool);

	struct s_thm *elm, *elm_list;
	struct s_thm *r;
	uint32_t key;
	int i;

	elm_list = malloc(sizeof(*elm) * n);

	for (i = 0; i < n; i+= 230)
		thm_pool_new_block(&pool);
	thm_pool_new_block(&pool);

	gettimeofday(&tstart, NULL);

	TEST(bucket = THM_INSERT(s_thm, &head, elm), bucket != NULL,
	    key, THM_BUCKET_FIRST(s_thm, THM_FIND(s_thm, &head, key, NULL)),
	    THM_REMOVE(s_thm, &head, elm));

	gettimeofday(&tend, NULL);

	THM_HEAD_DESTROY(s_thm, &head);
	thm_pool_destroy(&pool);

	free(elm_list);

	benchmark_result("thashmap", n, &tstart, &tend);
}

static void
test_rbtree(int *keys, int n)
{
	struct timeval tstart, tend;
	struct s_rbtree head;

	RB_INIT(&head);

	struct s_rb *elm, *elm_list;
	struct s_rb *r;
	struct s_rb key;
	int i;

	elm_list = malloc(sizeof(*elm) * n);

	gettimeofday(&tstart, NULL);

	TEST(r = RB_INSERT(s_rbtree, &head, elm), r == NULL,
	    key.key, RB_FIND(s_rbtree, &head, &key),
	    RB_REMOVE(s_rbtree, &head, elm));


	gettimeofday(&tend, NULL);

	free(elm_list);

	benchmark_result("rbtree", n, &tstart, &tend);
}

static void
test_llrbtree(int *keys, int n)
{
	struct timeval tstart, tend;
	struct s_llrbtree head;

	s_llrbtree_new(&head);

	struct s_llrb *elm, *elm_list;
	struct s_llrb *r;
	struct s_llrb key;
	int i;

	elm_list = malloc(sizeof(*elm) * n);

	gettimeofday(&tstart, NULL);

	TEST(LLRB_INSERT(s_llrbtree, &head, elm), 1,
	    key.key, LLRB_FIND(s_llrbtree, &head, &key),
	    LLRB_REMOVE(s_llrbtree, &head, elm));


	gettimeofday(&tend, NULL);

	free(elm_list);

	benchmark_result("llrbtree", n, &tstart, &tend);
}

static void *
hashtbl_init(int elements, uint32_t *hashmask)
{
	long hashsize;
	LIST_HEAD(generic, generic) *hashtbl;
	int i;

	assert(elements > 0);

	for (hashsize = 1; hashsize <= elements; hashsize <<= 1)
		continue;
	hashsize >>= 1;

	hashtbl = malloc((u_long)hashsize * sizeof(*hashtbl));

	for (i = 0; i < hashsize; i++)
		LIST_INIT(&hashtbl[i]);

	*hashmask = hashsize - 1;

	return hashtbl;
}

static __inline void
hashtbl_insert(struct s_hashtbl_head *hashtbl, uint32_t hashmask, struct s_hashtbl *elm)
{
	struct s_hashtbl_head *head;

	head = &hashtbl[elm->key & hashmask];
	LIST_INSERT_HEAD(head, elm, entry);
}

static __inline void
hashtbl_remove(struct s_hashtbl *elm)
{
	LIST_REMOVE(elm, entry);
}

static __inline struct s_hashtbl *
hashtbl_search(struct s_hashtbl_head *hashtbl, uint32_t hashmask, uint32_t key)
{
	struct s_hashtbl_head *head;
	struct s_hashtbl *elm;

	head = &hashtbl[key & hashmask];
	LIST_FOREACH(elm, head, entry) {
		if (elm->key == key)
			return (elm);
	}

	return (NULL);
}

static void
test_hashtbl(int *keys, int n, int hashdiv)
{
	struct timeval tstart, tend;
	struct s_hashtbl_head *head;
	uint32_t hashmask;

	head = hashtbl_init(n/hashdiv, &hashmask);

	struct s_hashtbl *elm, *elm_list;
	struct s_hashtbl *r;
	uint32_t key;
	int i;

	elm_list = malloc(sizeof(*elm) * n);

	gettimeofday(&tstart, NULL);

	TEST(hashtbl_insert(head, hashmask, elm), 1,
	    key, hashtbl_search(head, hashmask, key),
	    hashtbl_remove(elm));

	gettimeofday(&tend, NULL);

	free(elm_list);
	free(head);

	char namebuf[32];
	snprintf(namebuf, sizeof(namebuf), "hashbuf/%d", hashdiv);
	benchmark_result(namebuf, n, &tstart, &tend);
}

static int
key_random(void)
{
#if defined(__FreeBSD__)
	return (arc4random() & THM_KEY_MASK);
#else
	return (random() & THM_KEY_MASK);
#endif
}

static int
key_cmp(const void *xa, const void *xb)
{
	const int *a = xa, *b = xb;

	return (((*a) & THM_KEY_MASK) - ((*b) & THM_KEY_MASK));
}

static void
remove_dup(int *keys, int n)
{
	int i, j, restart, *keys_sorted;

	keys_sorted = malloc(n * sizeof(int));
	do {
		restart = 0;
		memcpy(keys_sorted, keys, n * sizeof(int));
		qsort(keys_sorted, n, sizeof(int), key_cmp);
		for (i = 1; i < n; i++) {
			if (keys_sorted[i - 1] != keys_sorted[i])
				continue;
			restart = 1;
			for (j = 0; j < n; j++) {
				if (keys[j] != keys_sorted[i])
					continue;
				keys[j] = key_random();
			}
		}
	} while (restart != 0);
	free(keys_sorted);
}

int
main(int argc, char **argv)
{
	int i, n, ntests, *keys;

	n = 200000;
	ntests = 10;

	if (argc >= 3) {
		ntests = atoi(argv[2]);
		if (ntests <= 0) {
			fprintf(stderr, "invalid number: %s\n", argv[2]);
			return (1);
		}
	}

	if (argc >= 2) {
		n = atoi(argv[1]);
		if (ntests <= 0) {
			fprintf(stderr, "invalid number: %s\n", argv[1]);
			return (1);
		}
	}

	/*
	 * Handling duplicates in rb-tree by storing them in linked list is
	 * rather messy, remove all duplicates instead. Assume there is no
	 * duplicates in thashmap test as well.
	 */
	keys = malloc(n * sizeof(int));
	for (; ntests > 0; ntests--) {
		for (i = 0; i < n; i++)
			keys[i] = key_random();
		remove_dup(keys, n);

		test_thm(keys, n);
		test_hashtbl(keys, n, 1);
		test_hashtbl(keys, n, 4);
		test_hashtbl(keys, n, 8);
		test_rbtree(keys, n);
		test_llrbtree(keys, n);
	}
	free(keys);

	return (0);
}
