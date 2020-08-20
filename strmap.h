#ifndef STRMAP_H
#define STRMAP_H

#include "hashmap.h"

struct strmap {
	struct hashmap map;
	unsigned int strdup_strings:1;
};

struct strmap_entry {
	struct hashmap_entry ent;
	const char *key;
	void *value;
};

/*
 * Initialize the members of the strmap.  Any keys added to the strmap will
 * be strdup'ed with their memory managed by the strmap.
 */
void strmap_init(struct strmap *map);

/*
 * Same as strmap_init, but for those who want to control the memory management
 * carefully instead of using the default of strdup_strings=1.
 * (OCD = Obsessive Compulsive Disorder, a joke that those who use this function
 * are obsessing over minor details.)
 */
void strmap_ocd_init(struct strmap *map,
		     int strdup_strings);

/*
 * Remove all entries from the map, releasing any allocated resources.
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
 * Return the strmap_entry mapped by "str", or NULL if there is not such
 * an item in map.
 */
struct strmap_entry *strmap_get_entry(struct strmap *map, const char *str);

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
 * iterate through @map using @iter, @var is a pointer to a type strmap_entry
 */
#define strmap_for_each_entry(mystrmap, iter, var)	\
	for (var = hashmap_iter_first_entry_offset(&(mystrmap)->map, iter, \
						   OFFSETOF_VAR(var, ent)); \
		var; \
		var = hashmap_iter_next_entry_offset(iter, \
						     OFFSETOF_VAR(var, ent)))

#endif /* STRMAP_H */
