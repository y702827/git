/*
 *
 * Copyright (C) 2005 Junio C Hamano
 */
#include "cache.h"
#include "diff.h"
#include "diffcore.h"
#include "object-store.h"
#include "hashmap.h"
#include "progress.h"
#include "promisor-remote.h"
#include "strmap.h"

#if 0
#define VERBOSE_DEBUG
#endif
#if 0
#define SECTION_LABEL
#endif

/* Table of rename/copy destinations */

static struct diff_rename_dst {
	struct diff_filespec *two;
	struct diff_filepair *pair;
} *rename_dst;
static int rename_dst_nr, rename_dst_alloc;

static int find_rename_dst(struct diff_filespec *two)
{
	int first, last;

	first = 0;
	last = rename_dst_nr;
	while (last > first) {
		int next = first + ((last - first) >> 1);
		struct diff_rename_dst *dst = &(rename_dst[next]);
		int cmp = strcmp(two->path, dst->two->path);
		if (!cmp)
			return next;
		if (cmp < 0) {
			last = next;
			continue;
		}
		first = next+1;
	}
	return -first - 1;
}

static struct diff_rename_dst *locate_rename_dst(struct diff_filespec *two)
{
	int ofs = find_rename_dst(two);
	return ofs < 0 ? NULL : &rename_dst[ofs];
}

/*
 * Returns 0 on success, -1 if we found a duplicate.
 */
static int add_rename_dst(struct diff_filespec *two)
{
	int first = find_rename_dst(two);

	if (first >= 0)
		return -1;
	first = -first - 1;

	/* insert to make it at "first" */
	ALLOC_GROW(rename_dst, rename_dst_nr + 1, rename_dst_alloc);
	rename_dst_nr++;
	if (first < rename_dst_nr)
		MOVE_ARRAY(rename_dst + first + 1, rename_dst + first,
			   rename_dst_nr - first - 1);
	rename_dst[first].two = alloc_filespec(two->path);
	fill_filespec(rename_dst[first].two, &two->oid, two->oid_valid,
		      two->mode);
	rename_dst[first].pair = NULL;
	return 0;
}

/* Table of rename/copy src files */
static struct diff_rename_src {
	struct diff_filepair *p;
	unsigned short score; /* to remember the break score */
} *rename_src;
static int rename_src_nr, rename_src_alloc;

static struct diff_rename_src *register_rename_src(struct diff_filepair *p)
{
	int first, last;
	struct diff_filespec *one = p->one;
	unsigned short score = p->score;

	first = 0;
	last = rename_src_nr;
	while (last > first) {
		int next = first + ((last - first) >> 1);
		struct diff_rename_src *src = &(rename_src[next]);
		int cmp = strcmp(one->path, src->p->one->path);
		if (!cmp)
			return src;
		if (cmp < 0) {
			last = next;
			continue;
		}
		first = next+1;
	}

	/* insert to make it at "first" */
	ALLOC_GROW(rename_src, rename_src_nr + 1, rename_src_alloc);
	rename_src_nr++;
	if (first < rename_src_nr)
		MOVE_ARRAY(rename_src + first + 1, rename_src + first,
			   rename_src_nr - first - 1);
	rename_src[first].p = p;
	rename_src[first].score = score;
	return &(rename_src[first]);
}

static int basename_same(struct diff_filespec *src, struct diff_filespec *dst)
{
	int src_len = strlen(src->path), dst_len = strlen(dst->path);
	while (src_len && dst_len) {
		char c1 = src->path[--src_len];
		char c2 = dst->path[--dst_len];
		if (c1 != c2)
			return 0;
		if (c1 == '/')
			return 1;
	}
	return (!src_len || src->path[src_len - 1] == '/') &&
		(!dst_len || dst->path[dst_len - 1] == '/');
}

struct diff_score {
	int src; /* index in rename_src */
	int dst; /* index in rename_dst */
	unsigned short score;
	short name_score;
};

struct prefetch_options {
	struct repository *repo;
	int skip_unmodified;
};
static void prefetch(void *prefetch_options)
{
	struct prefetch_options *options = prefetch_options;
	int i;
	struct oid_array to_fetch = OID_ARRAY_INIT;

	for (i = 0; i < rename_dst_nr; i++) {
		if (rename_dst[i].pair)
			/*
			 * The loop in diffcore_rename() will not need these
			 * blobs, so skip prefetching.
			 */
			continue; /* already found exact match */
		diff_add_if_missing(options->repo, &to_fetch,
				    rename_dst[i].two);
	}
	for (i = 0; i < rename_src_nr; i++) {
		if (options->skip_unmodified &&
		    diff_unmodified_pair(rename_src[i].p))
			/*
			 * The loop in diffcore_rename() will not need these
			 * blobs, so skip prefetching.
			 */
			continue;
		diff_add_if_missing(options->repo, &to_fetch,
				    rename_src[i].p->one);
	}
	promisor_remote_get_direct(options->repo, to_fetch.oid, to_fetch.nr);
	oid_array_clear(&to_fetch);
}

