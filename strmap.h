#ifndef STRMAP_H
#define STRMAP_H

#include "hashmap.h"
#include "string-list.h"

struct strmap {
	struct hashmap map;
	unsigned int strdup_strings:1;
};

struct str_entry {
	struct hashmap_entry ent;
	struct string_list_item item;
};

/*
 * Initialize the members of the strmap, set `strdup_strings`
 * member according to the value of the second parameter.
 */
void strmap_init(struct strmap *map, int strdup_strings);

/*
 * Remove all entries from the map, releasing any allocated resources.
 */
void strmap_free(struct strmap *map, int free_values);

/*
 * Similar to calling strmap_free() followed by strmap_init(), but slightly
 * faster since it doesn't deallocate the hashmap array and leaves it
 * pre-sized where it left off so that fewer rehashings are needed.
 */
void strmap_clear(struct strmap *map, int free_values);

/*
 * Insert "str" into the map, pointing to "data".
 *
 * If an entry for "str" already exists, its data pointer is overwritten, and
 * the original data pointer returned. Otherwise, returns NULL.
 */
void *strmap_put(struct strmap *map, const char *str, void *data);

/*
 * Return the string_list_item mapped by "str", or NULL if there is not such
 * an item in map.
 */
struct string_list_item *strmap_get_item(struct strmap *map, const char *str);

/*
 * Return the data pointer mapped by "str", or NULL if the entry does not
 * exist.
 */
void *strmap_get(struct strmap *map, const char *str);

/*
 * Return non-zero iff "str" is present in the map. This differs from
 * strmap_get() in that it can distinguish entries with a NULL data pointer.
 */
int strmap_contains(struct strmap *map, const char *str);

/*
 * Remove the given entry from the strmap.  If the string isn't in the
 * strmap, the map is not altered.
 */
void strmap_remove(struct strmap *map, const char *str, int free_value);

/*
 * Return whether the strmap is empty.
 */
static inline int strmap_empty(struct strmap *map)
{
	return hashmap_get_size(&map->map) == 0;
}

/*
 * Return how many entries the strmap has.
 */
static inline unsigned int strmap_get_size(struct strmap *map)
{
	return hashmap_get_size(&map->map);
}

/*
 * iterate through @map using @iter, @var is a pointer to a type str_entry
 */
#define strmap_for_each_entry(mystrmap, iter, var)	\
	for (var = hashmap_iter_first_entry_offset(&(mystrmap)->map, iter, \
						   OFFSETOF_VAR(var, ent)); \
		var; \
		var = hashmap_iter_next_entry_offset(iter, \
						OFFSETOF_VAR(var, ent)))

/*
 * Helper functions for using strmap as map of string -> int, using the void*
 * field to store the int instead of allocating an int and having the void*
 * member point to the allocated int.
 */

static inline int strintmap_get(struct strmap *map, const char *str,
				int default_value)
{
	struct string_list_item *result = strmap_get_item(map, str);
	if (!result)
		return default_value;
	return (intptr_t)result->util;
}

static inline void strintmap_set(struct strmap *map, const char *str, intptr_t v)
{
	strmap_put(map, str, (void *)v);
}

void strintmap_incr(struct strmap *map, const char *str, intptr_t amt);

static inline void strintmap_clear(struct strmap *map)
{
	strmap_clear(map, 0);
}

static inline void strintmap_free(struct strmap *map)
{
	strmap_free(map, 0);
}

#endif /* STRMAP_H */
