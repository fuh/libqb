/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * Author: Steven Dake <sdake@redhat.com>
 *
 * This file is part of libqb.
 *
 * libqb is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * libqb is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libqb.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "os_base.h"

#include <qb/qbmap.h>
#include "util_int.h"
#include "map_int.h"
#include <qb/qblist.h>

#define FNV_32_PRIME ((uint32_t)0x01000193)

struct hash_node {
	struct qb_list_head list;
	void *value;
	void *key;
};

struct hash_bucket {
	struct qb_list_head list_head;
};

struct hash_table {
	struct qb_map map;
	size_t count;
	uint32_t order;
	qb_hash_func hash_fn;
	uint32_t hash_buckets_len;
	struct hash_bucket hash_buckets[0];
};

static uint32_t
hash_fnv(const void *value, uint32_t valuelen, uint32_t order)
{
	uint8_t *cd = (uint8_t *) value;
	uint8_t *ed = (uint8_t *) value + valuelen;
	uint32_t hash_result = 0x811c9dc5;
	int32_t res;

	while (cd < ed) {
		hash_result ^= *cd;
		hash_result *= FNV_32_PRIME;
		cd++;
	}
	res = ((hash_result >> order) ^ hash_result) & ((1 << order) - 1);
	return (res);
}

uint32_t
qb_hash_string(const void *key, uint32_t order)
{
	char* str = (char*)key;
	return hash_fnv(key, strlen(str), order);
}

uint32_t
qb_hash_char(const void *key, uint32_t order)
{
	return hash_fnv(key, sizeof(char), order);
}

uint32_t
qb_hash_pointer(const void *key, uint32_t order)
{
	return hash_fnv(key, sizeof(uint32_t), order);
}

static void*
hashtable_get(struct qb_map *map, const void* key)
{
	struct hash_table *hash_table = (struct hash_table *)map;
	uint32_t hash_entry;
	struct qb_list_head *list;
	struct hash_node *hash_node;

	hash_entry = hash_table->hash_fn(key, hash_table->order);

	for (list = hash_table->hash_buckets[hash_entry].list_head.next;
	     list != &hash_table->hash_buckets[hash_entry].list_head;
	     list = list->next) {

		hash_node = qb_list_entry(list, struct hash_node, list);
		if (map->key_compare_func(hash_node->key, key,
					  map->key_compare_data) == 0) {
			return hash_node->value;
		}
	}

	return NULL;
}

static int32_t
hashtable_rm_with_hash(struct qb_map *map, const void* key,
		       uint32_t hash_entry)
{
	struct hash_table *hash_table = (struct hash_table *)map;
	struct qb_list_head *list;
	struct hash_node *hash_node;

	for (list = hash_table->hash_buckets[hash_entry].list_head.next;
	     list != &hash_table->hash_buckets[hash_entry].list_head;
	     list = list->next) {

		hash_node = qb_list_entry(list, struct hash_node, list);
		if (map->key_compare_func(hash_node->key, key,
					  map->key_compare_data) == 0) {

			if (map->key_destroy_func) {
				map->key_destroy_func(hash_node->key);
			}
			if (map->value_destroy_func) {
				map->value_destroy_func(hash_node->value);
			}
			qb_list_del(&hash_node->list);
			free(hash_node);
			hash_table->count--;
			return QB_TRUE;
		}
	}

	return QB_FALSE;
}

static int32_t
hashtable_rm(struct qb_map *map, const void* key)
{
	struct hash_table *hash_table = (struct hash_table *)map;
	uint32_t hash_entry;

	hash_entry = hash_table->hash_fn(key, hash_table->order);
	return hashtable_rm_with_hash(map, key, hash_entry);
}

static void
hashtable_put(struct qb_map *map, const void* key, const void* value)
{
	struct hash_table *hash_table = (struct hash_table *)map;
	uint32_t hash_entry;
	struct hash_node *hash_node;

	hash_entry = hash_table->hash_fn(key, hash_table->order);
	hashtable_rm_with_hash(map, key, hash_entry);
	hash_node = malloc(sizeof(struct hash_node));
	if (hash_node == NULL) {
		errno = ENOMEM;
		return;
	}

	hash_table->count++;
	hash_node->key = (void*)key;
	hash_node->value = (void*)value;
	qb_list_init(&hash_node->list);
	qb_list_add_tail(&hash_node->list,
			 &hash_table->hash_buckets[hash_entry].
			 list_head);
}

static size_t
hashtable_count_get(struct qb_map *map)
{
	struct hash_table *hash_table = (struct hash_table *)map;
	return hash_table->count;
}

static void
hashtable_foreach(struct qb_map *map, qb_transverse_func func, void* user_data)
{
	struct hash_table *hash_table = (struct hash_table *)map;
	struct qb_list_head *list;
	struct qb_list_head *next;
	struct hash_node *hash_node;
	int32_t i;

	for (i = 0; i < hash_table->hash_buckets_len; i++) {
		for (list = hash_table->hash_buckets[i].list_head.next;
		     list != &hash_table->hash_buckets[i].list_head;
		     list = next) {

			next = list->next;

			hash_node = qb_list_entry(list, struct hash_node, list);
			if (func(hash_node->key, hash_node->value, user_data)) {
				return;
			}
		}
	}
}

static void
hashtable_destroy(struct qb_map *map)
{
	struct hash_table *hash_table = (struct hash_table *)map;

	free(hash_table);
}

qb_map_t *
qb_hashtable_create(qb_compare_func key_compare_func,
		   void *key_compare_data,
		   qb_destroy_notifier_func key_destroy_func,
		   qb_destroy_notifier_func value_destroy_func,
		   size_t max_size, qb_hash_func hash_fn)
{
	int32_t i;
	int32_t order;
	int32_t n = max_size;
	uint64_t size;
	struct hash_table *ht;

	for (i = 0; n; i++) {
		n >>= 1;
	}
	order = QB_MAX(i, 3);

	size = sizeof(struct hash_table) +
	    (sizeof(struct hash_bucket) * (1 << order));

	ht = calloc(1, size);

	ht->map.key_compare_func = key_compare_func;
	ht->map.key_compare_data = key_compare_data;
	ht->map.key_destroy_func = key_destroy_func;
	ht->map.value_destroy_func = value_destroy_func;

	ht->map.put = hashtable_put;
	ht->map.get = hashtable_get;
	ht->map.rm = hashtable_rm;
	ht->map.count_get = hashtable_count_get;
	ht->map.foreach = hashtable_foreach;
	ht->map.destroy = hashtable_destroy;

	ht->count = 0;
	ht->order = order;

	if (hash_fn == NULL) {
		ht->hash_fn = qb_hash_pointer;
	} else {
		ht->hash_fn = hash_fn;
	}

	ht->hash_buckets_len = 1 << order;
	for (i = 0; i < ht->hash_buckets_len; i++) {
		qb_list_init(&ht->hash_buckets[i].list_head);
	}
	return (qb_map_t *) ht;
}

#if 0
int32_t qb_hash_key_context_get(qb_handle_t handle,
				const char *key, void **context)
{
	struct hash_table *hash_table;
	int32_t res = 0;

	qb_hdb_handle_get(&qb_hash_handle_db, handle, (void *)&hash_table);
	qb_hdb_handle_put(&qb_hash_handle_db, handle);
	return (res);
}
#endif