static int estimate_similarity(struct repository *r,
			       struct diff_filespec *src,
			       struct diff_filespec *dst,
			       int minimum_score,
			       int skip_unmodified)
{
	/* src points at a file that existed in the original tree (or
	 * optionally a file in the destination tree) and dst points
	 * at a newly created file.  They may be quite similar, in which
	 * case we want to say src is renamed to dst or src is copied into
	 * dst, and then some edit has been applied to dst.
	 *
	 * Compare them and return how similar they are, representing
	 * the score as an integer between 0 and MAX_SCORE.
	 *
	 * When there is an exact match, it is considered a better
	 * match than anything else; the destination does not even
	 * call into this function in that case.
	 */
	unsigned long max_size, delta_size, base_size, src_copied, literal_added;
	int score;
	struct diff_populate_filespec_options dpf_options = {
		.check_size_only = 1
	};
	struct prefetch_options prefetch_options = {r, skip_unmodified};

	if (r == the_repository && has_promisor_remote()) {
		dpf_options.missing_object_cb = prefetch;
		dpf_options.missing_object_data = &prefetch_options;
	}

	/*
	 * We deal only with regular files.  Symlink renames are handled
	 * only when they are exact matches --- in other words, no edits
	 * after renaming.  Similarly, submodule renames would require
	 * a bunch of additional comparison logic (or might be better
	 * done through just looking at changes to the toplevel
	 * .gitmodules file and comparing submodule.*.url fields).
	 */
	if (!S_ISREG(src->mode) || !S_ISREG(dst->mode))
		return 0;

	/*
	 * Need to check that source and destination sizes are
	 * filled in before comparing them.
	 *
	 * If we already have "cnt_data" filled in, we know it's
	 * all good (avoid checking the size for zero, as that
	 * is a possible size - we really should have a flag to
	 * say whether the size is valid or not!)
	 */
	if (!src->cnt_data &&
	    diff_populate_filespec(r, src, &dpf_options))
		return 0;
	if (!dst->cnt_data &&
	    diff_populate_filespec(r, dst, &dpf_options))
		return 0;

	max_size = ((src->size > dst->size) ? src->size : dst->size);
	base_size = ((src->size < dst->size) ? src->size : dst->size);
	delta_size = max_size - base_size;

	/* We would not consider edits that change the file size so
	 * drastically.  delta_size must be smaller than
	 * (MAX_SCORE-minimum_score)/MAX_SCORE * min(src->size, dst->size).
	 *
	 * Note that base_size == 0 case is handled here already
	 * and the final score computation below would not have a
	 * divide-by-zero issue.
	 */
	if (max_size * (MAX_SCORE-minimum_score) < delta_size * MAX_SCORE)
		return 0;

	dpf_options.check_size_only = 0;

	if (!src->cnt_data && diff_populate_filespec(r, src, &dpf_options))
		return 0;
	if (!dst->cnt_data && diff_populate_filespec(r, dst, &dpf_options))
		return 0;

	if (diffcore_count_changes(r, src, dst,
				   &src->cnt_data, &dst->cnt_data,
				   &src_copied, &literal_added))
		return 0;

	/* How similar are they?
	 * what percentage of material in dst are from source?
	 */
	if (!dst->size)
		score = 0; /* should not happen */
	else
		score = (int)(src_copied * MAX_SCORE / max_size);
	return score;
}

static void record_rename_pair(int dst_index, int src_index, int score)
{
	struct diff_filespec *src, *dst;
	struct diff_filepair *dp;

	if (rename_dst[dst_index].pair)
		die("internal error: dst already matched.");

	src = rename_src[src_index].p->one;
	src->rename_used++;
	src->count++;

	dst = rename_dst[dst_index].two;
	dst->count++;

	dp = diff_queue(NULL, src, dst);
	dp->renamed_pair = 1;
	if (!strcmp(src->path, dst->path))
		dp->score = rename_src[src_index].score;
	else
		dp->score = score;
	rename_dst[dst_index].pair = dp;
}

/*
 * We sort the rename similarity matrix with the score, in descending
 * order (the most similar first).
 */
static int score_compare(const void *a_, const void *b_)
{
	const struct diff_score *a = a_, *b = b_;

	/* sink the unused ones to the bottom */
	if (a->dst < 0)
		return (0 <= b->dst);
	else if (b->dst < 0)
		return -1;

	if (a->score == b->score)
		return b->name_score - a->name_score;

	return b->score - a->score;
}

