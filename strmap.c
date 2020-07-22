#include "git-compat-util.h"
#include "strmap.h"
#include "mem-pool.h"

static int cmp_str_entry(const void *hashmap_cmp_fn_data,
			 const struct hashmap_entry *entry1,
			 const struct hashmap_entry *entry2,
			 const void *keydata)
{
	const struct str_entry *e1, *e2;

	e1 = container_of(entry1, const struct str_entry, ent);
	e2 = container_of(entry2, const struct str_entry, ent);
	return strcmp(e1->item.string, e2->item.string);
}

static struct str_entry *find_str_entry(struct strmap *map,
					const char *str)
{
	struct str_entry entry;
	hashmap_entry_init(&entry.ent, strhash(str));
	entry.item.string = (char *)str;
	return hashmap_get_entry(&map->map, &entry, ent, NULL);
}

void strmap_init(struct strmap *map, int strdup_strings)
{
	hashmap_init(&map->map, cmp_str_entry, NULL, 0);
	map->pool = NULL;
	map->strdup_strings = strdup_strings;
}

void strmap_init_with_mem_pool(struct strmap *map,
			       struct mem_pool *pool,
			       int strdup_strings)
{
	strmap_init(map, strdup_strings);
	map->pool = pool;
}

static void strmap_free_entries_(struct strmap *map, int free_util)
{
	struct hashmap_iter iter;
	struct str_entry *e;

	if (!map)
		return;

	if (!free_util && map->pool)
		/* Memory other than util is owned by and freed with the pool */
		return;

	/*
	 * We need to iterate over the hashmap entries and free
	 * e->item.string and e->item.util ourselves; hashmap has no API to
	 * take care of that for us.  Since we're already iterating over
	 * the hashmap, though, might as well free e too and avoid the need
	 * to make some call into the hashmap API to do that.
	 */
	hashmap_for_each_entry(&map->map, &iter, e, ent) {
		if (free_util)
			free(e->item.util);
		if (!map->pool) {
			if (map->strdup_strings)
				free(e->item.string);
			free(e);
		}
	}
}

void strmap_free(struct strmap *map, int free_util)
{
	strmap_free_entries_(map, free_util);
	hashmap_free(&map->map);
}

void strmap_clear(struct strmap *map, int free_util)
{
	strmap_free_entries_(map, free_util);
	hashmap_clear(&map->map);
}

/*
 * Insert "str" into the map, pointing to "data".
 *
 * If an entry for "str" already exists, its data pointer is overwritten, and
 * the original data pointer returned. Otherwise, returns NULL.
 */
void *strmap_put(struct strmap *map, const char *str, void *data)
{
	struct str_entry *entry = find_str_entry(map, str);
	void *old = NULL;

	if (entry) {
		old = entry->item.util;
		entry->item.util = data;
	} else {
		/*
		 * We won't modify entry->item.string so it really should be
		 * const, but changing string_list_item to use a const char *
		 * is a bit too big of a change at this point.
		 */
		char *key = (char *)str;

		entry = map->pool ? mem_pool_alloc(map->pool, sizeof(*entry))
				  : xmalloc(sizeof(*entry));
		hashmap_entry_init(&entry->ent, strhash(str));

		if (map->strdup_strings)
			key = map->pool ? mem_pool_strdup(map->pool, str)
					: xstrdup(str);
		entry->item.string = key;
		entry->item.util = data;
		hashmap_add(&map->map, &entry->ent);
	}
	return old;
}

struct string_list_item *strmap_get_item(struct strmap *map,
					 const char *str)
{
	struct str_entry *entry = find_str_entry(map, str);
	return entry ? &entry->item : NULL;
}

void *strmap_get(struct strmap *map, const char *str)
{
	struct str_entry *entry = find_str_entry(map, str);
	return entry ? entry->item.util : NULL;
}

int strmap_contains(struct strmap *map, const char *str)
{
	return find_str_entry(map, str) != NULL;
}

void strmap_remove(struct strmap *map, const char *str, int free_util)
{
	struct str_entry entry, *ret;
	hashmap_entry_init(&entry.ent, strhash(str));
	entry.item.string = (char *)str;
	ret = hashmap_remove_entry(&map->map, &entry, ent, NULL);
	if (!ret)
		return;
	if (free_util)
		free(ret->item.util);
	if (!map->pool) {
		if (map->strdup_strings)
			free(ret->item.string);
		free(ret);
	}
}

void strintmap_incr(struct strmap *map, const char *str, intptr_t amt)
{
	struct str_entry *entry = find_str_entry(map, str);
	if (entry) {
		intptr_t *whence = (intptr_t*)&entry->item.util;
		*whence += amt;
	}
	else
		strintmap_set(map, str, amt);
}
