// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2011-2013  ProFUSION embedded systems
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <shared/hash.h>
#include <shared/util.h>

struct hash_entry {
	const char *key;
	const void *value;
};

struct hash_bucket {
	struct hash_entry *entries;
	unsigned int used;
	unsigned int total;
};

struct hash {
	unsigned int count;
	unsigned int step;
	unsigned int n_buckets;
	void (*free_value)(void *value);
	struct hash_bucket buckets[];
};

struct hash *hash_new(unsigned int n_buckets, void (*free_value)(void *value))
{
	struct hash *hash;

	n_buckets = align_power2(n_buckets);
	hash = calloc(1, sizeof(struct hash) + n_buckets * sizeof(struct hash_bucket));
	if (hash == NULL)
		return NULL;
	hash->n_buckets = n_buckets;
	hash->free_value = free_value;
	hash->step = n_buckets / 32;
	if (hash->step < 4)
		hash->step = 4;
	else if (hash->step > 64)
		hash->step = 64;
	return hash;
}

void hash_free(struct hash *hash)
{
	struct hash_bucket *bucket, *bucket_end;

	if (hash == NULL)
		return;

	bucket = hash->buckets;
	bucket_end = bucket + hash->n_buckets;
	for (; bucket < bucket_end; bucket++) {
		if (hash->free_value && bucket->used != 0) {
			struct hash_entry *entry, *entry_end;
			entry = bucket->entries;
			entry_end = entry + bucket->used;
			for (; entry < entry_end; entry++)
				hash->free_value((void *)entry->value);
		}
		free(bucket->entries);
	}
	free(hash);
}

static inline unsigned int hash_superfast(const char *key, unsigned int len)
{
	/* Paul Hsieh (http://www.azillionmonkeys.com/qed/hash.html)
	 * used by WebCore (http://webkit.org/blog/8/hashtables-part-2/)
	 * EFL's eina and possible others.
	 */
	unsigned int tmp, hash = len, rem = len & 3;

	len /= 4;

	/* Main loop */
	for (; len > 0; len--) {
		hash += get_unaligned((uint16_t *)key);
		tmp = (get_unaligned((uint16_t *)(key + 2)) << 11) ^ hash;
		hash = (hash << 16) ^ tmp;
		key += 4;
		hash += hash >> 11;
	}

	/* Handle end cases */
	switch (rem) {
	case 3:
		hash += get_unaligned((uint16_t *)key);
		hash ^= hash << 16;
		hash ^= key[2] << 18;
		hash += hash >> 11;
		break;

	case 2:
		hash += get_unaligned((uint16_t *)key);
		hash ^= hash << 11;
		hash += hash >> 17;
		break;

	case 1:
		hash += *key;
		hash ^= hash << 10;
		hash += hash >> 1;
	}

	/* Force "avalanching" of final 127 bits */
	hash ^= hash << 3;
	hash += hash >> 5;
	hash ^= hash << 4;
	hash += hash >> 17;
	hash ^= hash << 25;
	hash += hash >> 6;

	return hash;
}

/*
 * add or replace key in hash map.
 *
 * none of key or value are copied, just references are remembered as is,
 * make sure they are live while pair exists in hash!
 */
int hash_add(struct hash *hash, const char *key, const void *value)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval & (hash->n_buckets - 1);
	struct hash_bucket *bucket = hash->buckets + pos;
	struct hash_entry *entry, *entry_end;

	if (bucket->used + 1 >= bucket->total) {
		unsigned new_total = bucket->total + hash->step;
		size_t size = new_total * sizeof(struct hash_entry);
		struct hash_entry *tmp = realloc(bucket->entries, size);
		if (tmp == NULL)
			return -ENOMEM;
		bucket->entries = tmp;
		bucket->total = new_total;
	}

	entry = bucket->entries;
	entry_end = entry + bucket->used;
	for (; entry < entry_end; entry++) {
		int c = strcmp(key, entry->key);
		if (c == 0) {
			if (hash->free_value)
				hash->free_value((void *)entry->value);
			entry->key = key;
			entry->value = value;
			return 0;
		} else if (c < 0) {
			memmove(entry + 1, entry,
				(entry_end - entry) * sizeof(struct hash_entry));
			break;
		}
	}

	entry->key = key;
	entry->value = value;
	bucket->used++;
	hash->count++;
	return 0;
}