struct file_similarity {
	struct hashmap_entry entry;
	int index;
	struct diff_filespec *filespec;
};

static unsigned int hash_filespec(struct repository *r,
				  struct diff_filespec *filespec)
{
	if (!filespec->oid_valid) {
		if (diff_populate_filespec(r, filespec, NULL))
			return 0;
		hash_object_file(r->hash_algo, filespec->data, filespec->size,
				 "blob", &filespec->oid);
	}
	return oidhash(&filespec->oid);
}

static int find_identical_files(struct hashmap *srcs,
				int dst_index,
				struct diff_options *options)
{
	int renames = 0;
	struct diff_filespec *target = rename_dst[dst_index].two;
	struct file_similarity *p, *best = NULL;
	int i = 100, best_score = -1;
	unsigned int hash = hash_filespec(options->repo, target);

	/*
	 * Find the best source match for specified destination.
	 */
	p = hashmap_get_entry_from_hash(srcs, hash, NULL,
					struct file_similarity, entry);
	hashmap_for_each_entry_from(srcs, p, entry) {
		int score;
		struct diff_filespec *source = p->filespec;

		/* False hash collision? */
		if (!oideq(&source->oid, &target->oid))
			continue;
		/* Non-regular files? If so, the modes must match! */
		if (!S_ISREG(source->mode) || !S_ISREG(target->mode)) {
			if (source->mode != target->mode)
				continue;
		}
		/* Give higher scores to sources that haven't been used already */
		score = !source->rename_used;
		if (source->rename_used && options->detect_rename != DIFF_DETECT_COPY)
			continue;
		score += basename_same(source, target);
		if (score > best_score) {
			best = p;
			best_score = score;
			if (score == 2)
				break;
		}

		/* Too many identical alternatives? Pick one */
		if (!--i)
			break;
	}
	if (best) {
#ifdef VERBOSE_DEBUG
		printf("  Exact rename: %s -> %s\n",
		       rename_src[best->index].p->one->path, target->path);
#endif
		record_rename_pair(dst_index, best->index, MAX_SCORE);
		renames++;
	}
	return renames;
}

static void insert_file_table(struct repository *r,
			      struct hashmap *table, int index,
			      struct diff_filespec *filespec)
{
	struct file_similarity *entry = xmalloc(sizeof(*entry));

	entry->index = index;
	entry->filespec = filespec;

	hashmap_entry_init(&entry->entry, hash_filespec(r, filespec));
	hashmap_add(table, &entry->entry);
}

/*
 * Find exact renames first.
 *
 * The first round matches up the up-to-date entries,
 * and then during the second round we try to match
 * cache-dirty entries as well.
 */
static int find_exact_renames(struct diff_options *options)
{
	int i, renames = 0;
	struct hashmap file_table;

	/* Add all sources to the hash table in reverse order, because
	 * later on they will be retrieved in LIFO order.
	 */
	hashmap_init(&file_table, NULL, NULL, rename_src_nr);
	for (i = rename_src_nr-1; i >= 0; i--)
		insert_file_table(options->repo,
				  &file_table, i,
				  rename_src[i].p->one);

	/* Walk the destinations and find best source match */
	for (i = 0; i < rename_dst_nr; i++)
		renames += find_identical_files(&file_table, i, options);

	/* Free the hash data structure and entries */
	hashmap_free_entries(&file_table, struct file_similarity, entry);

	return renames;
}

struct rename_guess_info {
	struct strmap idx_map;
	struct strmap dir_rename;
	int initialized;
};

static char *get_dirname(char *filename)
{
	char *slash = strrchr(filename, '/');
	return slash ? xstrndup(filename, slash-filename) : xstrdup("");
}

static char *get_highest_rename_path(struct strmap *counts) {
	int highest_count = 0;
	char *highest_target_dir = NULL;
	struct hashmap_iter iter;
	struct str_entry *entry;

	strmap_for_each_entry(counts, &iter, entry) {
		char *target_dir = entry->item.string;
		intptr_t count = (intptr_t)entry->item.util;
		if (count > highest_count) {
			highest_count = count;
			highest_target_dir = target_dir;
		}
	}
	return highest_target_dir;
}

static void initialize_rename_guess_info(struct rename_guess_info *info,
					 struct strmap *dirs_removed)
{
	/*
	 * NOTE: This function relies on rename_dst being sorted, but that
	 * was guaranteed by add_rename_dst().
	 *
	 * FIXME: Actually, it relies on all entries within the same
	 *        immediate directory being adjacent to each other, which
	 *        sorted order does not give us.  e.g.:
	 *             foo.txt
	 *             foo/bar
	 *             fooey
	 */
	char *prev_old_dir = NULL, *old_dir, *new_dir;
	char *best_newdir;
	struct strmap counts;
	int i;

