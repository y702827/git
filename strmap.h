#ifndef STRMAP_H
#define STRMAP_H

#include "hashmap.h"

struct strmap {
	struct hashmap map;
};

#define STRMAP_INIT { { NULL } }

/*
 * Initialize an empty strmap (this is unnecessary if it was initialized with
 * STRMAP_INIT).
 */
void strmap_init(struct strmap *map);

/*
 * Remove all entries from the map, releasing any allocated resources.
 */
void strmap_clear(struct strmap *map, int free_values);

/*
 * Insert "str" into the map, pointing to "data". A copy of "str" is made, so
 * it does not need to persist after the this function is called.
 *
 * If an entry for "str" already exists, its data pointer is overwritten, and
 * the original data pointer returned. Otherwise, returns NULL.
 */
void *strmap_put(struct strmap *map, const char *str, void *data);

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
 * strmap, the list is not altered.
 */
void strmap_remove(struct strmap *map, const char *str, int free_value);

#endif /* STRMAP_H */