/* similar to hash_add(), but fails if key already exists */
int hash_add_unique(struct hash *hash, const char *key, const void *value)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval & (hash->n_buckets - 1);
	struct hash_bucket *bucket = hash->buckets + pos;
	struct hash_entry *entry, *entry_end;

	if (bucket->used + 1 >= bucket->total) {
		unsigned new_total = bucket->total + hash->step;
		size_t size = new_total * sizeof(struct hash_entry);
		struct hash_entry *tmp = realloc(bucket->entries, size);
		if (tmp == NULL)
			return -ENOMEM;
		bucket->entries = tmp;
		bucket->total = new_total;
	}

	entry = bucket->entries;
	entry_end = entry + bucket->used;
	for (; entry < entry_end; entry++) {
		int c = strcmp(key, entry->key);
		if (c == 0)
			return -EEXIST;
		else if (c < 0) {
			memmove(entry + 1, entry,
				(entry_end - entry) * sizeof(struct hash_entry));
			break;
		}
	}

	entry->key = key;
	entry->value = value;
	bucket->used++;
	hash->count++;
	return 0;
}

static int hash_entry_cmp(const void *pa, const void *pb)
{
	const struct hash_entry *a = pa;
	const struct hash_entry *b = pb;
	return strcmp(a->key, b->key);
}

void *hash_find(const struct hash *hash, const char *key)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval & (hash->n_buckets - 1);
	const struct hash_bucket *bucket = hash->buckets + pos;
	const struct hash_entry se = {
		.key = key,
		.value = NULL,
	};
	const struct hash_entry *entry;

	if (!bucket->entries)
		return NULL;

	entry = bsearch(&se, bucket->entries, bucket->used, sizeof(struct hash_entry),
			hash_entry_cmp);

	return entry ? (void *)entry->value : NULL;
}

int hash_del(struct hash *hash, const char *key)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval & (hash->n_buckets - 1);
	unsigned int steps_used, steps_total;
	struct hash_bucket *bucket = hash->buckets + pos;
	struct hash_entry *entry, *entry_end;
	const struct hash_entry se = {
		.key = key,
		.value = NULL,
	};

	if (bucket->entries == NULL)
		return -ENOENT;

	entry = bsearch(&se, bucket->entries, bucket->used, sizeof(struct hash_entry),
			hash_entry_cmp);
	if (entry == NULL)
		return -ENOENT;

	if (hash->free_value)
		hash->free_value((void *)entry->value);

	entry_end = bucket->entries + bucket->used;
	memmove(entry, entry + 1, (entry_end - entry - 1) * sizeof(struct hash_entry));

	bucket->used--;
	hash->count--;

	steps_used = bucket->used / hash->step;
	steps_total = bucket->total / hash->step;
	if (steps_used + 1 < steps_total) {
		size_t size = (steps_used + 1) * hash->step * sizeof(struct hash_entry);
		struct hash_entry *tmp = realloc(bucket->entries, size);
		if (tmp) {
			bucket->entries = tmp;
			bucket->total = (steps_used + 1) * hash->step;
		}
	}

	return 0;
}

unsigned int hash_get_count(const struct hash *hash)
{
	return hash->count;
}

void hash_iter_init(const struct hash *hash, struct hash_iter *iter)
{
	iter->hash = hash;
	iter->bucket = 0;
	iter->entry = -1;
}

bool hash_iter_next(struct hash_iter *iter, const char **key, const void **value)
{
	const struct hash_bucket *b = iter->hash->buckets + iter->bucket;
	const struct hash_entry *e;

	iter->entry++;

	if (iter->entry >= b->used) {
		iter->entry = 0;

		for (iter->bucket++; iter->bucket < iter->hash->n_buckets;
		     iter->bucket++) {
			b = iter->hash->buckets + iter->bucket;

			if (b->used > 0)
				break;
		}

		if (iter->bucket >= iter->hash->n_buckets)
			return false;
	}

	e = b->entries + iter->entry;

	if (value != NULL)
		*value = e->value;
	if (key != NULL)
		*key = e->key;

	return true;
}