	info->initialized = 1;
	strmap_init(&info->idx_map, 0);
	strmap_init(&info->dir_rename, 0);

	strmap_init(&counts, 1);
	for (i = 0; i < rename_dst_nr; ++i) {
		char *filename = rename_dst[i].two->path;
		char *oldname;
		int prev_count;

		if (!rename_dst[i].pair) {
			strintmap_set(&info->idx_map, filename, i);
			continue;
		}

		/* FIXME: Remove this assert once I verify it's sane. */
		assert(rename_dst[i].pair->two->path ==
		       rename_dst[i].two->path);

		oldname = rename_dst[i].pair->one->path;
		old_dir = get_dirname(oldname);
		if (!strmap_contains(dirs_removed, old_dir)) {
			free(old_dir);
			continue;
		}

		new_dir = get_dirname(filename);

		if (!prev_old_dir) {
			prev_old_dir = xstrdup(old_dir);
		} else if (strcmp(old_dir, prev_old_dir)) {
			best_newdir = get_highest_rename_path(&counts);
			strmap_put(&info->dir_rename, prev_old_dir,
				   xstrdup(best_newdir));
			strintmap_clear(&counts);

			prev_old_dir = xstrdup(old_dir);
		} /* else leave prev_old_dir alone */

		prev_count = strintmap_get(&counts, new_dir, 0);
		strintmap_set(&counts, new_dir, prev_count + 1);

		free(old_dir);
		free(new_dir);
	}

	best_newdir = get_highest_rename_path(&counts);
	strmap_put(&info->dir_rename, prev_old_dir, xstrdup(best_newdir));
	strintmap_clear(&counts);
}

static void cleanup_rename_guess_info(struct rename_guess_info *info)
{
	if (!info->initialized)
		return;
	/*
	 * The strings placed into dir_rename were not strdup()'ed an extra
	 * time, but they weren't used anywhere else and weren't free'd.  So,
	 * pretend we strdup()'ed them earlier so they'll be freed now.
	 */
	info->dir_rename.strdup_strings = 1;
	strmap_free(&info->dir_rename, 1);
	strintmap_free(&info->idx_map);
}

static int idx_possible_rename(char *filename,
			       struct rename_guess_info *info,
			       struct strmap *dirs_removed)
{
	char *old_dir, *new_dir, *new_path, *basename;
	int idx;

	if (!dirs_removed)
		return -1;
	if (!info->initialized)
		initialize_rename_guess_info(info, dirs_removed);

	old_dir = get_dirname(filename);
	new_dir = strmap_get(&info->dir_rename, old_dir);
	free(old_dir);
	if (!new_dir)
		return -1;

	basename = strrchr(filename, '/');
	basename = (basename ? basename+1 : filename);
	new_path = xstrfmt("%s/%s", new_dir, basename);

	idx = strintmap_get(&info->idx_map, new_path, -1);
	free(new_path);
	return idx;
}

static int find_basename_matches(struct diff_options *options,
				 int minimum_score,
				 int num_src,
				 struct strmap *relevant_sources,
				 struct strmap *relevant_targets,
				 struct strmap *dirs_removed)
{
	/*
	 * When I checked, over 76% of file renames in linux just moved
	 * files to a different directory but kept the same basename.  gcc
	 * did that with over 64% of renames, gecko did it with over 79%,
	 * and WebKit did it with over 89%.
	 *
	 * Therefore we can bypass the normal exhaustive NxM matrix
	 * comparison of similarities between all potential rename sources
	 * and targets by instead using file basename as a hint, checking
	 * for similarity between files with the same basename, and if we
	 * find a pair that are sufficiently similar, record the rename
	 * pair and exclude those two from the NxM matrix.
	 *
	 * This *might* cause us to find a less than optimal pairing (if
	 * there is another file that we are even more similar to but has a
	 * different basename).  Given the huge performance advantage
	 * basename matching provides, and given the frequency with which
	 * people use the same basename in real world projects, that's a
	 * trade-off we are willing to accept when doing just rename
	 * detection.  However, if someone wants copy detection that
	 * implies they are willing to spend more cycles to find
	 * similarities between files, so it may be less likely that this
	 * heuristic is wanted.
	 */

	int i, renames = 0;
	int skip_unmodified;
	struct strmap sources; //= STRMAP_INIT_NODUP;
	struct strmap dests; // = STRMAP_INIT_NODUP;
	struct rename_guess_info info;

	info.initialized = 0;
	if (dirs_removed && strmap_empty(dirs_removed))
		/* If we have no dirs_removed, make it easy to exit early. */
		dirs_removed = NULL;

