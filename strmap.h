#ifndef STRMAP_H
#define STRMAP_H

#include "hashmap.h"
#include "string-list.h"

struct mempool;
struct strmap {
	struct hashmap map;
	struct mem_pool *pool;
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
 * Same as strmap_init, but also set the strmap to use a mempool for
 * allocating str_entry structs.  (Note that the hashmap never allocates
 * the data fields passed to strmap_put(), so those will not be affected
 * by the mempool; just the allocation of str_entry fields, and any
 * strdup calls if strdup_strings is true.)
 */
void strmap_init_with_mem_pool(struct strmap *map,
			       struct mem_pool *pool,
			       int strdup_strings);

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
 * strintmap:
 *    A map of string -> int, typecasting the void* of strmap to an int.
 *
 * Primary differences:
 *    1) Since the void* value is just an int in disguise, there is no value
 *       to free.  (Thus one fewer argument to strintmap_free & strintmap_clear)
 *    2) strintmap_get() returns an int; it also requires an extra parameter to
 *       be specified so it knows what value to return if the underlying strmap
 *       has not key matching the given string.
 *    3) No strmap_put() equivalent; strintmap_set() and strintmap_incr()
 *       instead.
 */

struct strintmap {
	struct strmap map;
};

#define strintmap_for_each_entry(mystrmap, iter, var)	\
	strmap_for_each_entry(&(mystrmap)->map, iter, var)
#if 0
	for (var = hashmap_iter_first_entry_offset(&(mystrmap)->map.map, iter, \
						   OFFSETOF_VAR(var, ent)); \
		var; \
		var = hashmap_iter_next_entry_offset(iter, \
						OFFSETOF_VAR(var, ent)))
#endif

static inline void strintmap_init(struct strintmap *map, int strdup_strings)
{
	strmap_init(&map->map, strdup_strings);
}

static inline void strintmap_init_with_mem_pool(struct strintmap *map,
						struct mem_pool *pool,
						int strdup_strings)
{
	strmap_init_with_mem_pool(&map->map, pool, strdup_strings);
}

static inline void strintmap_free(struct strintmap *map)
{
	strmap_free(&map->map, 0);
}

static inline void strintmap_clear(struct strintmap *map)
{
	strmap_clear(&map->map, 0);
}

static inline int strintmap_contains(struct strintmap *map, const char *str)
{
	return strmap_contains(&map->map, str);
}

static inline void strintmap_remove(struct strintmap *map, const char *str)
{
	return strmap_remove(&map->map, str, 0);
}

static inline int strintmap_empty(struct strintmap *map)
{
	return strmap_empty(&map->map);
}

static inline unsigned int strintmap_get_size(struct strintmap *map)
{
	return strmap_get_size(&map->map);
}

static inline int strintmap_get(struct strintmap *map, const char *str,
				int default_value)
{
	struct string_list_item *result = strmap_get_item(&map->map, str);
	if (!result)
		return default_value;
	return (intptr_t)result->util;
}

static inline void strintmap_set(struct strintmap *map, const char *str, intptr_t v)
{
	strmap_put(&map->map, str, (void *)v);
}

void strintmap_incr(struct strintmap *map, const char *str, intptr_t amt);

/*
 * strset:
 *    A set of strings.
 *
 * Primary differences with strmap:
 *    1) The value is always NULL, and ignored.  As there is no value to free,
 *       there is one fewer argument to strset_free & strset_clear
 *    2) No strset_get() because there is no value.
 *    3) No strset_put(); used strset_add() instead.
 */

struct strset {
	struct strmap map;
};

#define strset_for_each_entry(mystrset, iter, var)	\
	strmap_for_each_entry(&(mystrset)->map, iter, var)

static inline void strset_init(struct strset *set, int strdup_strings)
{
	strmap_init(&set->map, strdup_strings);
}

static inline void strset_init_with_mem_pool(struct strset *set,
						struct mem_pool *pool,
						int strdup_strings)
{
	strmap_init_with_mem_pool(&set->map, pool, strdup_strings);
}

static inline void strset_free(struct strset *set)
{
	strmap_free(&set->map, 0);
}

static inline void strset_clear(struct strset *set)
{
	strmap_clear(&set->map, 0);
}

static inline int strset_contains(struct strset *set, const char *str)
{
	return strmap_contains(&set->map, str);
}

static inline void strset_remove(struct strset *set, const char *str)
{
	return strmap_remove(&set->map, str, 0);
}

static inline int strset_empty(struct strset *set)
{
	return strmap_empty(&set->map);
}

static inline unsigned int strset_get_size(struct strset *set)
{
	return strmap_get_size(&set->map);
}

static inline void strset_add(struct strset *set, const char *str)
{
	strmap_put(&set->map, str, NULL);
}

#endif /* STRMAP_H */
