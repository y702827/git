#include "git-compat-util.h"
#include "strmap.h"

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

void strmap_init(struct strmap *map)
{
	hashmap_init(&map->map, cmp_str_entry, NULL, 0);
}

void strmap_clear(struct strmap *map, int free_util)
{
	struct hashmap_iter iter;
	struct str_entry *e;

	if (!map)
		return;

	hashmap_for_each_entry(&map->map, &iter, e, ent /* member name */) {
		free(e->item.string);
		if (free_util)
			free(e->item.util);
	}
	hashmap_free_entries(&map->map, struct str_entry, ent);
}

/*
 * Insert "str" into the map, pointing to "data". A copy of "str" is made, so
 * it does not need to persist after the this function is called.
 *
 * If an entry for "str" already exists, its data pointer is overwritten, and
 * the original data pointer returned. Otherwise, returns NULL.
 */
void *strmap_put(struct strmap *map, const char *str, void *data)
{
	struct str_entry *entry;
	entry = xmalloc(sizeof(*entry));
	hashmap_entry_init(&entry->ent, strhash(str));
	entry->item.string = strdup(str);
	entry->item.util = data;
	return hashmap_put(&map->map, &entry->ent);
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
	if (ret && free_util)
		free(ret->item.util);
}