	/*
	 * The prefeteching stuff wants to know if it can skip prefetching blobs
	 * that are unmodified.  unmodified blobs are only relevant when doing
	 * copy detection.  find_basename_matches() is only used when detecting
	 * renames, not when detecting copies, so it'll only be used when a file
	 * only existed in the source.  Since we already know that the file
	 * won't be unmodified, there's no point checking for it; that's just a
	 * waste of resources.  So set skip_unmodified to 0 so that
	 * estimate_similarity() and prefetch() won't waste resources checking
	 * for something we already know is false.
	 */
	skip_unmodified = 0;

	/* Create maps of basename -> fullname(s) for sources and dests */
	strmap_init(&sources, 0);
	strmap_init(&dests, 0);
	for (i = 0; i < num_src; ++i) {
		char *filename = rename_src[i].p->one->path;
		char *base;

		/* exact renames removed in remove_unneeded_paths_from_src() */
		assert(!rename_src[i].p->one->rename_used);

		base = strrchr(filename, '/');
		base = (base ? base+1 : filename);

		if (strmap_contains(&sources, base))
			strintmap_set(&sources, base, -1);
		else
			strintmap_set(&sources, base, i);
	}
	for (i = 0; i < rename_dst_nr; ++i) {
		char *filename = rename_dst[i].two->path;
		char *base;

		if (rename_dst[i].pair)
			continue; /* involved in exact match already. */

		base = strrchr(filename, '/');
		base = (base ? base+1 : filename);

		if (strmap_contains(&dests, base))
			strintmap_set(&dests, base, -1);
		else
			strintmap_set(&dests, base, i);
	}

	/* Now look for basename matchups and do similarity estimation */
	for (i = 0; i < num_src; ++i) {
		char *filename = rename_src[i].p->one->path;
		char *base = NULL;
		intptr_t src_index;
		intptr_t dst_index;

		base = strrchr(filename, '/');
		base = (base ? base+1 : filename);

		src_index = strintmap_get(&sources, base, -1);
		assert(src_index == -1 || src_index == i);

		if (strmap_contains(&dests, base)) {
			struct diff_filespec *one, *two;
			int score;

			dst_index = strintmap_get(&dests, base, -1);
			if (src_index == -1 || dst_index == -1) {
				src_index = i;
				dst_index = idx_possible_rename(filename,
								&info,
								dirs_removed);
			}
			if (dst_index == -1)
				continue;

			if (rename_dst[dst_index].pair)
				continue; /* already used previously */

			one = rename_src[src_index].p->one;
			two = rename_dst[dst_index].two;

			/* If we don't care about the source/target, skip it */
			if (relevant_sources &&
			    !strmap_contains(relevant_sources, one->path))
				continue;
			if (relevant_targets &&
			    !strmap_contains(relevant_targets, two->path))
				continue;

			/* Estimate the similarity */
			score = estimate_similarity(options->repo, one, two,
						    minimum_score, skip_unmodified);

			/* If sufficiently similar, record as rename pair */
			if (score < minimum_score)
				continue;
#ifdef VERBOSE_DEBUG
			printf("  Basename-matched rename: %s -> %s (%d)\n",
			       one->path, two->path, score);
#endif
			record_rename_pair(dst_index, src_index, score);
			renames++;

			/*
			 * Found a rename so don't need text anymore; if we
			 * didn't find a rename, the filespec_blob would get
			 * re-used when doing the matrix of comparisons.
			 */
			diff_free_filespec_blob(one);
			diff_free_filespec_blob(two);
		}
	}

	strintmap_free(&sources);
	strintmap_free(&dests);
	cleanup_rename_guess_info(&info);

	return renames;
}

#define NUM_CANDIDATE_PER_DST 4
static void record_if_better(struct diff_score m[], struct diff_score *o)
{
	int i, worst;

	/* find the worst one */
	worst = 0;
	for (i = 1; i < NUM_CANDIDATE_PER_DST; i++)
		if (score_compare(&m[i], &m[worst]) > 0)
			worst = i;

	/* is it better than the worst one? */
	if (score_compare(&m[worst], o) > 0)
		m[worst] = *o;
}

/*
 * Returns:
 * 0 if we are under the limit;
 * 1 if we need to disable inexact rename detection;
 * 2 if we would be under the limit if we were given -C instead of -C -C.
 */
static int too_many_rename_candidates(int num_targets, int num_sources,
				      struct diff_options *options)
{
	int rename_limit = options->rename_limit;
	int i, num_src;

	options->needed_rename_limit = 0;

	/*
	 * This basically does a test for the rename matrix not
	 * growing larger than a "rename_limit" square matrix, ie:
	 *
	 *    num_targets * num_sources > rename_limit * rename_limit
	 */
	if (rename_limit <= 0)
		rename_limit = 32767;
	if ((num_targets <= rename_limit || num_sources <= rename_limit) &&
	    ((uint64_t)num_targets * (uint64_t)num_sources
	     <= (uint64_t)rename_limit * (uint64_t)rename_limit))
		return 0;

	options->needed_rename_limit =
		num_sources > num_targets ? num_sources : num_targets;

	/* Are we running under -C -C? */
	if (!options->flags.find_copies_harder)
		return 1;

	/* Would we bust the limit if we were running under -C? */
	for (num_src = i = 0; i < num_sources; i++) {
		if (diff_unmodified_pair(rename_src[i].p))
			continue;
		num_src++;
	}
	if ((num_targets <= rename_limit || num_src <= rename_limit) &&
	    ((uint64_t)num_targets * (uint64_t)num_src
	     <= (uint64_t)rename_limit * (uint64_t)rename_limit))
		return 2;
	return 1;
}

static int find_renames(struct diff_score *mx, int dst_cnt, int minimum_score, int copies)
{
	int count = 0, i;

	for (i = 0; i < dst_cnt * NUM_CANDIDATE_PER_DST; i++) {
		struct diff_rename_dst *dst;

		if ((mx[i].dst < 0) ||
		    (mx[i].score < minimum_score))
			break; /* there is no more usable pair. */
		dst = &rename_dst[mx[i].dst];
		if (dst->pair)
			continue; /* already done, either exact or fuzzy. */
		if (!copies && rename_src[mx[i].src].p->one->rename_used)
			continue;
#ifdef VERBOSE_DEBUG
		printf("  Exhaustively-matched rename: %s -> %s (%d)\n",
		       rename_src[mx[i].src].p->one->path,
		       rename_dst[mx[i].dst].two->path,
		       mx[i].score);
#endif
		record_rename_pair(mx[i].dst, mx[i].src, mx[i].score);
		count++;
	}
	return count;
}

static void dump_unmatched(int num_src)
{
#ifdef VERBOSE_DEBUG
	int i;

	for (i = 0; i < num_src; ++i) {
		char *filename = rename_src[i].p->one->path;

		if (rename_src[i].p->one->rename_used)
			continue;

		printf("  Unmatched source: %s\n", filename);
	}
	for (i = 0; i < rename_dst_nr; ++i) {
		char *filename = rename_dst[i].two->path;

		if (rename_dst[i].pair)
			continue;

		printf("  Unmatched target: %s\n", filename);
	}
#endif
}

static int remove_unneeded_paths_from_src(int num_src,
					  int detecting_copies,
					  struct strmap *interesting)
{
	int i, new_num_src;

	if (detecting_copies && !interesting)
		return num_src;

	for (i = 0, new_num_src = 0; i < num_src; i++) {
		struct diff_filespec *one = rename_src[i].p->one;

		/*
		 * renames are stored in rename_dst, so if a rename has
		 * already been detected using this source, just remove it.
		 */
		if (!detecting_copies && one->rename_used)
			continue;

		/* If we don't care about the source path, skip it */
		if (interesting && !strmap_contains(interesting, one->path))
			continue;

		if (new_num_src < i)
			/* FIXME: Does this leak memory of what we overwrite? */
			memcpy(&rename_src[new_num_src], &rename_src[i],
			       sizeof(struct diff_rename_src));
		new_num_src++;
	}

	return new_num_src;
}

void diffcore_rename_extended(struct diff_options *options,
			      struct strmap *relevant_sources,
			      struct strmap *relevant_targets,
			      struct strmap *dirs_removed)
{
	int detect_rename = options->detect_rename;
	int minimum_score = options->rename_score;
	struct diff_queue_struct *q = &diff_queued_diff;
	struct diff_queue_struct outq;
	struct diff_score *mx;
	int i, j, exact_count, rename_count, skip_unmodified = 0;
	int num_create, dst_cnt, num_src, want_copies;
	struct progress *progress = NULL;

	trace2_region_enter("diff", "setup", options->repo);
	want_copies = (detect_rename == DIFF_DETECT_COPY);
	if (want_copies && dirs_removed)
		BUG("dirs_removed incompatible with copy detection");

	if (!minimum_score)
		minimum_score = DEFAULT_RENAME_SCORE;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		if (!DIFF_FILE_VALID(p->one)) {
			if (!DIFF_FILE_VALID(p->two))
				continue; /* unmerged */
			else if (options->single_follow &&
				 strcmp(options->single_follow, p->two->path))
				continue; /* not interested */
			else if (!options->flags.rename_empty &&
				 is_empty_blob_oid(&p->two->oid))
				continue;
			else if (add_rename_dst(p->two) < 0) {
				warning("skipping rename detection, detected"
					" duplicate destination '%s'",
					p->two->path);
				goto cleanup;
			}
		}
		else if (!options->flags.rename_empty &&
			 is_empty_blob_oid(&p->one->oid))
			continue;
		else if (!DIFF_PAIR_UNMERGED(p) && !DIFF_FILE_VALID(p->two)) {
			/*
			 * If the source is a broken "delete", and
			 * they did not really want to get broken,
			 * that means the source actually stays.
			 * So we increment the "rename_used" score
			 * by one, to indicate ourselves as a user
			 */
			if (p->broken_pair && !p->score)
				p->one->rename_used++;
			register_rename_src(p);
		}
		else if (detect_rename == DIFF_DETECT_COPY) {
			/*
			 * Increment the "rename_used" score by
			 * one, to indicate ourselves as a user.
			 */
			p->one->rename_used++;
			register_rename_src(p);
		}
	}
	trace2_region_leave("diff", "setup", options->repo);
	if (rename_dst_nr == 0 || rename_src_nr == 0)
		goto cleanup; /* nothing to do */

	trace2_region_enter("diff", "exact renames", options->repo);
	/*
	 * We really want to cull the candidates list early
	 * with cheap tests in order to avoid doing deltas.
	 */
#ifdef SECTION_LABEL
	printf("Looking for exact renames...\n");
#endif
	exact_count = rename_count = find_exact_renames(options);
#ifdef SECTION_LABEL
	printf("Done.\n");
#endif
	trace2_region_leave("diff", "exact renames", options->repo);

	/* Did we only want exact renames? */
	if (minimum_score == MAX_SCORE)
		goto cleanup;

	num_src = rename_src_nr;

	if (!want_copies) {
		/* Cull sources used in exact renames */
		trace2_region_enter("diff", "cull exact", options->repo);
		num_src = remove_unneeded_paths_from_src(num_src, want_copies,
							 NULL);
		trace2_region_leave("diff", "cull exact", options->repo);

#ifdef SECTION_LABEL
		printf("Looking for basename-based renames...\n");
#endif
		/* Cull the candidates list based on basename match. */
		trace2_region_enter("diff", "basename matches", options->repo);
		rename_count += find_basename_matches(options, minimum_score,
						      num_src, relevant_sources,
						      relevant_targets,
						      dirs_removed);
		trace2_region_leave("diff", "basename matches", options->repo);
#ifdef SECTION_LABEL
		printf("Done.\n");
#endif
	}

	/*
	 * Cull sources:
	 *   - remove ones already involved in renames if we don't want copies
	 *   - remove ones not found in relevant_sources (if it's not NULL)
	 */
	trace2_region_enter("diff", "cull basename", options->repo);
	num_src = remove_unneeded_paths_from_src(num_src, want_copies,
						 relevant_sources);
	trace2_region_leave("diff", "cull basename", options->repo);

	/*
	 * Calculate how many rename targets are left
	 */
	num_create = (rename_dst_nr - rename_count);

#if 0
	/* Debug spew */
	fflush(NULL);
	printf("\nRename stats:\n");
	printf("  Started with (%d x %d), %d relevant\n",
	       rename_src_nr, rename_dst_nr,
	       relevant_sources ? strmap_get_size(relevant_sources) : rename_src_nr);
	printf("  Found %d exact & %d basename\n", exact_count, rename_count - exact_count);
	printf("  Now have (%d x %d)\n", num_src, num_create);
	if (num_src > 0)
		printf("  Remaining sources:\n");
	for (i = 0; i < num_src; i++)
		printf("    %s\n", rename_src[i].p->one->path);
	fflush(NULL);

#else
	(void)exact_count;
#endif
	/* Avoid other code trying to use invalidated entries */
	rename_src_nr = num_src;
	
	/* All done? */
	if (!num_create || !num_src)
		goto cleanup;

	switch (too_many_rename_candidates(num_create, num_src, options)) {
	case 1:
		goto cleanup;
	case 2:
		options->degraded_cc_to_c = 1;
		skip_unmodified = 1;
		break;
	default:
		break;
	}

#ifdef SECTION_LABEL
	printf("Looking for inexact renames...\n");
#endif
	trace2_region_enter("diff", "inexact renames", options->repo);
	if (options->show_rename_progress) {
		progress = start_delayed_progress(
				_("Performing inexact rename detection"),
				(uint64_t)num_create * (uint64_t)num_src);
	}

	mx = xcalloc(st_mult(NUM_CANDIDATE_PER_DST, num_create), sizeof(*mx));
	for (dst_cnt = i = 0; i < rename_dst_nr; i++) {
		struct diff_filespec *two = rename_dst[i].two;
		struct diff_score *m;

		if (rename_dst[i].pair)
			continue; /* dealt with exact & basename match already */

		if (relevant_targets &&
		    !strmap_contains(relevant_targets, two->path))
			continue;

		m = &mx[dst_cnt * NUM_CANDIDATE_PER_DST];
		for (j = 0; j < NUM_CANDIDATE_PER_DST; j++)
			m[j].dst = -1;

		for (j = 0; j < num_src; j++) {
			struct diff_filespec *one = rename_src[j].p->one;
			struct diff_score this_src;

			assert(!one->rename_used ||
			       detect_rename == DIFF_DETECT_COPY);
			/*
			 * FIXME: Can remove the below if condition based on the
			 * assertion above.
			 */
			if (one->rename_used &&
			    detect_rename != DIFF_DETECT_COPY)
				continue;

			if (skip_unmodified &&
			    diff_unmodified_pair(rename_src[j].p))
				continue;

			this_src.score = estimate_similarity(options->repo,
							     one, two,
							     minimum_score,
							     skip_unmodified);
			this_src.name_score = basename_same(one, two);
			this_src.dst = i;
			this_src.src = j;
			record_if_better(m, &this_src);
			/*
			 * Once we run estimate_similarity,
			 * We do not need the text anymore.
			 */
			diff_free_filespec_blob(one);
			diff_free_filespec_blob(two);
		}
		dst_cnt++;
		display_progress(progress, (uint64_t)dst_cnt*(uint64_t)num_src);
	}
	stop_progress(&progress);

	/* cost matrix sorted by most to least similar pair */
	STABLE_QSORT(mx, dst_cnt * NUM_CANDIDATE_PER_DST, score_compare);

	rename_count += find_renames(mx, dst_cnt, minimum_score, 0);
	if (detect_rename == DIFF_DETECT_COPY)
		rename_count += find_renames(mx, dst_cnt, minimum_score, 1);
	free(mx);
#ifdef SECTION_LABEL
	printf("Done.\n");
#endif
	trace2_region_leave("diff", "inexact renames", options->repo);

	dump_unmatched(num_src);

 cleanup:
	/* At this point, we have found some renames and copies and they
	 * are recorded in rename_dst.  The original list is still in *q.
	 */
	trace2_region_enter("diff", "write back to queue", options->repo);
	DIFF_QUEUE_CLEAR(&outq);
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		struct diff_filepair *pair_to_free = NULL;

		if (DIFF_PAIR_UNMERGED(p)) {
			diff_q(&outq, p);
		}
		else if (!DIFF_FILE_VALID(p->one) && DIFF_FILE_VALID(p->two)) {
			/*
			 * Creation
			 *
			 * We would output this create record if it has
			 * not been turned into a rename/copy already.
			 */
			struct diff_rename_dst *dst = locate_rename_dst(p->two);
			if (dst && dst->pair) {
				diff_q(&outq, dst->pair);
				pair_to_free = p;
			}
			else
				/* no matching rename/copy source, so
				 * record this as a creation.
				 */
				diff_q(&outq, p);
		}
		else if (DIFF_FILE_VALID(p->one) && !DIFF_FILE_VALID(p->two)) {
			/*
			 * Deletion
			 *
			 * We would output this delete record if:
			 *
			 * (1) this is a broken delete and the counterpart
			 *     broken create remains in the output; or
			 * (2) this is not a broken delete, and rename_dst
			 *     does not have a rename/copy to move p->one->path
			 *     out of existence.
			 *
			 * Otherwise, the counterpart broken create
			 * has been turned into a rename-edit; or
			 * delete did not have a matching create to
			 * begin with.
			 */
			if (DIFF_PAIR_BROKEN(p)) {
				/* broken delete */
				struct diff_rename_dst *dst = locate_rename_dst(p->one);
				if (dst && dst->pair)
					/* counterpart is now rename/copy */
					pair_to_free = p;
			}
			else {
				if (p->one->rename_used)
					/* this path remains */
					pair_to_free = p;
			}

			if (pair_to_free)
				;
			else
				diff_q(&outq, p);
		}
		else if (!diff_unmodified_pair(p))
			/* all the usual ones need to be kept */
			diff_q(&outq, p);
		else
			/* no need to keep unmodified pairs */
			pair_to_free = p;

		if (pair_to_free)
			diff_free_filepair(pair_to_free);
	}
	diff_debug_queue("done copying original", &outq);

	free(q->queue);
	*q = outq;
	diff_debug_queue("done collapsing", q);

	for (i = 0; i < rename_dst_nr; i++)
		free_filespec(rename_dst[i].two);

	FREE_AND_NULL(rename_dst);
	rename_dst_nr = rename_dst_alloc = 0;
	FREE_AND_NULL(rename_src);
	rename_src_nr = rename_src_alloc = 0;
	trace2_region_leave("diff", "write back to queue", options->repo);
	return;
}

void diffcore_rename(struct diff_options *options)
{
	diffcore_rename_extended(options, NULL, NULL, NULL);
}
