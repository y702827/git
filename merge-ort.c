/*
 * "Ostensibly Recursive's Twin" merge strategy, or "ort" for short.  Meant
 * as a drop-in replacement for the "recursive" merge strategy, allowing one
 * to replace
 *
 *   git merge [-s recursive]
 *
 * with
 *
 *   git merge -s ort
 *
 * Note: git's parser allows the space between '-s' and its argument to be
 * missing.  (Should I have backronymed "ham", "alsa", "kip", "nap, "alvo",
 * "cale", "peedy", or "ins" instead of "ort"?)
 */

#include "cache.h"
#include "merge-ort.h"

#include "alloc.h"
#include "attr.h"
#include "blob.h"
#include "cache-tree.h"
#include "commit.h"
#include "commit-reach.h"
#include "diff.h"
#include "diffcore.h"
#include "dir.h"
#include "ll-merge.h"
#include "object-store.h"
#include "revision.h"
#include "strmap.h"
#include "submodule.h"
#include "trace2.h"
#include "unpack-trees.h"
#include "xdiff-interface.h"

#define USE_MEMORY_POOL 1 /* faster, but obscures memory leak hunting */

#if 0
#define VERBOSE_DEBUG
#endif

enum relevance {
	RELEVANT_CONTENT = 1,
	RELEVANT_LOCATION = 2,
	RELEVANT_BOTH = 3
};

struct rename_info {
	/* For the next six vars, the 0th entry is ignored and unused */
	struct diff_queue_struct pairs[3]; /* input to & output from diffcore_rename */
	struct strintmap relevant_sources[3];  /* filepath => enum relevance */
	struct strintmap dirs_removed[3];      /* directory => bool */
	struct strintmap possible_trivial_merges[3]; /* dirname->dir_rename_mask */
	struct strset target_dirs[3];             /* set of directory paths */
	unsigned trivial_merges_okay[3];          /* 0 = no, 1 = maybe */
	/*
	 * dir_rename_mask:
	 *   0: optimization removing unmodified potential rename source okay
	 *   2 or 4: optimization okay, but must check for files added to dir
	 *   7: optimization forbidden; need rename source in case of dir rename
	 */
	unsigned dir_rename_mask:3;

	/*
	 * dir_rename_mask needs to be coupled with a traversal through trees
	 * that iterates over all files in a given tree before all immediate
	 * subdirectories within that tree.  Since traverse_trees() doesn't do
	 * that naturally, we have a traverse_trees_wrapper() that stores any
	 * immediate subdirectories while traversing files, then traverses the
	 * immediate subdirectories later.
	 */
	struct traversal_callback_data *callback_data;
	int callback_data_nr, callback_data_alloc;
	char *callback_data_traverse_path;

	/*
	 * When doing repeated merges, we can re-use renaming information from
	 * previous merges under special circumstances;
	 */
	struct tree *merge_trees[3];
	int cached_pairs_valid_side;
	struct strmap cached_pairs[3];   /* fullnames -> {rename_path or NULL} */
	struct strset cached_target_names[3]; /* set of target fullnames */
	/*
	 * And sometimes it pays to detect renames, and then restart the merge
	 * with the renames cached so that we can do trivial tree merging.
	 * Values: 0 = don't bother, 1 = let's do it, 2 = we already did it.
	 */
	unsigned redo_after_renames;
};

struct traversal_callback_data {
	unsigned long mask;
	unsigned long dirmask;
	struct name_entry names[3];
};

struct merge_options_internal {
	struct strmap paths;    /* maps path -> (merged|conflict)_info */
	struct strmap unmerged; /* maps path -> conflict_info */
#if USE_MEMORY_POOL
	struct mem_pool pool;
#else
	struct string_list paths_to_free; /* list of strings to free */
#endif
	struct rename_info *renames;
	struct index_state attr_index; /* renormalization weirdly needs one... */
	struct strmap output;  /* maps path -> conflict messages */
	const char *current_dir_name;
	char *toplevel_dir; /* see merge_info.directory_name comment */
	int call_depth;
	int needed_rename_limit;
};

struct version_info {
	struct object_id oid;
	unsigned short mode;
};

struct merged_info {
	struct version_info result;
	unsigned is_null:1;
	unsigned clean:1;
	size_t basename_offset;
	 /*
	  * Containing directory name.  Note that we assume directory_name is
	  * constructed such that
	  *    strcmp(dir1_name, dir2_name) == 0 iff dir1_name == dir2_name,
	  * i.e. string equality is equivalent to pointer equality.  For this
	  * to hold, we have to be careful setting directory_name.
	  */
	const char *directory_name;
};

struct conflict_info {
	struct merged_info merged;
	struct version_info stages[3];
	const char *pathnames[3];
	unsigned df_conflict:1;
	unsigned path_conflict:1;
	unsigned filemask:3;
	unsigned dirmask:3;
	unsigned match_mask:3;
	unsigned processed:1;
};

struct oidmap_string_list_entry {
	struct oidmap_entry entry;
	struct string_list fullpaths;
};

/***** Copy-paste static functions from merge-recursive.c *****/

/*
 * Yeah, I know this is kind of odd.  But due to my goals:
 *   1) Minimize churn to merge-recursive.c
 *   2) Eventually just delete merge-recursive.c
 * I decided to just copy-paste these for now.  Once we're ready to switch
 * over, we can
 *   #define merge_recursive merge_ort
 *   #define merge_trees merge_ort_nonrecursive
 * and delete merge-recursive.c.
 */

static struct tree *shift_tree_object(struct repository *repo,
				      struct tree *one, struct tree *two,
				      const char *subtree_shift)
{
	struct object_id shifted;

	if (!*subtree_shift) {
		shift_tree(repo, &one->object.oid, &two->object.oid, &shifted, 0);
	} else {
		shift_tree_by(repo, &one->object.oid, &two->object.oid, &shifted,
			      subtree_shift);
	}
	if (oideq(&two->object.oid, &shifted))
		return two;
	return lookup_tree(repo, &shifted);
}

static inline void set_commit_tree(struct commit *c, struct tree *t)
{
	c->maybe_tree = t;
}

static struct commit *make_virtual_commit(struct repository *repo,
					  struct tree *tree,
					  const char *comment)
{
	struct commit *commit = alloc_commit_node(repo);

	set_merge_remote_desc(commit, comment, (struct object *)commit);
	set_commit_tree(commit, tree);
	commit->object.parsed = 1;
	return commit;
}

static int err(struct merge_options *opt, const char *err, ...)
{
	va_list params;
	struct strbuf sb = STRBUF_INIT;

	strbuf_addf(&sb, "error: ");
	va_start(params, err);
	strbuf_vaddf(&sb, err, params);
	va_end(params);

	error("%s", sb.buf);
	strbuf_release(&sb);

	return -1;
}

static void format_commit(struct strbuf *sb,
			  int indent,
			  struct commit *commit)
{
	struct merge_remote_desc *desc;
	struct pretty_print_context ctx = {0};
	ctx.abbrev = DEFAULT_ABBREV;

	strbuf_addchars(sb, ' ', indent);
	desc = merge_remote_util(commit);
	if (desc) {
		strbuf_addf(sb, "virtual %s\n", desc->name);
		return;
	}

	format_commit_message(commit, "%h %s", sb, &ctx);
	strbuf_addch(sb, '\n');
}

__attribute__((format (printf, 4, 5)))
static void path_msg(struct merge_options *opt,
		     const char *path,
		     int is_hint, /* hints can be omitted */
		     const char *fmt, ...)
{
	va_list ap;
	struct strbuf *sb = strmap_get(&opt->priv->output, path);
	if (!sb) {
		sb = xmalloc(sizeof(*sb));
		strbuf_init(sb, 0);
		strmap_put(&opt->priv->output, path, sb);
	}

	va_start(ap, fmt);
	strbuf_vaddf(sb, fmt, ap);
	va_end(ap);

	strbuf_addch(sb, '\n');
}

static inline int merge_detect_rename(struct merge_options *opt)
{
#ifdef SOMEONE_HAS_A_REASON_OTHER_THAN_PERF_FOR_TURNING_OFF_RENAME_DETECTION
	/*
	 * We do not have logic to handle the detection of copies.  In
	 * fact, it may not even make sense to add such logic: would we
	 * really want a change to a base file to be propagated through
	 * multiple other files by a merge?
	 */
	return !!opt->detect_renames;
#else
	return DIFF_DETECT_RENAME;
#endif
}

static struct commit_list *reverse_commit_list(struct commit_list *list)
{
	struct commit_list *next = NULL, *current, *backup;
	for (current = list; current; current = backup) {
		backup = current->next;
		current->next = next;
		next = current;
	}
	return next;
}

static int sort_dirs_next_to_their_children(const void *a, const void *b)
{
	/*
	 * Here we only care that entries for directories appear adjacent
	 * to and before files underneath the directory.  In other words,
	 * we do not want the natural sorting of
	 *     foo
	 *     foo.txt
	 *     foo/bar
	 * Instead, we want "foo" to sort as though it were "foo/", so that
	 * we instead get
	 *     foo.txt
	 *     foo
	 *     foo/bar
	 * To achieve this, we basically implement our own strcmp, except that
	 * if we get to the end of either string instead of comparing NUL to
	 * another character, we compare '/' to it.
	 *
	 * The reason to not use df_name_compare directly was that it was
	 * just too bloody expensive, so I had to reimplement it.
	 */
	const char *one = ((struct string_list_item *)a)->string;
	const char *two = ((struct string_list_item *)b)->string;
	unsigned char c1, c2;

	while (*one && (*one == *two)) {
		one++;
		two++;
	}

	c1 = *one;
	if (!c1)
		c1 = '/';

	c2 = *two;
	if (!c2)
		c2 = '/';

	if (c1 == c2) {
		/* Getting here means one is a leading directory of the other */
		return (*one) ? 1 : -1;
	}
	else
		return c1-c2;
}

/***** End copy-paste static functions from merge-recursive.c *****/

static int traverse_trees_wrapper_callback(int n,
					   unsigned long mask,
					   unsigned long dirmask,
					   struct name_entry *names,
					   struct traverse_info *info)
{
	struct merge_options *opt = info->data;
	struct rename_info *renames = opt->priv->renames;
	unsigned filemask = mask & ~dirmask;

	assert(n==3);

	if (!renames->callback_data_traverse_path)
		renames->callback_data_traverse_path = xstrdup(info->traverse_path);

	if (filemask == renames->dir_rename_mask)
		renames->dir_rename_mask = 0x07;

	ALLOC_GROW(renames->callback_data, renames->callback_data_nr + 1,
		   renames->callback_data_alloc);
	renames->callback_data[renames->callback_data_nr].mask = mask;
	renames->callback_data[renames->callback_data_nr].dirmask = dirmask;
	memcpy(renames->callback_data[renames->callback_data_nr].names, names,
	       3*sizeof(*names));
	renames->callback_data_nr++;

	return mask;
}

/*
 * Much like traverse_trees(), BUT:
 *   - read all the tree entries FIRST
 *   - determine if any correspond to new entries in index 1 or 2
 *   - call my callback the way traverse_trees() would, but make sure that
 *     opt->priv->renames->dir_rename_mask is set based on new entries
 */
static int traverse_trees_wrapper(struct index_state *istate,
				  int n,
				  struct tree_desc *t,
				  struct traverse_info *info)
{
	int ret, i, old_offset;
	traverse_callback_t old_fn;
	char *old_callback_data_traverse_path;
	struct merge_options *opt = info->data;
	struct rename_info *renames = opt->priv->renames;

	assert(opt->priv->renames->dir_rename_mask == 2 ||
	       opt->priv->renames->dir_rename_mask == 4);

	old_callback_data_traverse_path = renames->callback_data_traverse_path;
	old_fn = info->fn;
	old_offset = renames->callback_data_nr;

	renames->callback_data_traverse_path = NULL;
	info->fn = traverse_trees_wrapper_callback;
	ret = traverse_trees(istate, n, t, info);
	if (ret < 0)
		return ret;

	info->traverse_path = renames->callback_data_traverse_path;
	info->fn = old_fn;
	for (i = old_offset; i < renames->callback_data_nr; ++i) {
		info->fn(n,
			 renames->callback_data[i].mask,
			 renames->callback_data[i].dirmask,
			 renames->callback_data[i].names,
			 info);

	}

	renames->callback_data_nr = old_offset;
	free(renames->callback_data_traverse_path);
	renames->callback_data_traverse_path = old_callback_data_traverse_path;
	info->traverse_path = NULL;
	return 0;
}

static void setup_path_info(struct merge_options *opt,
			    struct string_list_item *result,
			    const char *current_dir_name,
			    int current_dir_name_len,
			    char *fullpath, /* we'll take over ownership */
			    struct name_entry *names,
			    struct name_entry *merged_version,
			    unsigned is_null,     /* boolean */
			    unsigned df_conflict, /* boolean */
			    unsigned filemask,
			    unsigned dirmask,
			    int resolved          /* boolean */)
{
	struct conflict_info *path_info;

	assert(!is_null || resolved);
	assert(!df_conflict || !resolved); /* df_conflict implies !resolved */
	assert(resolved == (merged_version != NULL));

#if USE_MEMORY_POOL
	path_info = mem_pool_calloc(&opt->priv->pool, 1,
				    resolved ? sizeof(struct merged_info) :
					       sizeof(struct conflict_info));
#else
	path_info = xcalloc(1, resolved ? sizeof(struct merged_info) :
					  sizeof(struct conflict_info));
#endif
	path_info->merged.directory_name = current_dir_name;
	path_info->merged.basename_offset = current_dir_name_len;
	path_info->merged.clean = !!resolved;
	if (resolved) {
#ifdef VERBOSE_DEBUG
		printf("For %s, mode=%o, sha=%s, is_null=%d, clean=%d\n",
		       fullpath, merged_version->mode,
		       oid_to_hex(&merged_version->oid), !!is_null,
		       path_info->merged.clean);
#endif
		path_info->merged.result.mode = merged_version->mode;
		oidcpy(&path_info->merged.result.oid, &merged_version->oid);
		path_info->merged.is_null = !!is_null;
	} else {
		int i;

		for (i = 0; i < 3; i++) {
			path_info->pathnames[i] = fullpath;
			path_info->stages[i].mode = names[i].mode;
			oidcpy(&path_info->stages[i].oid, &names[i].oid);
		}
		path_info->filemask = filemask;
		path_info->dirmask = dirmask;
		path_info->df_conflict = !!df_conflict;
	}
	result->string = fullpath;
	result->util = path_info;
}

#if USE_MEMORY_POOL
static struct diff_filespec *mempool_alloc_filespec(struct mem_pool *pool,
						    const char *path)
{
	struct diff_filespec *spec;
	size_t len = strlen(path);

	spec = mem_pool_calloc(pool, 1, st_add3(sizeof(*spec), len, 1));
	memcpy(spec+1, path, len);
	spec->path = (void*)(spec+1);

	spec->count = 1;
	spec->is_binary = -1;
	return spec;
}

static struct diff_filepair *mempool_diff_queue(struct mem_pool *pool,
						struct diff_queue_struct *queue,
						struct diff_filespec *one,
						struct diff_filespec *two)
{
	struct diff_filepair *dp = mem_pool_calloc(pool, 1, sizeof(*dp));
	dp->one = one;
	dp->two = two;
	if (queue)
		diff_q(queue, dp);
	return dp;
}
#endif

static void add_pair(struct merge_options *opt,
		     struct name_entry *names,
		     const char *pathname,
		     unsigned side,
		     unsigned is_add /* if false, is_delete */,
		     unsigned dir_rename_mask)
{
	struct diff_filespec *one, *two;
#if USE_MEMORY_POOL
	struct mem_pool *pool = &opt->priv->pool;
#endif
	struct rename_info *renames = opt->priv->renames;
	struct strmap *cache = is_add ? &renames->cached_target_names[side].map :
					&renames->cached_pairs[side];
	int names_idx = is_add ? side : 0;

	if (!is_add) {
		strintmap_set(&renames->relevant_sources[side], pathname,
			      (dir_rename_mask == 0x07) ? RELEVANT_BOTH :
							  RELEVANT_CONTENT);
	}

	if (strmap_contains(cache, pathname))
		return;

#if USE_MEMORY_POOL
	one = mempool_alloc_filespec(pool, pathname);
	two = mempool_alloc_filespec(pool, pathname);
#else
	one = alloc_filespec(pathname);
	two = alloc_filespec(pathname);
#endif
	fill_filespec(is_add ? two : one,
		      &names[names_idx].oid, 1, names[names_idx].mode);
#if USE_MEMORY_POOL
	mempool_diff_queue(pool, &renames->pairs[side], one, two);
#else
	diff_queue(&renames->pairs[side], one, two);
#endif
}

static void collect_rename_info(struct merge_options *opt,
				struct name_entry *names,
				const char *dirname,
				const char *fullname,
				unsigned filemask,
				unsigned dirmask)
{
	struct rename_info *renames = opt->priv->renames;
	unsigned side;

	/*
	 * Update dir_rename_mask (determines ignore-rename-source validity)
	 *
	 * When a file has the same contents on one side of history as the
	 * merge base, and is missing on the other side of history, we can
	 * usually ignore detecting that rename (because there are no changes
	 * on the unrenamed side of history to merge with the changes present
	 * on the renamed side).  But if the file was part of a directory that
	 * has been moved, we still need the rename in order to detect the
	 * directory rename.
	 *
	 * This mask has complicated rules, based on how we can know whether a
	 * directory might be involved in a directory rename.  In particular:
	 *
	 *   - If dir_rename_mask is 0x07, we already determined elsewhere
	 *     that the ignore-rename-source optimization is unsafe for this
	 *     directory and any subdirectories.
	 *   - Directory has to exist in merge base to have been renamed
	 *     (i.e. dirmask & 1 must be true)
	 *   - Directory cannot exist on both sides or it isn't renamed
	 *     (i.e. !(dirmask & 2) or !(dirmask & 4) must be true)
	 *   - If directory exists in neither side1 nor side2, then
	 *     there are no new files to send along with the directory
	 *     rename so there's no point detecting it[1].  (Thus,
	 *     either dirmask & 2 or dirmask & 4 must be true)
	 *   - The above rules mean dirmask is either 3 or 5, as checked
	 *     above.
	 *
	 * [1] When neither side1 nor side2 has the directory then at
	 *     best, both sides renamed it to the same place (which
	 *     will be handled by all individual files being renamed
	 *     to the same place and no dir rename detection is
	 *     needed).  At worst, they both renamed it differently
	 *     (but all individual files are renamed to different
	 *     places which will flag errors so again no dir rename
	 *     detection is needed.)
	 */
	if (renames->dir_rename_mask != 0x07 && (dirmask == 3 || dirmask == 5)) {
		/* simple sanity check */
		assert(renames->dir_rename_mask == 0 ||
		       renames->dir_rename_mask == (dirmask & ~1));
		/* update dir_rename_mask */
		renames->dir_rename_mask = (dirmask & ~1);
	}

	/* Update dirs_removed, as needed */
	if (dirmask == 1 || dirmask == 3 || dirmask == 5) {
		/* absent_mask = 0x07 - dirmask; sides = absent_mask/2 */
		unsigned sides = (0x07 - dirmask)/2;
		unsigned drd = (renames->dir_rename_mask == 0x07);
		if (sides & 1)
			strintmap_set(&renames->dirs_removed[1], fullname, drd);
		if (sides & 2)
			strintmap_set(&renames->dirs_removed[2], fullname, drd);
		if (!strncmp(fullname, "modules/pg-peering/src/test/java/com/palantir/nexus/db/peering", 62)) {
			printf("For %s, sides = %d, drd (for now) = %d\n",
			       fullname, sides, drd);
		}
	}

	if (renames->dir_rename_mask == 0x07 &&
	    (filemask == 2 || filemask == 4)) {
		/*
		 * Need directory rename for parent directory on other side
		 * of history.  Thus side = (~filemask & 0x06) >> 1, or
		 * side = 3 - (filemask/2).
		 */
		unsigned side = 3 - (filemask >> 1);
		strintmap_set(&renames->dirs_removed[side], dirname, 2);
		printf("Set dirs_removed[%d][%s] = 2 because of %s\n",
		       side, dirname, fullname);
	}

	if (filemask == 0 || filemask == 7)
		return;

	for (side = 1; side <= 2; ++side) {
		unsigned side_mask = (1 << side);

		if ((filemask & 1) && !(filemask & side_mask)) {
			// fprintf(stderr, "Side %d deletion: %s\n", side, fullname);
			add_pair(opt, names, fullname, side, 0 /* delete */,
				 renames->dir_rename_mask);
		}

		if (!(filemask & 1) && (filemask & side_mask)) {
			// fprintf(stderr, "Side %d addition: %s\n", side, fullname);
			add_pair(opt, names, fullname, side, 1 /* add */,
				 renames->dir_rename_mask);
		}
	}
}

static int collect_merge_info_callback(int n,
				       unsigned long mask,
				       unsigned long dirmask,
				       struct name_entry *names,
				       struct traverse_info *info)
{
	/*
	 * n is 3.  Always.
	 * common ancestor (mbase) has mask 1, and stored in index 0 of names
	 * head of side 1  (side1) has mask 2, and stored in index 1 of names
	 * head of side 2  (side2) has mask 4, and stored in index 2 of names
	 */
	struct merge_options *opt = info->data;
	struct merge_options_internal *opti = opt->priv;
	struct rename_info *renames = opt->priv->renames;
	struct string_list_item pi;  /* Path Info */
	struct conflict_info *ci; /* pi.util when there's a conflict */
	struct name_entry *p;
	size_t len;
	char *fullpath;
	const char *dirname = opti->current_dir_name;
	unsigned prev_dir_rename_mask = renames->dir_rename_mask;
	unsigned filemask = mask & ~dirmask;
	unsigned mbase_null = !(mask & 1);
	unsigned side1_null = !(mask & 2);
	unsigned side2_null = !(mask & 4);
	unsigned side1_is_tree = (dirmask & 2);
	unsigned side2_is_tree = (dirmask & 4);
	unsigned side1_matches_mbase = (!side1_null && !mbase_null &&
					names[0].mode == names[1].mode &&
					oideq(&names[0].oid, &names[1].oid));
	unsigned side2_matches_mbase = (!side2_null && !mbase_null &&
					names[0].mode == names[2].mode &&
					oideq(&names[0].oid, &names[2].oid));
	unsigned sides_match = (!side1_null && !side2_null &&
				names[1].mode == names[2].mode &&
				oideq(&names[1].oid, &names[2].oid));
	/*
	 * Note: We only label files with df_conflict, not directories.
	 * Since directories stay where they are, and files move out of the
	 * way to make room for a directory, we don't care if there was a
	 * directory/file conflict for a parent directory of the current path.
	 */
	unsigned df_conflict = (filemask != 0) && (dirmask != 0);

#ifdef VERBOSE_DEBUG
	printf("Called collect_merge_info_callback on %s, %s\n",
	       info->traverse_path, names[0].path);
#endif

	/* n = 3 is a fundamental assumption. */
	if (n != 3)
		BUG("Called collect_merge_info_callback wrong");

	/*
	 * A bunch of sanity checks verifying that traverse_trees() calls
	 * us the way I expect.  Could just remove these at some point,
	 * though maybe they are helpful to future code readers.
	 */
	assert(mbase_null == is_null_oid(&names[0].oid));
	assert(side1_null == is_null_oid(&names[1].oid));
	assert(side2_null == is_null_oid(&names[2].oid));
	assert(!mbase_null || !side1_null || !side2_null);
	assert(mask > 0 && mask < 8);

	/* Other invariant checks, mostly for documentation purposes. */
	assert(mask == (dirmask | filemask));

	/*
	 * Get the name of the relevant filepath, which we'll pass to
	 * setup_path_info() for tracking.
	 */
	p = names;
	while (!p->mode)
		p++;
	len = traverse_path_len(info, p->pathlen);
	/* +1 in both of the following lines to include the NUL byte */
#if USE_MEMORY_POOL
	fullpath = mem_pool_alloc(&opt->priv->pool, len+1);
#else
	fullpath = xmalloc(len+1);
#endif
	make_traverse_path(fullpath, len+1, info, p->path, p->pathlen);

	/*
	 * If mbase, side1, and side2 all match, we can resolve early.  Even
	 * if these are trees, there will be no renames or anything
	 * underneath.
	 */
	if (side1_matches_mbase && side2_matches_mbase) {
		/* mbase, side1, & side2 all match; use mbase as resolution */
		setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
				names, names+0, mbase_null, 0,
				filemask, dirmask, 1);
#ifdef VERBOSE_DEBUG
		printf("Path -1 for %s\n", pi.string);
#endif
		strmap_put(&opti->paths, pi.string, pi.util);
		return mask;
	}

	/*
	 * If all three paths are files, then there will be no renames
	 * either for or under this path.  If additionally the sides match,
	 * we can take either as the resolution.
	 */
	if (filemask == 7 && sides_match) {
		/* use side1 (== side2) version as resolution */
		setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
				names, names+1, 0, 0, filemask, dirmask, 1);
#ifdef VERBOSE_DEBUG
		printf("Path 0 for %s\n", pi.string);
#endif
		strmap_put(&opti->paths, pi.string, pi.util);
		return mask;
	}

	/*
	 * Sometimes we can tell that a source path need not be included in
	 * rename detection (because it matches one of the two sides; see
	 * more below).  However, we call collect_rename_info() even in that
	 * case, because exact renames are cheap and would let us remove both
	 * a source and destination path.  We'll cull the unneeded sources
	 * later.
	 */
	collect_rename_info(opt, names, dirname, fullpath, filemask, dirmask);

	/*
	 * If side1 matches mbase, then we have some simplifications.  In
	 * particular, we can ignore mbase as a rename source because
	 *   - side1 has no interesting contents or changes (use side2 versions)
	 *   - side1 has no content changes to include in renames on side2 side
	 *   - side1 contains no new files to move with side2's directory renames
	 * Note that if side2 is a tree, there may be new files on side2's side
	 * that are rename targets that need to be merged with changes from
	 * elsewhere on side1's side of history.  Also, if side2 is a file
	 * (and side1 is a tree), the path on side2 is an add that may
	 * correspond to a rename target so we have to mark that as conflicted.
	 */
	if (side1_matches_mbase) {
		if (side2_null && filemask) {
			/*
			 * This path was in relevant_sources, but
			 * RELEVANT_CONTENT is no longer true.  Remove that
			 * flag for the path, and if that makes the path no
			 * longer relevant, remove it from relevant_sources.
			 */
#ifdef VERBOSE_DEBUG
			printf("Path 1.A for %s\n", names[0].path);
#endif
			if (renames->dir_rename_mask == 0x07)
				strintmap_set(&renames->relevant_sources[2],
					      fullpath, RELEVANT_LOCATION);
			else
				strintmap_remove(&renames->relevant_sources[2],
						 fullpath);
		} else if (!side1_is_tree && !side2_is_tree) {
			/* use side2 version as resolution */
			assert(filemask == 0x07);
			assert(dirmask == 0x0);
			setup_path_info(opt, &pi, dirname, info->pathlen,
					fullpath, names, names+2, side2_null, 0,
					filemask, dirmask, 1);
#ifdef VERBOSE_DEBUG
			printf("Path 1.C for %s\n", pi.string);
#endif
			strmap_put(&opti->paths, pi.string, pi.util);
			return mask;
		}
	}

	/*
	 * If side2 matches mbase, then we have some simplifications.  In
	 * particular, we can ignore mbase as a rename source.  Same
	 * reasoning as for above but with side1 and side2 swapped.
	 */
	if (side2_matches_mbase) {
		if (side1_null && filemask) {
			/*
			 * This path was in relevant_sources, but
			 * RELEVANT_CONTENT is no longer true.  Remove that
			 * flag for the path, and if that makes the path no
			 * longer relevant, remove it from relevant_sources.
			 */
#ifdef VERBOSE_DEBUG
			printf("Path 2.A for %s\n", names[0].path);
#endif
			if (renames->dir_rename_mask == 0x07)
				strintmap_set(&renames->relevant_sources[1],
					      fullpath, RELEVANT_LOCATION);
			else
				strintmap_remove(&renames->relevant_sources[1],
						 fullpath);
		} else if (!side1_is_tree && !side2_is_tree) {
			/* use side1 version as resolution */
			assert(filemask == 0x07);
			assert(dirmask == 0x0);
			setup_path_info(opt, &pi, dirname, info->pathlen,
					fullpath, names, names+1, side1_null, 0,
					filemask, dirmask, 1);
#ifdef VERBOSE_DEBUG
			printf("Path 2.C for %s\n", pi.string);
#endif
			strmap_put(&opti->paths, pi.string, pi.util);
			return mask;
		}
	}

	/*
	 * None of the special cases above matched, so we have a
	 * provisional conflict.  (Rename detection might allow us to
	 * unconflict some more cases, but that comes later so all we can
	 * do now is record the different non-null file hashes.)
	 */
	setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
			names, NULL, 0, df_conflict, filemask, dirmask, 0);
#ifdef VERBOSE_DEBUG
	printf("Path 3 for %s, iprd = %u\n", pi.string,
	       renames->dir_rename_mask);
	printf("Stats:\n");
#endif
	ci = pi.util;
	if (side1_matches_mbase)
		ci->match_mask = 3;
	else if (side2_matches_mbase)
		ci->match_mask = 5;
	else if (sides_match)
		ci->match_mask = 6;
	/* else ci->match_mask is already 0; no need to set it */
#ifdef VERBOSE_DEBUG
	printf("  matchmask: %u\n", ci->match_mask);
#endif
#ifdef VERBOSE_DEBUG
	printf("  renames->dir_rename_mask: %u\n",
	       renames->dir_rename_mask);
	printf("  side1_null: %d\n", side1_null);
	printf("  side2_null: %d\n", side2_null);
	printf("  side1_is_tree: %d\n", side1_is_tree);
	printf("  side2_is_tree: %d\n", side2_is_tree);
	printf("  side1_matches_mbase: %d\n", side1_matches_mbase);
	printf("  side2_matches_mbase: %d\n", side2_matches_mbase);
	printf("  filemask: %u\n", filemask);
	printf("  dirmask:  %lu\n", dirmask);
#endif
	strmap_put(&opti->paths, pi.string, pi.util);

	/* If dirmask, recurse into subdirectories */
	if (dirmask) {
		struct traverse_info newinfo;
		struct tree_desc t[3];
		void *buf[3] = {NULL,};
		const char *original_dir_name;
		int i, ret, side;

		/*
		 * Check for whether we can avoid recursing due to one side
		 * matching the merge base.  The side that does NOT match is
		 * the one that might have a rename target we need.
		 */
		assert(!side1_matches_mbase || !side2_matches_mbase);
		side = side1_matches_mbase ? 2 :
			side2_matches_mbase ? 1 : 0;
		if (filemask == 0 && (dirmask == 2 || dirmask == 4)) {
			/*
			 * Also defer recursing into new directories; set up a
			 * few variables to let us do so.
			 */
			ci->match_mask = (7 - dirmask);
			side = dirmask / 2;
		}
		if (renames->dir_rename_mask != 0x07 && side &&
		    renames->trivial_merges_okay[side] &&
		    !strset_contains(&renames->target_dirs[side], pi.string)) {
			strintmap_set(&renames->possible_trivial_merges[side],
				      pi.string, renames->dir_rename_mask);
			renames->dir_rename_mask = prev_dir_rename_mask;
			return mask;
		}

		/* We need to recurse */
		ci->match_mask &= filemask;
		newinfo = *info;
		newinfo.prev = info;
		newinfo.name = p->path;
		newinfo.namelen = p->pathlen;
		newinfo.pathlen = st_add3(newinfo.pathlen, p->pathlen, 1);
		/*
		 * If we did care about parent directories having a D/F
		 * conflict, then we'd include
		 *    newinfo.df_conflicts |= (mask & ~dirmask);
		 * here.  But we don't.  (See comment near setting of local
		 * df_conflict variable near the beginning of this function).
		 */

		for (i = 0; i < 3; i++, dirmask >>= 1) {
			if (i == 1 && side1_matches_mbase)
				t[1] = t[0];
			else if (i == 2 && side2_matches_mbase)
				t[2] = t[0];
			else if (i == 2 && sides_match)
				t[2] = t[1];
			else {
				const struct object_id *oid = NULL;
				if (dirmask & 1)
					oid = &names[i].oid;
				buf[i] = fill_tree_descriptor(opt->repo,
							      t + i, oid);
			}
		}

		original_dir_name = opti->current_dir_name;
		opti->current_dir_name = pi.string;
		if (renames->dir_rename_mask == 0 ||
		    renames->dir_rename_mask == 0x07)
			ret = traverse_trees(NULL, 3, t, &newinfo);
		else
			ret = traverse_trees_wrapper(NULL, 3, t, &newinfo);
		opti->current_dir_name = original_dir_name;
		renames->dir_rename_mask = prev_dir_rename_mask;

		for (i = 0; i < 3; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}

	return mask;
}

static void resolve_trivial_directory_merge(struct conflict_info *ci, int side)
{
	assert((side == 1 && ci->match_mask == 5) ||
	       (side == 2 && ci->match_mask == 3));
	oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
	ci->merged.result.mode = ci->stages[side].mode;
	ci->merged.is_null = is_null_oid(&ci->stages[side].oid);
	/* FIXME: Do I need to worry about the next 5 lines?? */
	/* ci->dirmask &= ~ci->match_mask; */
	/* ci->df_conflict = 0; */
	ci->match_mask = 0;
	ci->merged.clean = 1; /* (ci->filemask == 0); */
	ci->processed = 1; /* ci->merged.clean; */
}

static int handle_deferred_entries(struct merge_options *opt,
				   struct traverse_info *info)
{
	struct rename_info *renames = opt->priv->renames;
	struct hashmap_iter iter;
	struct str_entry *entry;
	int side, ret = 0;
	int path_count_before, path_count_after = 0;

	path_count_before = strmap_get_size(&opt->priv->paths);
	for (side = 1; side <= 2; side++) {
		unsigned optimization_okay = 1;
		struct strintmap copy;

		/* Loop over the set of paths we need to know rename info for */
		strset_for_each_entry(&renames->relevant_sources[side],
				      &iter, entry) {
			char *rename_target, *dir, *dir_marker;
			struct string_list_item *item;

			/*
			 * if we don't know delete/rename info for this path,
			 * then we need to recurse into all trees to get all
			 * adds to make sure we have it.
			 */
			item = strmap_get_item(&renames->cached_pairs[side],
					       entry->item.string);
			if (!item) {
				optimization_okay = 0;
				break;
			}

			/* If this is a delete, we have enough info already */
			rename_target = item->util;
			if (!rename_target)
				continue;

			/* If we already walked the rename target, we're good */
			if (strmap_contains(&opt->priv->paths, rename_target))
				continue;

			/*
			 * Otherwise, we need to get a list of directories that
			 * will need to be recursed into to get this
			 * rename_target.
			 */
			dir = xstrdup(rename_target);
			while ((dir_marker = strrchr(dir, '/'))) {
				*dir_marker = '\0';
				if (strset_contains(&renames->target_dirs[side],
						    dir))
					break;
				strset_add(&renames->target_dirs[side], dir);
			}
			free(dir);
		}
		renames->trivial_merges_okay[side] = optimization_okay;
		/*
		 * We need to recurse into any directories in
		 * possible_trivial_merges[side] found in target_dirs[side].
		 * But when we recurse, we may need to queue up some of the
		 * subdirectories for possible_trivial_merges[side].  Since
		 * we can't safely iterate through a hashmap while also adding
		 * entries, move the entries into 'copy', iterate over 'copy',
		 * and then we'll also iterate anything added into
		 * possible_trivial_merges[side] once this loop is done.
		 */
		copy = renames->possible_trivial_merges[side];
#if USE_MEMORY_POOL
		strintmap_init_with_mem_pool(&renames->possible_trivial_merges[side],
					     &opt->priv->pool, 0);
#else
		strintmap_init(&renames->possible_trivial_merges[side], 0);
#endif
		strintmap_for_each_entry(&copy, &iter, entry) {
			char *path = entry->item.string;
			unsigned dir_rename_mask = (intptr_t)entry->item.util;
			struct conflict_info *ci;
			unsigned dirmask;
			struct tree_desc t[3];
			void *buf[3] = {NULL,};
			int i;

			ci = strmap_get(&opt->priv->paths, path);
			dirmask = ci->dirmask;

			if (optimization_okay &&
			    !strset_contains(&renames->target_dirs[side],
					     path)) {
				resolve_trivial_directory_merge(ci, side);
				continue;
			}

			info->name = path;
			info->namelen = strlen(path);
			info->pathlen = info->namelen + 1;
#if 0
			printf("dirmask:    %d\n", ci->dirmask);
			printf("match_mask: %d\n", ci->match_mask);
			printf("oid[0]: %s\n", oid_to_hex(&ci->stages[0].oid));
			printf("oid[1]: %s\n", oid_to_hex(&ci->stages[1].oid));
			printf("oid[2]: %s\n", oid_to_hex(&ci->stages[2].oid));
			printf("info->prev: %p\n", info->prev);
#endif

			for (i = 0; i < 3; i++, dirmask >>= 1) {
				if (i == 1 && ci->match_mask == 3)
					t[1] = t[0];
				else if (i == 2 && ci->match_mask == 5)
					t[2] = t[0];
				else if (i == 2 && ci->match_mask == 6)
					t[2] = t[1];
				else {
					const struct object_id *oid = NULL;
					if (dirmask & 1)
						oid = &ci->stages[i].oid;
					buf[i] = fill_tree_descriptor(opt->repo,
								      t+i, oid);
				}
			}

			ci->match_mask &= ci->filemask;
			opt->priv->current_dir_name = path;
			renames->dir_rename_mask = dir_rename_mask;
			if (renames->dir_rename_mask == 0 ||
			    renames->dir_rename_mask == 0x07)
				ret = traverse_trees(NULL, 3, t, info);
			else
				ret = traverse_trees_wrapper(NULL, 3, t, info);

			for (i = 0; i < 3; i++)
				free(buf[i]);

			if (ret < 0)
				return ret;
		}
		strintmap_free(&copy);
		strintmap_for_each_entry(&renames->possible_trivial_merges[side],
				      &iter, entry) {
			char *path = entry->item.string;
			struct conflict_info *ci;

			ci = strmap_get(&opt->priv->paths, path);

			assert(renames->trivial_merges_okay[side] &&
			       !strset_contains(&renames->target_dirs[side],
						path));
			resolve_trivial_directory_merge(ci, side);
		}
		if (!optimization_okay || path_count_after)
			path_count_after = strmap_get_size(&opt->priv->paths);
	}
	if (path_count_after) {
		/*
		 * Not sure were the right cut-off is for the optimization
		 * to redo collect_merge_info after we've cached the
		 * regular renames is.  Basically, collect_merge_info(),
		 * detect_regular_renames(), and process_entries() are
		 * similar costs and all big tent poles.  Caching the result
		 * of detect_regular_renames() means that redoing that one
		 * function will cost us virtually 0 extra, so it depends on
		 * the other two functions, which are both O(N) cost in the
		 * number of paths.  Thus, it makes sense that if we can
		 * cut the number of paths in half, then redoing
		 * collect_merge_info() at half cost in order to get
		 * process_entries() at half cost should be about equal cost.
		 * If we can cut by more than half, then we would win.
		 * However, even when we have renames cached, we still have
		 * to traverse down to the individual renames, so the factor
		 * of two needs a little fudge.
		 *
		 * Error on the side of a bigger fudge, just because it's
		 * all an optimization; the code works even if we get
		 * wanted_factor wrong.  For the linux kernel testcases I
		 * was looking at, I saw factors of 50 to 250.  For such
		 * cases, this optimization provides *very* nice speedups.
		 */
		int wanted_factor = 10;

		/* We should only redo collect_merge_info one time */
		assert(renames->redo_after_renames == 0);

		if (path_count_after / path_count_before > wanted_factor) {
			renames->redo_after_renames = 1;
			renames->cached_pairs_valid_side = -1;
		}
	} else if (opt->priv->renames->redo_after_renames == 2)
		opt->priv->renames->redo_after_renames = 0;
	return ret;
}

static int collect_merge_info(struct merge_options *opt,
			      struct tree *merge_base,
			      struct tree *side1,
			      struct tree *side2)
{
	int ret;
	struct tree_desc t[3];
	struct traverse_info info;

	opt->priv->toplevel_dir = "";
	opt->priv->current_dir_name = opt->priv->toplevel_dir;
	setup_traverse_info(&info, opt->priv->toplevel_dir);
	info.fn = collect_merge_info_callback;
	info.data = opt;
	info.show_all_errors = 1;

	parse_tree(merge_base);
	parse_tree(side1);
	parse_tree(side2);
#ifdef VERBOSE_DEBUG
	printf("Traversing %s, %s, and %s\n",
	       oid_to_hex(&merge_base->object.oid),
	       oid_to_hex(&side1->object.oid),
	       oid_to_hex(&side2->object.oid));
#endif
	init_tree_desc(t+0, merge_base->buffer, merge_base->size);
	init_tree_desc(t+1, side1->buffer, side1->size);
	init_tree_desc(t+2, side2->buffer, side2->size);

	trace_performance_enter();
	ret = traverse_trees(NULL, 3, t, &info);
	if (ret == 0)
		ret = handle_deferred_entries(opt, &info);
	trace_performance_leave("traverse_trees");

	return ret;
}

/* add a string to a strbuf, but converting "/" to "_" */
static void add_flattened_path(struct strbuf *out, const char *s)
{
	size_t i = out->len;
	strbuf_addstr(out, s);
	for (; i < out->len; i++)
		if (out->buf[i] == '/')
			out->buf[i] = '_';
}

static char *unique_path(struct strmap *existing_paths,
			 const char *path,
			 const char *branch)
{
	struct strbuf newpath = STRBUF_INIT;
	int suffix = 0;
	size_t base_len;

	strbuf_addf(&newpath, "%s~", path);
	add_flattened_path(&newpath, branch);

	base_len = newpath.len;
	while (strmap_contains(existing_paths, newpath.buf)) {
		strbuf_setlen(&newpath, base_len);
		strbuf_addf(&newpath, "_%d", suffix++);
	}

	return strbuf_detach(&newpath, NULL);
}

static int find_first_merges(struct repository *repo,
			     const char *path,
			     struct commit *a,
			     struct commit *b,
			     struct object_array *result)
{
	int i, j;
	struct object_array merges = OBJECT_ARRAY_INIT;
	struct commit *commit;
	int contains_another;

	char merged_revision[GIT_MAX_HEXSZ + 2];
	const char *rev_args[] = { "rev-list", "--merges", "--ancestry-path",
				   "--all", merged_revision, NULL };
	struct rev_info revs;
	struct setup_revision_opt rev_opts;

	memset(result, 0, sizeof(struct object_array));
	memset(&rev_opts, 0, sizeof(rev_opts));

	/* get all revisions that merge commit a */
	xsnprintf(merged_revision, sizeof(merged_revision), "^%s",
		  oid_to_hex(&a->object.oid));
	repo_init_revisions(repo, &revs, NULL);
	rev_opts.submodule = path;
	/* FIXME: can't handle linked worktrees in submodules yet */
	revs.single_worktree = path != NULL;
	setup_revisions(ARRAY_SIZE(rev_args)-1, rev_args, &revs, &rev_opts);

	/* save all revisions from the above list that contain b */
	if (prepare_revision_walk(&revs))
		die("revision walk setup failed");
	while ((commit = get_revision(&revs)) != NULL) {
		struct object *o = &(commit->object);
		if (in_merge_bases(b, commit))
			add_object_array(o, NULL, &merges);
	}
	reset_revision_walk();

	/* Now we've got all merges that contain a and b. Prune all
	 * merges that contain another found merge and save them in
	 * result.
	 */
	for (i = 0; i < merges.nr; i++) {
		struct commit *m1 = (struct commit *) merges.objects[i].item;

		contains_another = 0;
		for (j = 0; j < merges.nr; j++) {
			struct commit *m2 = (struct commit *) merges.objects[j].item;
			if (i != j && in_merge_bases(m2, m1)) {
				contains_another = 1;
				break;
			}
		}

		if (!contains_another)
			add_object_array(merges.objects[i].item, NULL, result);
	}

	object_array_clear(&merges);
	return result->nr;
}

static int merge_submodule(struct merge_options *opt,
			   const char *path,
			   const struct object_id *o,
			   const struct object_id *a,
			   const struct object_id *b,
			   struct object_id *result)
{
	struct commit *commit_o, *commit_a, *commit_b;
	int parent_count;
	struct object_array merges;
	struct strbuf sb = STRBUF_INIT;

	int i;
	int search = !opt->priv->call_depth;

	/* store fallback answer in result in case we fail */
	oidcpy(result, opt->priv->call_depth ? o : a);

	/* we can not handle deletion conflicts */
	if (is_null_oid(o))
		return 0;
	if (is_null_oid(a))
		return 0;
	if (is_null_oid(b))
		return 0;

	if (add_submodule_odb(path)) {
		path_msg(opt, path, 0,
			 _("Failed to merge submodule %s (not checked out)"),
			 path);
		return 0;
	}

	if (!(commit_o = lookup_commit_reference(opt->repo, o)) ||
	    !(commit_a = lookup_commit_reference(opt->repo, a)) ||
	    !(commit_b = lookup_commit_reference(opt->repo, b))) {
		path_msg(opt, path, 0,
			 _("Failed to merge submodule %s (commits not present)"),
			 path);
		return 0;
	}

	/* check whether both changes are forward */
	if (!in_merge_bases(commit_o, commit_a) ||
	    !in_merge_bases(commit_o, commit_b)) {
		path_msg(opt, path, 0,
			 _("Failed to merge submodule %s "
			   "(commits don't follow merge-base)"),
			 path);
		return 0;
	}

	/* Case #1: a is contained in b or vice versa */
	if (in_merge_bases(commit_a, commit_b)) {
		oidcpy(result, b);
		path_msg(opt, path, 1,
			 _("Note: Fast-forwarding submodule %s to %s"),
			 path, oid_to_hex(b));
		return 1;
	}
	if (in_merge_bases(commit_b, commit_a)) {
		oidcpy(result, a);
		path_msg(opt, path, 1,
			 _("Note: Fast-forwarding submodule %s to %s"),
			 path, oid_to_hex(a));
		return 1;
	}

	/*
	 * Case #2: There are one or more merges that contain a and b in
	 * the submodule. If there is only one, then present it as a
	 * suggestion to the user, but leave it marked unmerged so the
	 * user needs to confirm the resolution.
	 */

	/* Skip the search if makes no sense to the calling context.  */
	if (!search)
		return 0;

	/* find commit which merges them */
	parent_count = find_first_merges(opt->repo, path, commit_a, commit_b,
					 &merges);
	switch (parent_count) {
	case 0:
		path_msg(opt, path, 0, _("Failed to merge submodule %s"), path);
		break;

	case 1:
		format_commit(&sb, 4,
			      (struct commit *)merges.objects[0].item);
		path_msg(opt, path, 0,
			 _("Failed to merge submodule %s, but a possible merge "
			   "resolution exists:\n%s\n"),
			 path, sb.buf);
		path_msg(opt, path, 1,
			 _("If this is correct simply add it to the index "
			   "for example\n"
			   "by using:\n\n"
			   "  git update-index --cacheinfo 160000 %s \"%s\"\n\n"
			   "which will accept this suggestion.\n"),
			 oid_to_hex(&merges.objects[0].item->oid), path);
		strbuf_release(&sb);
		break;
	default:
		for (i = 0; i < merges.nr; i++)
			format_commit(&sb, 4,
				      (struct commit *)merges.objects[i].item);
		path_msg(opt, path, 0,
			 _("Failed to merge submodule %s, but multiple "
			   "possible merges exist:\n%s"), path, sb.buf);
		strbuf_release(&sb);
	}

	object_array_clear(&merges);
	return 0;
}

static void initialize_attr_index(struct merge_options *opt)
{
	/*
	 * The renormalize_buffer() functions require attributes, and
	 * annoyingly those can only be read from the working tree or from
	 * an index_state.  merge-ort doesn't have an index_state, so we
	 * generate a fake one containing only attribute information.
	 */
	struct conflict_info *ci;
	struct index_state *attr_index = &opt->priv->attr_index;
	struct cache_entry *ce;

	if (!opt->renormalize)
		return;

	if (attr_index->initialized)
		return;
	attr_index->initialized = 1;

	ci = strmap_get(&opt->priv->paths, GITATTRIBUTES_FILE);
	if (!ci)
		return;

	if (ci->merged.clean) {
		int len = strlen(GITATTRIBUTES_FILE);
		ce = make_empty_cache_entry(attr_index, len);
		ce->ce_mode = create_ce_mode(ci->merged.result.mode);
		ce->ce_flags = create_ce_flags(0);
		ce->ce_namelen = len;
		oidcpy(&ce->oid, &ci->merged.result.oid);
		memcpy(ce->name, GITATTRIBUTES_FILE, len);
		add_index_entry(attr_index, ce,
				ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE);
		get_stream_filter(attr_index, GITATTRIBUTES_FILE, &ce->oid);
	}
	else {
		int stage, len;
		for (stage=0; stage<3; ++stage) {
			unsigned stage_mask = (1 << stage);

			if (!(ci->filemask & stage_mask))
				continue;
			len = strlen(GITATTRIBUTES_FILE);
			// make_empty_transient_cache_entry(len)
			ce = make_empty_cache_entry(attr_index, len);
			ce->ce_mode = create_ce_mode(ci->stages[stage].mode);
			ce->ce_flags = create_ce_flags(stage);
			ce->ce_namelen = len;
			oidcpy(&ce->oid, &ci->stages[stage].oid);
			memcpy(ce->name, GITATTRIBUTES_FILE, len);
			add_index_entry(attr_index, ce,
					ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE);
			get_stream_filter(attr_index, GITATTRIBUTES_FILE,
					  &ce->oid);
		}
	}
}

static int merge_3way(struct merge_options *opt,
		      const char *path,
		      const struct object_id *o,
		      const struct object_id *a,
		      const struct object_id *b,
		      const char *pathnames[3],
		      const int extra_marker_size,
		      mmbuffer_t *result_buf)
{
	mmfile_t orig, src1, src2;
	struct ll_merge_options ll_opts = {0};
	char *base, *name1, *name2;
	int merge_status;

	initialize_attr_index(opt);

	ll_opts.renormalize = opt->renormalize;
	ll_opts.extra_marker_size = extra_marker_size;
	ll_opts.xdl_opts = opt->xdl_opts;

	if (opt->priv->call_depth) {
		ll_opts.virtual_ancestor = 1;
		ll_opts.variant = 0;
	} else {
		switch (opt->recursive_variant) {
		case MERGE_VARIANT_OURS:
			ll_opts.variant = XDL_MERGE_FAVOR_OURS;
			break;
		case MERGE_VARIANT_THEIRS:
			ll_opts.variant = XDL_MERGE_FAVOR_THEIRS;
			break;
		default:
			ll_opts.variant = 0;
			break;
		}
	}

	assert(pathnames[0] && pathnames[1] && pathnames[2] && opt->ancestor);
	if (pathnames[0] == pathnames[1] && pathnames[1] == pathnames[2]) {
		base  = mkpathdup("%s", opt->ancestor);
		name1 = mkpathdup("%s", opt->branch1);
		name2 = mkpathdup("%s", opt->branch2);
	} else {
		base  = mkpathdup("%s:%s", opt->ancestor, pathnames[0]);
		name1 = mkpathdup("%s:%s", opt->branch1,  pathnames[1]);
		name2 = mkpathdup("%s:%s", opt->branch2,  pathnames[2]);
	}

	read_mmblob(&orig, o);
	read_mmblob(&src1, a);
	read_mmblob(&src2, b);

	merge_status = ll_merge(result_buf, path, &orig, base,
				&src1, name1, &src2, name2,
				&opt->priv->attr_index, &ll_opts);

	free(base);
	free(name1);
	free(name2);
	free(orig.ptr);
	free(src1.ptr);
	free(src2.ptr);
	return merge_status;
}

static int handle_content_merge(struct merge_options *opt,
				const char *path,
				const struct version_info *o,
				const struct version_info *a,
				const struct version_info *b,
				const char *pathnames[3],
				const int extra_marker_size,
				struct version_info *result)
{
	/*
	 * path is the target location where we want to put the file, and
	 * is used to determine any normalization rules in ll_merge.
	 *
	 * The normal case is that path and all entries in pathnames are
	 * identical, though renames can affect which path we got one of
	 * the three blobs to merge on various sides of history.
	 *
	 * extra_marker_size is the amount to extend conflict markers in
	 * ll_merge; this is neeed if we have content merges of content
	 * merges, which happens for example with rename/rename(2to1) and
	 * rename/add conflicts.
	 */
	unsigned clean = 1;

	/*
	 * handle_content_merge() needs both files to be of the same type, i.e.
	 * both files OR both submodules OR both symlinks.  Conflicting types
	 * needs to be handled elsewhere.
	 */
	assert((S_IFMT & a->mode) == (S_IFMT & b->mode));

	/* Merge modes */
	if (a->mode == b->mode || a->mode == o->mode)
		result->mode = b->mode;
	else {
		/* must be the 100644/100755 case */
		assert(S_ISREG(a->mode));
		result->mode = a->mode;
		clean = (b->mode == o->mode);
		/*
		 * FIXME: If opt->priv->call_depth && !clean, then we really
		 * should not make result->mode match either a->mode or
		 * b->mode; that causes t6036 "check conflicting mode for
		 * regular file" to fail.  It would be best to use some other
		 * mode, but we'll confuse all kinds of stuff if we use one
		 * where S_ISREG(result->mode) isn't true, and if we use
		 * something like 0100666, then tree-walk.c's calls to
		 * canon_mode() will just normalize that to 100644 for us and
		 * thus not solve anything.
		 *
		 * Not sure if there's anything we can do...
		 */
	}

	/*
	 * Trivial oid merge.
	 *
	 * Note: While one might assume that the next four lines would
	 * be unnecessary due to the fact that match_mask is often
	 * setup and already handled, renames don't always take care
	 * of that.
	 */
	if (oideq(&a->oid, &b->oid) || oideq(&a->oid, &o->oid))
		oidcpy(&result->oid, &b->oid);
	else if (oideq(&b->oid, &o->oid))
		oidcpy(&result->oid, &a->oid);

	/* Remaining rules depend on file vs. submodule vs. symlink. */
	else if (S_ISREG(a->mode)) {
		mmbuffer_t result_buf;
		int ret = 0, merge_status;
		int two_way;

		/*
		 * If 'o' is different type, treat it as null so we do a
		 * two-way merge.
		 */
		two_way = ((S_IFMT & o->mode) != (S_IFMT & a->mode));

		merge_status = merge_3way(opt, path,
					  two_way ? &null_oid : &o->oid,
					  &a->oid, &b->oid,
					  pathnames, extra_marker_size,
					  &result_buf);

		if ((merge_status < 0) || !result_buf.ptr)
			ret = err(opt, _("Failed to execute internal merge"));

		if (!ret &&
		    write_object_file(result_buf.ptr, result_buf.size,
				      blob_type, &result->oid))
			ret = err(opt, _("Unable to add %s to database"),
				  path);

		free(result_buf.ptr);
		if (ret)
			return -1;
		clean &= (merge_status == 0);
		path_msg(opt, path, 1, _("Auto-merging %s"), path);
	} else if (S_ISGITLINK(a->mode)) {
		int two_way = ((S_IFMT & o->mode) != (S_IFMT & a->mode));
		clean = merge_submodule(opt, pathnames[0],
					two_way ? &null_oid : &o->oid,
					&a->oid, &b->oid, &result->oid);
		if (opt->priv->call_depth && two_way && !clean) {
			result->mode = o->mode;
			oidcpy(&result->oid, &o->oid);
		}
	} else if (S_ISLNK(a->mode)) {
		if (opt->priv->call_depth) {
			clean = 0;
			result->mode = o->mode;
			oidcpy(&result->oid, &o->oid);
		} else {
			switch (opt->recursive_variant) {
			case MERGE_VARIANT_NORMAL:
				clean = 0;
				oidcpy(&result->oid, &a->oid);
				break;
			case MERGE_VARIANT_OURS:
				oidcpy(&result->oid, &a->oid);
				break;
			case MERGE_VARIANT_THEIRS:
				oidcpy(&result->oid, &b->oid);
				break;
			}
		}
	} else
		BUG("unsupported object type in the tree: %06o for %s",
		    a->mode, path);

	return clean;
}

static int process_renames(struct merge_options *opt,
			   struct diff_queue_struct *renames)
{
	int clean_merge = 1, i;

	for (i = 0; i < renames->nr; ++i) {
		const char *oldpath, *newpath;
		struct diff_filepair *pair = renames->queue[i];
		struct conflict_info *oldinfo, *newinfo;
		unsigned int old_sidemask;
		int target_index, other_source_index;
		int source_deleted, collision, type_changed;
		const char *rename_branch = NULL, *delete_branch = NULL;

		oldpath = pair->one->path;
		newpath = pair->two->path;
		oldinfo = strmap_get(&opt->priv->paths, pair->one->path);
		newinfo = strmap_get(&opt->priv->paths, pair->two->path);

		/*
		 * If oldpath isn't in opt->priv->paths, that means that a
		 * parent directory of oldpath was resolved and we don't
		 * even need the rename, so skip it.  If oldinfo->merged.clean,
		 * then the other side of history had no changes to oldpath
		 * and we don't need the rename and can skip it.
		 */
		if (!oldinfo || oldinfo->merged.clean)
			continue;

		if (i+1 < renames->nr &&
		    !strcmp(oldpath, renames->queue[i+1]->one->path)) {
			/* Handle rename/rename(1to2) or rename/rename(1to1) */
			const char *pathnames[3];
			struct version_info merged;
			struct conflict_info *base, *side1, *side2;
			unsigned was_binary_blob = 0;

			pathnames[0] = oldpath;
			pathnames[1] = newpath;
			pathnames[2] = renames->queue[i+1]->two->path;
			base = strmap_get(&opt->priv->paths, pathnames[0]);
			side1 = strmap_get(&opt->priv->paths, pathnames[1]);
			side2 = strmap_get(&opt->priv->paths, pathnames[2]);

			if (!strcmp(pathnames[1], pathnames[2])) {
				/* This is a rename/rename(1to1) */
				assert(side1 == side2);
				memcpy(&side1->stages[0], &base->stages[0],
				       sizeof(merged));
				side1->filemask |= (1 << 0);
				/* Mark base as resolved by removal */
				base->merged.is_null = 1;
				base->merged.clean = 1;

				/* This one is handled; move to next rename */
				continue;
			}

			/* This is a rename/rename(1to2) */
#ifdef VERBOSE_DEBUG
			printf("--> Rename/rename(1to2):\n");
			printf("      Paths: %s, %s, %s\n",
			       pathnames[0], pathnames[1], pathnames[2]);
			printf("      Copied merge into both sides stages\n");
			printf("      base: %s, %s, %s\n",
			       oid_to_hex(&base->stages[0].oid),
			       oid_to_hex(&base->stages[1].oid),
			       oid_to_hex(&base->stages[2].oid));
			printf("      side1: %s, %s, %s\n",
			       oid_to_hex(&side1->stages[0].oid),
			       oid_to_hex(&side1->stages[1].oid),
			       oid_to_hex(&side1->stages[2].oid));
			printf("      side2: %s, %s, %s\n",
			       oid_to_hex(&side2->stages[0].oid),
			       oid_to_hex(&side2->stages[1].oid),
			       oid_to_hex(&side2->stages[2].oid));
			printf("    pair->score: %d\n", pair->score);
			printf("    other->score: %d\n", renames->queue[i+1]->score);
#endif
			clean_merge = handle_content_merge(opt,
							   pair->one->path,
							   &base->stages[0],
							   &side1->stages[1],
							   &side2->stages[2],
							   pathnames,
							   1 + 2 * opt->priv->call_depth,
							   &merged);
			if (!clean_merge &&
			    merged.mode == side1->stages[1].mode &&
			    oideq(&merged.oid, &side1->stages[1].oid)) {
				was_binary_blob = 1;
			}
			memcpy(&side1->stages[1], &merged, sizeof(merged));
			if (was_binary_blob) {
				/*
				 * Getting here means we were attempting to
				 * merge a binary blob.
				 *
				 * Since we can't merge binaries,
				 * handle_content_merge() just takes one
				 * side.  But we don't want to copy the
				 * contents of one side to both paths.  We
				 * used the contents of side1 above for
				 * side1->stages, let's use the contents of
				 * side2 for side2->stages below.
				 */
				oidcpy(&merged.oid, &side2->stages[2].oid);
				merged.mode = side2->stages[2].mode;
			}
			memcpy(&side2->stages[2], &merged, sizeof(merged));

			side1->path_conflict = 1;
			side2->path_conflict = 1;
			/*
			 * TODO: For renames we normally remove the path at the
			 * old name.  It would thus seem consistent to do the
			 * same for rename/rename(1to2) cases, but we haven't
			 * done so traditionally and a number of the regression
			 * tests now encode an expectation that the file is
			 * left there at stage 1.  If we ever decide to change
			 * this, add the following two lines here:
			 *    base->merged.is_null = 1;
			 *    base->merged.clean = 1;
			 * and remove the setting of base->path_conflict to 1.
			 */
			base->path_conflict = 1;
			path_msg(opt, oldpath, 0,
				 _("CONFLICT (rename/rename): %s renamed to "
				   "%s in %s and to %s in %s."),
				 pathnames[0],
				 pathnames[1], opt->branch1,
				 pathnames[2], opt->branch2);

			i++; /* We handled both renames, i.e. i+1 handled */
			continue;
		}

		assert(oldinfo);
		assert(newinfo);
		assert(!oldinfo->merged.clean);
		assert(!newinfo->merged.clean);
		target_index = pair->score; /* from append_rename_pairs() */
		assert(target_index == 1 || target_index == 2);
		other_source_index = 3-target_index;
		old_sidemask = (1 << other_source_index); /* 2 or 4 */
		source_deleted = (oldinfo->filemask == 1);
		collision = ((newinfo->filemask & old_sidemask) != 0);
		type_changed = !source_deleted &&
			(S_ISREG(oldinfo->stages[other_source_index].mode) !=
			 S_ISREG(newinfo->stages[target_index].mode));
		if (type_changed) {
			/*
			 * if type_changed && collision are both true, then this
			 * was really a double rename, but one side wasn't
			 * detected due to lack of break detection.  I.e.
			 * something like
			 *    orig: has normal file 'foo'
			 *    side1: renames 'foo' to 'bar', adds 'foo' symlink
			 *    side2: renames 'foo' to 'bar'
			 * In this case, the foo->bar rename on side1 won't be
			 * detected because the new symlink named 'foo' is
			 * there and we don't do break detection.  But we detect
			 * this here because we don't want to merge the content
			 * of the foo symlink with the foo->bar file, so we
			 * have some logic to handle this special case.  The
			 * easiest way to do that is make 'bar' on side1 not
			 * be considered a colliding file but the other part
			 * of a normal rename.  If the file is very different,
			 * well we're going to get content merge conflicts
			 * anyway so it doesn't hurt.  And if the colliding
			 * file also has a different type, that'll be handled
			 * by the content merge logic in process_entry() too.
			 *
			 * See also t3030, 'rename vs. rename/symlink'
			 */
			collision = 0;
		}
		if (source_deleted) {
			if (target_index == 1) {
				rename_branch = opt->branch1;
				delete_branch = opt->branch2;
			} else {
				rename_branch = opt->branch2;
				delete_branch = opt->branch1;
			}
		}

#ifdef VERBOSE_DEBUG
		printf("collision: %d, source_deleted: %d\n",
		       collision, source_deleted);

		printf("  oldpath: %s, newpath: %s\n", oldpath, newpath);
		printf("source_deleted: %d\n", source_deleted);
		printf("oldinfo->filemask: %d\n", oldinfo->filemask);
		printf("old_sidemask: %d\n", old_sidemask);
#endif
		assert(source_deleted || oldinfo->filemask & old_sidemask);

		if (!type_changed) {
			/* Mark the original as resolved by removal */
			oldinfo->merged.is_null = 1;
			oldinfo->merged.clean = 1;
		}

		/* Need to check for special types of rename conflicts... */
		if (collision && !source_deleted) {
			/* collision: rename/add or rename/rename(2to1) */
			const char *pathnames[3];
			struct version_info merged;

			struct conflict_info *base, *side1, *side2;
			unsigned clean;

			pathnames[0] = oldpath;
			pathnames[other_source_index] = oldpath;
			pathnames[target_index] = newpath;
			base = strmap_get(&opt->priv->paths, pathnames[0]);
			side1 = strmap_get(&opt->priv->paths, pathnames[1]);
			side2 = strmap_get(&opt->priv->paths, pathnames[2]);
			clean = handle_content_merge(opt, pair->one->path,
						     &base->stages[0],
						     &side1->stages[1],
						     &side2->stages[2],
						     pathnames,
						     1 + 2*opt->priv->call_depth,
						     &merged);

#ifdef VERBOSE_DEBUG
			printf("--> Rename/add:\n");
			printf("      Paths: %s, %s, %s\n",
			       pathnames[0], pathnames[1], pathnames[2]);
			printf("      other_source_index: %d, target_index: %d\n",
			       other_source_index, target_index);
			printf("      Copied merge result into %s's stage %d\n",
			       newpath, target_index);
#endif
			memcpy(&newinfo->stages[target_index], &merged,
			       sizeof(merged));
			if (!clean) {
				path_msg(opt, newpath, 0,
					 _("CONFLICT (rename involved in "
					   "collision): rename of %s -> %s has "
					   "content conflicts AND collides "
					   "with another path; this may result "
					   "in nested conflict markers."),
					 oldpath, newpath);
			}
		} else if (collision && source_deleted) {
			/*
			 * rename/add/delete or rename/rename(2to1)/delete:
			 * since oldpath was deleted on the side that didn't
			 * do the rename, there's not much of a content merge
			 * we can do for the rename.  oldinfo->merged.is_null
			 * was already set, so we just leave things as-is so
			 * they look like an add/add conflict.
			 */

#ifdef VERBOSE_DEBUG
			printf("--> Rename/add/delete; not touching.\n");
#endif
			newinfo->path_conflict = 1;
			path_msg(opt, newpath, 0,
				 _("CONFLICT (rename/delete): %s renamed "
				   "to %s in %s, but deleted in %s."),
				 oldpath, newpath, rename_branch, delete_branch);
		} else {
			/*
			 * normal rename or rename/delete; copy the existing
			 * stage(s) from oldinfo over the newinfo and update
			 * the pathname(s).
			 */
#ifdef VERBOSE_DEBUG
			printf("--> Normal rename (or rename/delete):\n");
			printf("      Involving %s -> %s\n", oldpath, newpath);
			printf("      Copied stage 0 from old to new\n");
#endif
			memcpy(&newinfo->stages[0], &oldinfo->stages[0],
			       sizeof(newinfo->stages[0]));
			newinfo->filemask |= (1 << 0);
			newinfo->pathnames[0] = oldpath;
			if (type_changed) {
				/* Mark the original as resolved by removal */
				memcpy(&oldinfo->stages[0].oid, &null_oid,
				       sizeof(oldinfo->stages[0].oid));
				oldinfo->stages[0].mode = 0;
				oldinfo->filemask &= 0x06;
			} else if (source_deleted) {
				newinfo->path_conflict = 1;
				path_msg(opt, newpath, 0,
					 _("CONFLICT (rename/delete): %s renamed"
					   " to %s in %s, but deleted in %s."),
					 oldpath, newpath,
					 rename_branch, delete_branch);
			} else {
#ifdef VERBOSE_DEBUG
				printf("      Copied stage %d from old to new\n",
				       other_source_index);
#endif
				memcpy(&newinfo->stages[other_source_index],
				       &oldinfo->stages[other_source_index],
				       sizeof(newinfo->stages[0]));
				newinfo->filemask |= (1 << other_source_index);
				newinfo->pathnames[other_source_index] = oldpath;
			}
		}
	}

	return clean_merge;
}

/*** Directory rename stuff ***/

/*
 * For dir_rename_info, directory names are stored as a full path from the
 * toplevel of the repository and do not include a trailing '/'.  Also:
 *
 *   new_dir:            final name of directory being renamed
 *   possible_new_dirs:  temporary used to help determine new_dir; see comments
 *                       in get_directory_renames() for details
 */
struct dir_rename_info {
	struct strbuf new_dir;
	struct strintmap possible_new_dirs;
};

struct collision_info {
	struct string_list source_files;
	unsigned reported_already:1;
};

/*
 * Return a new string that replaces the beginning portion (which matches
 * rename_info->item.string), with rename_info->util.new_dir.  In perl-speak:
 *   new_path_name = (old_path =~ s/rename_info->item.string/rename_info->util.new_dir/);
 * NOTE:
 *   Caller must ensure that old_path starts with rename_info->string + '/'.
 */
static char *apply_dir_rename(struct string_list_item *rename_info,
			      const char *old_path)
{
	struct strbuf new_path = STRBUF_INIT;
	struct dir_rename_info *info = rename_info->util;
	int oldlen, newlen;

	oldlen = strlen(rename_info->string);
	if (info->new_dir.len == 0)
		/*
		 * If someone renamed/merged a subdirectory into the root
		 * directory (e.g. 'some/subdir' -> ''), then we want to
		 * avoid returning
		 *     '' + '/filename'
		 * as the rename; we need to make old_path + oldlen advance
		 * past the '/' character.
		 */
		oldlen++;
	newlen = info->new_dir.len + (strlen(old_path) - oldlen) + 1;
	strbuf_grow(&new_path, newlen);
	strbuf_addbuf(&new_path, &info->new_dir);
	strbuf_addstr(&new_path, &old_path[oldlen]);

	return strbuf_detach(&new_path, NULL);
}

static void get_renamed_dir_portion(const char *old_path, const char *new_path,
				    char **old_dir, char **new_dir)
{
	char *end_of_old, *end_of_new;

	/* Default return values: NULL, meaning no rename */
	*old_dir = NULL;
	*new_dir = NULL;

	/*
	 * For
	 *    "a/b/c/d/e/foo.c" -> "a/b/some/thing/else/e/foo.c"
	 * the "e/foo.c" part is the same, we just want to know that
	 *    "a/b/c/d" was renamed to "a/b/some/thing/else"
	 * so, for this example, this function returns "a/b/c/d" in
	 * *old_dir and "a/b/some/thing/else" in *new_dir.
	 */

	/*
	 * If the basename of the file changed, we don't care.  We want
	 * to know which portion of the directory, if any, changed.
	 */
	end_of_old = strrchr(old_path, '/');
	end_of_new = strrchr(new_path, '/');

	/*
	 * If end_of_old is NULL, old_path wasn't in a directory, so there
	 * could not be a directory rename (our rule elsewhere that a
	 * directory which still exists is not considered to have been
	 * renamed means the root directory can never be renamed -- because
	 * the root directory always exists).
	 */
	if (end_of_old == NULL)
		return; /* Note: *old_dir and *new_dir are still NULL */

	/*
	 * If new_path contains no directory (end_of_new is NULL), then we
	 * have a rename of old_path's directory to the root directory.
	 */
	if (end_of_new == NULL) {
		*old_dir = xstrndup(old_path, end_of_old - old_path);
		*new_dir = xstrdup("");
		return;
	}

	/* Find the first non-matching character traversing backwards */
	while (*--end_of_new == *--end_of_old &&
	       end_of_old != old_path &&
	       end_of_new != new_path)
		; /* Do nothing; all in the while loop */

	/*
	 * If both got back to the beginning of their strings, then the
	 * directory didn't change at all, only the basename did.
	 */
	if (end_of_old == old_path && end_of_new == new_path &&
	    *end_of_old == *end_of_new)
		return; /* Note: *old_dir and *new_dir are still NULL */

	/*
	 * If end_of_new got back to the beginning of its string, and
	 * end_of_old got back to the beginning of some subdirectory, then
	 * we have a rename/merge of a subdirectory into the root, which
	 * needs slightly special handling.
	 *
	 * Note: There is no need to consider the opposite case, with a
	 * rename/merge of the root directory into some subdirectory
	 * because as noted above the root directory always exists so it
	 * cannot be considered to be renamed.
	 */
	if (end_of_new == new_path &&
	    end_of_old != old_path && end_of_old[-1] == '/') {
		*old_dir = xstrndup(old_path, --end_of_old - old_path);
		*new_dir = xstrdup("");
		return;
	}

	/*
	 * We've found the first non-matching character in the directory
	 * paths.  That means the current characters we were looking at
	 * were part of the first non-matching subdir name going back from
	 * the end of the strings.  Get the whole name by advancing both
	 * end_of_old and end_of_new to the NEXT '/' character.  That will
	 * represent the entire directory rename.
	 *
	 * The reason for the increment is cases like
	 *    a/b/star/foo/whatever.c -> a/b/tar/foo/random.c
	 * After dropping the basename and going back to the first
	 * non-matching character, we're now comparing:
	 *    a/b/s          and         a/b/
	 * and we want to be comparing:
	 *    a/b/star/      and         a/b/tar/
	 * but without the pre-increment, the one on the right would stay
	 * a/b/.
	 */
	end_of_old = strchr(++end_of_old, '/');
	end_of_new = strchr(++end_of_new, '/');

	/* Copy the old and new directories into *old_dir and *new_dir. */
	*old_dir = xstrndup(old_path, end_of_old - old_path);
	*new_dir = xstrndup(new_path, end_of_new - new_path);
}

#if 0
static void remove_rename_entries(struct strmap *dir_renames,
				  struct string_list *items_to_remove)
{
	int i;

	for (i = 0; i < items_to_remove->nr; i++)
		strmap_remove(dir_renames, items_to_remove->items[i].string, 1);
	string_list_clear(items_to_remove, 0);
}
#endif

static int path_in_way(struct strmap *paths, const char *path, unsigned side_mask)
{
	struct conflict_info *ci = strmap_get(paths, path);
	if (!ci)
		return 0;
	return ci->merged.clean || (side_mask & (ci->filemask | ci->dirmask));
}

/*
 * See if there is a directory rename for path, and if there are any file
 * level conflicts on the given side for the renamed location.  If there is
 * a rename and there are no conflicts, return the new name.  Otherwise,
 * return NULL.
 */
static char *handle_path_level_conflicts(struct merge_options *opt,
					 const char *path,
					 unsigned side_index,
					 struct string_list_item *rename_info,
					 struct strmap *collisions)
{
	char *new_path = NULL;
	struct collision_info *c_info;
	int clean = 1;
	struct strbuf collision_paths = STRBUF_INIT;

	/*
	 * entry has the mapping of old directory name to new directory name
	 * that we want to apply to path.
	 */
	new_path = apply_dir_rename(rename_info, path);
	if (!new_path)
		BUG("Failed to apply directory rename!");

	/*
	 * The caller needs to have ensured that it has pre-populated
	 * collisions with all paths that map to new_path.  Do a quick check
	 * to ensure that's the case.
	 */
	c_info = strmap_get(collisions, new_path);
	if (c_info == NULL)
		BUG("c_info is NULL");

	/*
	 * Check for one-sided add/add/.../add conflicts, i.e.
	 * where implicit renames from the other side doing
	 * directory rename(s) can affect this side of history
	 * to put multiple paths into the same location.  Warn
	 * and bail on directory renames for such paths.
	 */
	if (c_info->reported_already) {
		clean = 0;
	} else if (path_in_way(&opt->priv->paths, new_path, 1 << side_index)) {
		c_info->reported_already = 1;
		strbuf_add_separated_string_list(&collision_paths, ", ",
						 &c_info->source_files);
		path_msg(opt, new_path, 0,
			 _("CONFLICT (implicit dir rename): Existing file/dir "
			   "at %s in the way of implicit directory rename(s) "
			   "putting the following path(s) there: %s."),
		       new_path, collision_paths.buf);
		clean = 0;
	} else if (c_info->source_files.nr > 1) {
		c_info->reported_already = 1;
		strbuf_add_separated_string_list(&collision_paths, ", ",
						 &c_info->source_files);
		path_msg(opt, new_path, 0,
			 _("CONFLICT (implicit dir rename): Cannot map more "
			   "than one path to %s; implicit directory renames "
			   "tried to put these paths there: %s"),
		       new_path, collision_paths.buf);
		clean = 0;
	}

	/* Free memory we no longer need */
	strbuf_release(&collision_paths);
	if (!clean && new_path) {
		free(new_path);
		return NULL;
	}

	return new_path;
}

static struct strmap *get_directory_renames(struct merge_options *opt,
					    unsigned side,
					    int *clean)
{
	struct strmap *dir_renames;
	struct hashmap_iter iter;
	struct str_entry *entry;
	struct string_list to_remove = STRING_LIST_INIT_NODUP;
	struct diff_queue_struct *pairs = &opt->priv->renames->pairs[side];
	int i;

	dir_renames = xmalloc(sizeof(*dir_renames));
	strmap_init(dir_renames, 0);

#if 0 /* FIXME: Clean this up */
	/* It's tempting to bail early if there aren't any dirs_removed to
	 * consider, but it causes things like tests 6b of t6043 to fail.
	 */
	if (strset_get_size(&opt->priv->renames->dirs_removed[side]) == 0)
		return dir_renames;
#endif

	/*
	 * Typically, we think of a directory rename as all files from a
	 * certain directory being moved to a target directory.  However,
	 * what if someone first moved two files from the original
	 * directory in one commit, and then renamed the directory
	 * somewhere else in a later commit?  At merge time, we just know
	 * that files from the original directory went to two different
	 * places, and that the bulk of them ended up in the same place.
	 * We want each directory rename to represent where the bulk of the
	 * files from that directory end up; this function exists to find
	 * where the bulk of the files went.
	 *
	 * The first loop below simply iterates through the list of file
	 * renames, finding out how often each directory rename pair
	 * possibility occurs.
	 */
	for (i = 0; i < pairs->nr; ++i) {
		struct diff_filepair *pair = pairs->queue[i];
		struct dir_rename_info *info;
		int count;
		char *old_dir, *new_dir;

		/* File not part of directory rename if it wasn't renamed */
		if (pair->status != 'R')
			continue;

		get_renamed_dir_portion(pair->one->path, pair->two->path,
					&old_dir,        &new_dir);
		if (!old_dir)
			/* Directory didn't change at all; ignore this one. */
			continue;

#if 0 /* FIXME: Clean this up */
		if (!strset_contains(&opt->priv->renames->dirs_removed[side],
				     old_dir)) {
			/* old_dir still exists and can't be a dir rename */
			free(old_dir);
			free(new_dir);
			continue;
		}
#endif

		info = strmap_get(dir_renames, old_dir);
		if (info) {
			free(old_dir);
		} else {
			info = xcalloc(1, sizeof(*info));
			strbuf_init(&info->new_dir, 0);
			strintmap_init(&info->possible_new_dirs, 0);
			strmap_put(dir_renames, old_dir, info);
		}

		count = strintmap_get(&info->possible_new_dirs, new_dir, 0);
		strintmap_set(&info->possible_new_dirs, new_dir, count+1);
		if (count)
			free(new_dir);
	}

	/*
	 * For each directory with files moved out of it, we find out which
	 * target directory received the most files so we can declare it to
	 * be the "winning" target location for the directory rename.  This
	 * winner gets recorded in new_dir.  If there is no winner
	 * (multiple target directories received the same number of files),
	 * we set non_unique_new_dir.  Once we've determined the winner (or
	 * that there is no winner), we no longer need possible_new_dirs.
	 */
	strmap_for_each_entry(dir_renames, &iter, entry) {
		int max = 0;
		int bad_max = 0;
		char *best = NULL;
		struct dir_rename_info *info = entry->item.util;
		struct hashmap_iter pnd_iter;
		struct str_entry *pnd_entry;

		strintmap_for_each_entry(&info->possible_new_dirs, &pnd_iter,
					 pnd_entry) {
			intptr_t count = (intptr_t)pnd_entry->item.util;

			if (count == max)
				bad_max = max;
			else if (count > max) {
				max = count;
				best = pnd_entry->item.string;
			}
		}
		if (bad_max == max) {
			string_list_append(&to_remove, entry->item.string);
			path_msg(opt, entry->item.string, 0,
			       _("CONFLICT (directory rename split): "
				 "Unclear where to rename %s to; it was "
				 "renamed to multiple other directories, with "
				 "no destination getting a majority of the "
				 "files."),
			       entry->item.string);
			*clean &= 0;
		} else {
			assert(info->new_dir.len == 0);
			strbuf_addstr(&info->new_dir, best);
#ifdef VERBOSE_DEBUG
			fprintf(stderr, "Dir rename %s -> %s\n",
				entry->item.string, best);
#endif
		}
		/*
		 * The relevant directory sub-portion of the original full
		 * filepaths were xstrndup'ed before inserting into
		 * possible_new_dirs, and instead of manually iterating the
		 * list and free'ing each, just lie and tell
		 * possible_new_dirs that it did the strdup'ing so that it
		 * will free them for us.
		 */
		info->possible_new_dirs.map.strdup_strings = 1;
		strintmap_free(&info->possible_new_dirs);
	}

	for (i=0; i<to_remove.nr; ++i)
		strmap_remove(dir_renames, to_remove.items[i].string, 1);

	return dir_renames;
}

static void remove_invalid_dir_renames(struct merge_options *opt,
				       struct strmap *side_dir_renames,
				       unsigned side_mask)
{
	struct hashmap_iter iter;
	struct str_entry *entry;
	struct conflict_info *ci;
	struct string_list removable = STRING_LIST_INIT_NODUP;

	strmap_for_each_entry(side_dir_renames, &iter, entry) {
		ci = strmap_get(&opt->priv->paths, entry->item.string);
		if (!ci) {
			/*
			 * This rename came from a directory that was unchanged
			 * on the other side of history, and NULL on our side.
			 * We don't need to detect a directory rename for it.
			 */
			string_list_append(&removable, entry->item.string);
			continue;
		}
		assert(!ci->merged.clean);
		if (ci->dirmask & side_mask)
			/*
			 * This directory "rename" isn't valid because the
			 * source directory name still exists on the destination
			 * side.
			 */
			string_list_append(&removable, entry->item.string);
	}

	for (int i=0; i<removable.nr; ++i)
		strmap_remove(side_dir_renames, removable.items[i].string, 1);
	string_list_clear(&removable, 0);
}

static void handle_directory_level_conflicts(struct merge_options *opt,
					     struct strmap *side1_dir_renames,
					     struct strmap *side2_dir_renames)
{
	struct hashmap_iter iter;
	struct str_entry *entry;
	struct string_list duplicated = STRING_LIST_INIT_NODUP;

	strmap_for_each_entry(side1_dir_renames, &iter, entry) {
		if (strmap_contains(side2_dir_renames, entry->item.string))
			string_list_append(&duplicated, entry->item.string);
	}

	for (int i=0; i<duplicated.nr; ++i) {
		strmap_remove(side1_dir_renames, duplicated.items[i].string, 1);
		strmap_remove(side2_dir_renames, duplicated.items[i].string, 1);
	}
	string_list_clear(&duplicated, 0);

	remove_invalid_dir_renames(opt, side1_dir_renames, 2);
	remove_invalid_dir_renames(opt, side2_dir_renames, 4);
}

static struct string_list_item *check_dir_renamed(const char *path,
						  struct strmap *dir_renames)
{
	char *temp = xstrdup(path);
	char *end;
	struct string_list_item *item = NULL;

	while ((end = strrchr(temp, '/'))) {
		*end = '\0';
		item = strmap_get_item(dir_renames, temp);
		if (item)
			break;
	}
	free(temp);
	return item;
}

static void compute_collisions(struct strmap *collisions,
			       struct strmap *dir_renames,
			       struct diff_queue_struct *pairs)
{
	int i;

	strmap_init(collisions, 0);
	if (strmap_empty(dir_renames))
		return;

	/*
	 * Multiple files can be mapped to the same path due to directory
	 * renames done by the other side of history.  Since that other
	 * side of history could have merged multiple directories into one,
	 * if our side of history added the same file basename to each of
	 * those directories, then all N of them would get implicitly
	 * renamed by the directory rename detection into the same path,
	 * and we'd get an add/add/.../add conflict, and all those adds
	 * from *this* side of history.  This is not representable in the
	 * index, and users aren't going to easily be able to make sense of
	 * it.  So we need to provide a good warning about what's
	 * happening, and fall back to no-directory-rename detection
	 * behavior for those paths.
	 *
	 * See testcases 9e and all of section 5 from t6043 for examples.
	 */
	for (i = 0; i < pairs->nr; ++i) {
		struct string_list_item *rename_info;
		struct collision_info *collision_info;
		char *new_path;
		struct diff_filepair *pair = pairs->queue[i];

		if (pair->status != 'A' && pair->status != 'R')
			continue;
		rename_info = check_dir_renamed(pair->two->path, dir_renames);
		if (!rename_info)
			continue;

		new_path = apply_dir_rename(rename_info, pair->two->path);
		assert(new_path);
		collision_info = strmap_get(collisions, new_path);
		if (collision_info) {
			free(new_path);
		} else {
			collision_info = xcalloc(1,
						 sizeof(struct collision_info));
			string_list_init(&collision_info->source_files, 0);
			strmap_put(collisions, new_path, collision_info);
		}
		string_list_insert(&collision_info->source_files,
				   pair->two->path);
	}
}

static char *check_for_directory_rename(struct merge_options *opt,
					const char *path,
					unsigned side_index,
					struct strmap *dir_renames,
					struct strmap *dir_rename_exclusions,
					struct strmap *collisions,
					int *clean_merge)
{
	char *new_path = NULL;
	struct string_list_item *rename_info;
	struct string_list_item *otherinfo = NULL;
	struct dir_rename_info *rename_dir_info;

	if (strmap_empty(dir_renames))
		return new_path;
	rename_info = check_dir_renamed(path, dir_renames);
	if (!rename_info)
		return new_path;
	rename_dir_info = rename_info->util;

	/*
	 * This next part is a little weird.  We do not want to do an
	 * implicit rename into a directory we renamed on our side, because
	 * that will result in a spurious rename/rename(1to2) conflict.  An
	 * example:
	 *   Base commit: dumbdir/afile, otherdir/bfile
	 *   Side 1:      smrtdir/afile, otherdir/bfile
	 *   Side 2:      dumbdir/afile, dumbdir/bfile
	 * Here, while working on Side 1, we could notice that otherdir was
	 * renamed/merged to dumbdir, and change the diff_filepair for
	 * otherdir/bfile into a rename into dumbdir/bfile.  However, Side
	 * 2 will notice the rename from dumbdir to smrtdir, and do the
	 * transitive rename to move it from dumbdir/bfile to
	 * smrtdir/bfile.  That gives us bfile in dumbdir vs being in
	 * smrtdir, a rename/rename(1to2) conflict.  We really just want
	 * the file to end up in smrtdir.  And the way to achieve that is
	 * to not let Side1 do the rename to dumbdir, since we know that is
	 * the source of one of our directory renames.
	 *
	 * That's why otherinfo and dir_rename_exclusions is here.
	 *
	 * As it turns out, this also prevents N-way transient rename
	 * confusion; See testcases 9c and 9d of t6043.
	 */
	otherinfo = strmap_get_item(dir_rename_exclusions,
				    rename_dir_info->new_dir.buf);
	if (otherinfo) {
		path_msg(opt, rename_info->string, 1,
			 _("WARNING: Avoiding applying %s -> %s rename "
			   "to %s, because %s itself was renamed."),
			 rename_info->string, rename_dir_info->new_dir.buf,
			 path, rename_dir_info->new_dir.buf);
		return NULL;
	}

	new_path = handle_path_level_conflicts(opt, path, side_index,
					       rename_info, collisions);
	*clean_merge &= (new_path != NULL);

	return new_path;
}

static void dump_conflict_info(struct conflict_info *ci, char *name)
{
#ifdef VERBOSE_DEBUG
	printf("conflict_info for %s (at %p):\n", name, ci);
	printf("  ci->merged.directory_name: %s\n",
	       ci->merged.directory_name);
	printf("  ci->merged.basename_offset: %lu\n",
	       ci->merged.basename_offset);
	printf("  ci->merged.is_null: %d\n",
	       ci->merged.is_null);
	printf("  ci->merged.clean: %d\n",
	       ci->merged.clean);
	if (ci->merged.clean)
		return;
	for (int i=0; i<3; i++) {
		printf("  ci->pathnames[%d]:   %s\n", i, ci->pathnames[i]);
		printf("  ci->stages[%d].mode: %o\n", i, ci->stages[i].mode);
		printf("  ci->stages[%d].oid:  %s\n", i, oid_to_hex(&ci->stages[i].oid));
	}
	printf("  ci->df_conflict:   %d\n", ci->df_conflict);
	printf("  ci->path_conflict: %d\n", ci->path_conflict);
	printf("  ci->filemask:      %d\n", ci->filemask);
	printf("  ci->dirmask:       %d\n", ci->dirmask);
	printf("  ci->match_mask:    %d\n", ci->match_mask);
	printf("  ci->processed:     %d\n", ci->processed);
#endif
}

static void dump_pairs(struct diff_queue_struct *pairs, char *label)
{
#ifdef VERBOSE_DEBUG
	int i;

	printf("%s pairs:\n", label);
	for (i=0; i<pairs->nr; ++i) {
		printf("  %c %d %d %d %d %d %d\n",
		       pairs->queue[i]->status,
		       pairs->queue[i]->broken_pair,
		       pairs->queue[i]->renamed_pair,
		       pairs->queue[i]->is_unmerged,
		       pairs->queue[i]->done_skip_stat_unmatch,
		       pairs->queue[i]->skip_stat_unmatch_result,
		       pairs->queue[i]->score);
		printf("    %06o %s %s\n",
		       pairs->queue[i]->one->mode,
		       oid_to_hex(&pairs->queue[i]->one->oid),
		       pairs->queue[i]->one->path);
		printf("    %06o %s %s\n",
		       pairs->queue[i]->two->mode,
		       oid_to_hex(&pairs->queue[i]->two->oid),
		       pairs->queue[i]->two->path);
	}
#endif
}

static void apply_directory_rename_modifications(struct merge_options *opt,
						 struct diff_filepair *pair,
						 char *new_path)
{
	/*
	 * The basic idea is to get the conflict_info from opt->priv->paths
	 * at old path, and insert it into new_path; basically just this:
	 *     ci = strmap_get(&opt->priv->paths, old_path);
	 *     strmap_remove(&opt->priv->paths, old_path, 0);
	 *     strmap_put(&opt->priv->paths, new_path, ci);
	 * However, there are some factors complicating this:
	 *     - opt->priv->paths may already have an entry at new_path
	 *     - Each ci tracks its containing directory, so we need to
	 *       update that
	 *     - If another ci has the same containing directory, then
	 *       the two char*'s MUST point to the same location.  See the
	 *       comment in struct merged_info.  strcmp equality is not
	 *       enough; we need pointer equality.
	 *     - opt->priv->paths must hold the parent directories of any
	 *       entries that are added.  So, if this directory rename
	 *       causes entirely new directories, we must recursively add
	 *       parent directories.
	 *     - For each parent directory added to opt->priv->paths, we
	 *       also need to get its parent directory stored in its
	 *       conflict_info->merged.directory_name with all the same
	 *       requirements about pointer equality.
	 */
	struct string_list dirs_to_insert = STRING_LIST_INIT_NODUP;
	struct conflict_info *ci, *new_ci;
	struct string_list_item *item;
	const char *branch_with_new_path, *branch_with_dir_rename;
	char *old_path = pair->two->path;
	char *parent_name;
	char *cur_path;
	int i, len;

	item = strmap_get_item(&opt->priv->paths, old_path);
	old_path = item->string;
	ci = item->util;
	dump_conflict_info(ci, old_path);

	/* Find parent directories missing from opt->priv->paths */
#if USE_MEMORY_POOL
	cur_path = mem_pool_strdup(&opt->priv->pool, new_path);
	free(new_path);
	new_path = cur_path;
#else
	cur_path = new_path;
#endif
	while (1) {
		/* Find the parent directory of cur_path */
		char *last_slash = strrchr(cur_path, '/');
		if (last_slash)
#if USE_MEMORY_POOL
			parent_name = mem_pool_strndup(&opt->priv->pool,
						       cur_path,
						       last_slash - cur_path);
#else
			parent_name = xstrndup(cur_path, last_slash - cur_path);
#endif
		else {
			parent_name = opt->priv->toplevel_dir;
			break;
		}

		/* Look it up in opt->priv->paths */
		item = strmap_get_item(&opt->priv->paths, parent_name);
		if (item) {
#if !USE_MEMORY_POOL
			free(parent_name);
#endif
			parent_name = item->string; /* reuse known pointer */
			break;
		}

		/* Record this is one of the directories we need to insert */
		string_list_append(&dirs_to_insert, parent_name);
		cur_path = parent_name;
	}

	/* Traverse dirs_to_insert and insert them into opt->priv->paths */
	for (i = dirs_to_insert.nr-1; i >= 0; --i) {
		struct conflict_info *dir_ci;
		char *cur_dir = dirs_to_insert.items[i].string;

		dir_ci = xcalloc(1, sizeof(*dir_ci));

		dir_ci->merged.directory_name = parent_name;
		len = strlen(parent_name);
		/* len+1 because of trailing '/' character */
		dir_ci->merged.basename_offset = (len > 0 ? len+1 : len);
		dir_ci->dirmask = ci->filemask;
		strmap_put(&opt->priv->paths, cur_dir, dir_ci);

		parent_name = cur_dir;
	}

#if !USE_MEMORY_POOL
	/*
	 * We are removing old_path from opt->priv->paths.  old_path also will
	 * eventually need to be freed, but it may still be used by e.g.
	 * ci->pathnames.  So, store it in another string-list for now.
	 */
	string_list_append(&opt->priv->paths_to_free, old_path);
#endif
#ifdef VERBOSE_DEBUG
	printf("Removing %s from opt->priv->paths!\n", old_path);
#endif
	assert(ci->filemask == 2 || ci->filemask == 4);
	assert(ci->dirmask == 0);
	strmap_remove(&opt->priv->paths, old_path, 0);

	branch_with_new_path   = (ci->filemask == 2) ? opt->branch1 : opt->branch2;
	branch_with_dir_rename = (ci->filemask == 2) ? opt->branch2 : opt->branch1;

	/* Now, finally update ci and stick it into opt->priv->paths */
	ci->merged.directory_name = parent_name;
	len = strlen(parent_name);
	ci->merged.basename_offset = (len > 0 ? len+1 : len);
	new_ci = strmap_get(&opt->priv->paths, new_path);
#ifdef VERBOSE_DEBUG
	printf("Renaming %s to %s; new_ci = %p\n", old_path, new_path, new_ci);
#endif
	if (!new_ci) {
		/* Place ci back into opt->priv->paths, but at new_path */
		strmap_put(&opt->priv->paths, new_path, ci);
	} else {
		int index;

		/* A few sanity checks */
		assert(ci->filemask == 2 || ci->filemask == 4);
		assert((new_ci->filemask & ci->filemask) == 0);
		assert(!new_ci->merged.clean);

		/* Massive debuggery */
#ifdef VERBOSE_DEBUG
		printf("Copying stuff from ci to new_ci:\n");
		dump_conflict_info(ci, "ci");
		dump_conflict_info(new_ci, "new_ci");
#endif

		/* Copy stuff from ci into new_ci */
		new_ci->filemask |= ci->filemask;
		if (new_ci->dirmask)
			new_ci->df_conflict = 1;
		index = (ci->filemask >> 1);
		new_ci->pathnames[index] = ci->pathnames[index];
		new_ci->stages[index].mode = ci->stages[index].mode;
		oidcpy(&new_ci->stages[index].oid, &ci->stages[index].oid);

#if !USE_MEMORY_POOL
		free(ci);
#endif
		ci = new_ci;
	}

	if (opt->detect_directory_renames == MERGE_DIRECTORY_RENAMES_TRUE) {
		/* Notify user of updated path */
		if (pair->status == 'A')
			path_msg(opt, new_path, 1,
				 _("Path updated: %s added in %s inside a "
				   "directory that was renamed in %s; moving "
				   "it to %s."),
				 old_path, branch_with_new_path,
				 branch_with_dir_rename, new_path);
		else
			path_msg(opt, new_path, 1,
				 _("Path updated: %s renamed to %s in %s, "
				   "inside a directory that was renamed in %s; "
				   "moving it to %s."),
				 pair->one->path, old_path, branch_with_new_path,
				 branch_with_dir_rename, new_path);
	} else {
		/*
		 * opt->detect_directory_renames has the value
		 * MERGE_DIRECTORY_RENAMES_CONFLICT, so mark these as conflicts.
		 */
		ci->path_conflict = 1;
		if (pair->status == 'A')
			path_msg(opt, new_path, 0,
				 _("CONFLICT (file location): %s added in %s "
				   "inside a directory that was renamed in %s, "
				   "suggesting it should perhaps be moved to "
				   "%s."),
				 old_path, branch_with_new_path,
				 branch_with_dir_rename, new_path);
		else
			path_msg(opt, new_path, 0,
				 _("CONFLICT (file location): %s renamed to %s "
				   "in %s, inside a directory that was renamed "
				   "in %s, suggesting it should perhaps be "
				   "moved to %s."),
				 pair->one->path, old_path, branch_with_new_path,
				 branch_with_dir_rename, new_path);
	}

	/*
	 * Finally, record the new location.
	 */
	pair->two->path = new_path;
}

/*** Rename stuff ***/

static inline int possible_renames(struct rename_info *renames,
				   unsigned side_index)
{
	return renames->pairs[side_index].nr > 0 &&
	       !strintmap_empty(&renames->relevant_sources[side_index]);
}

static void resolve_diffpair_statuses(struct diff_queue_struct *q)
{
	/*
	 * A simplified version of diff_resolve_rename_copy(); would probably
	 * just use that function but it's static...
	 */
	int i;
	struct diff_filepair *p;

	for (i = 0; i < q->nr; ++i) {
		p = q->queue[i];
		p->status = 0; /* undecided */
		if (!DIFF_FILE_VALID(p->one))
			p->status = DIFF_STATUS_ADDED;
		else if (!DIFF_FILE_VALID(p->two))
			p->status = DIFF_STATUS_DELETED;
		else if (DIFF_PAIR_RENAME(p))
			p->status = DIFF_STATUS_RENAMED;
	}
}

static void prune_cached_from_relevant(struct rename_info *renames,
				       unsigned side)
{
	struct hashmap_iter iter;
	struct str_entry *entry;

	/* Remove from relevant_sources all entries in cached_pairs[side] */
	strmap_for_each_entry(&renames->cached_pairs[side], &iter, entry) {
		strintmap_remove(&renames->relevant_sources[side],
				 entry->item.string);
	}
}

static void use_cached_pairs(struct merge_options *opt,
			     struct strmap *cached_pairs,
			     struct diff_queue_struct *pairs)
{
	struct hashmap_iter iter;
	struct str_entry *entry;
#if USE_MEMORY_POOL
	struct mem_pool *pool = &opt->priv->pool;
#endif

	/* Add to side_pairs all entries from renames->cached_pairs[side_index] */
	strmap_for_each_entry(cached_pairs, &iter, entry) {
		struct diff_filespec *one, *two;
		char *old_name = entry->item.string;
		char *new_name = entry->item.util;
		if (!new_name)
			new_name = old_name;

		/* We don't care about oid/mode, only filenames and status */
#if USE_MEMORY_POOL
		one = mempool_alloc_filespec(pool, old_name);
		two = mempool_alloc_filespec(pool, new_name);
		mempool_diff_queue(pool, pairs, one, two);
#else
		one = alloc_filespec(old_name);
		two = alloc_filespec(new_name);
		diff_queue(pairs, one, two);
#endif
		pairs->queue[pairs->nr-1]->status = entry->item.util ? 'R' : 'D';
	}
}

static void possibly_cache_new_pair(struct rename_info *renames,
				    struct diff_filepair *p,
				    unsigned side,
				    char *new_path)
{
	char *old_value;

	if (!new_path &&
	    !strintmap_contains(&renames->relevant_sources[side], p->one->path))
		return;
	if (p->status == 'D') {
		/*
		 * If we already had this delete, we'll just set it's value
		 * to NULL again, so no harm.
		 */
		strmap_put(&renames->cached_pairs[side], p->one->path, NULL);
	} else if (p->status == 'R') {
		if (!new_path)
			new_path = p->two->path;
		old_value = strmap_put(&renames->cached_pairs[side],
				       p->one->path, xstrdup(new_path));
		free(old_value);
	} else if (p->status == 'A' && new_path) {
		old_value = strmap_put(&renames->cached_pairs[side],
				       p->two->path, xstrdup(new_path));
		assert(!old_value);
	}
}

static int compare_pairs(const void *a_, const void *b_)
{
	const struct diff_filepair *a = *((const struct diff_filepair **)a_);
	const struct diff_filepair *b = *((const struct diff_filepair **)b_);

	int cmp = strcmp(a->one->path, b->one->path);
	if (cmp)
		return cmp;
	return a->score - b->score;
}

/* Call diffcore_rename() to update deleted/added pairs into rename pairs */
static int detect_regular_renames(struct merge_options *opt,
				   unsigned side_index)
{
	struct diff_options diff_opts;
	struct rename_info *renames = opt->priv->renames;

	prune_cached_from_relevant(renames, side_index);
	if (!possible_renames(renames, side_index)) {
		/*
		 * No rename detection needed for this side, but we still need
		 * to make sure 'adds' are marked correctly in case the other
		 * side had directory renames.
		 */
		resolve_diffpair_statuses(&renames->pairs[side_index]);
		return 0;
	}

	repo_diff_setup(opt->repo, &diff_opts);
	diff_opts.flags.recursive = 1;
	diff_opts.flags.rename_empty = 0;
	diff_opts.detect_rename = merge_detect_rename(opt);
	/*
	 * We do not have logic to handle the detection of copies.  In
	 * fact, it may not even make sense to add such logic: would we
	 * really want a change to a base file to be propagated through
	 * multiple other files by a merge?
	 */
	if (diff_opts.detect_rename > DIFF_DETECT_RENAME)
		diff_opts.detect_rename = DIFF_DETECT_RENAME;
	diff_opts.rename_limit = opt->rename_limit;
	if (opt->rename_limit <= 0)
		diff_opts.rename_limit = 1000;
	diff_opts.rename_score = opt->rename_score;
	diff_opts.show_rename_progress = opt->show_rename_progress;
	diff_opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_setup_done(&diff_opts);

	diff_queued_diff = renames->pairs[side_index];
	dump_pairs(&diff_queued_diff, "Before diffcore_rename");
	trace2_region_enter("diff", "diffcore_rename", opt->repo);
	printf("Doing rename detection on side %d\n", side_index);
	diffcore_rename_extended(&diff_opts,
#if USE_MEMORY_POOL
				 &opt->priv->pool,
#else
				 NULL,
#endif
				 &renames->relevant_sources[side_index],
				 NULL,
				 &renames->dirs_removed[side_index]);
	trace2_region_leave("diff", "diffcore_rename", opt->repo);
	resolve_diffpair_statuses(&diff_queued_diff);
	dump_pairs(&diff_queued_diff, "After diffcore_rename");
#ifdef VERBOSE_DEBUG
	printf("Done.\n");
#endif
	if (diff_opts.needed_rename_limit > opt->priv->needed_rename_limit)
		opt->priv->needed_rename_limit = diff_opts.needed_rename_limit;

	renames->pairs[side_index] = diff_queued_diff;

	diff_opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_queued_diff.nr = 0;
	diff_queued_diff.queue = NULL;
	diff_flush(&diff_opts);

	if (renames->redo_after_renames) {
		int i;
		struct diff_filepair *p;

		renames->redo_after_renames = 2;
		for (i = 0; i < renames->pairs[side_index].nr; ++i) {
			p = renames->pairs[side_index].queue[i];
			possibly_cache_new_pair(renames, p, side_index, NULL);
		}
	}
	return 1;
}

/*
 * Get information of all renames which occurred in 'side_pairs', making use
 * of any implicit directory renames in side_dir_renames (also making use of
 * implicit directory renames rename_exclusions as needed by
 * check_for_directory_rename()).  Add all (updated) renames into result.
 */
static int collect_renames(struct merge_options *opt,
			   struct diff_queue_struct *result,
			   unsigned side_index,
			   struct strmap *dir_renames_for_side,
			   struct strmap *rename_exclusions)
{
	int i, clean = 1;
	struct strmap collisions;
	struct diff_queue_struct *side_pairs;
	struct hashmap_iter iter;
	struct str_entry *entry;
	struct rename_info *renames = opt->priv->renames;

	side_pairs = &renames->pairs[side_index];
	compute_collisions(&collisions, dir_renames_for_side, side_pairs);

#ifdef VERBOSE_DEBUG
	fprintf(stderr, "All pairs:\n");
#endif
	for (i = 0; i < side_pairs->nr; ++i) {
		struct diff_filepair *p = side_pairs->queue[i];
		char *new_path; /* non-NULL only with directory renames */

#ifdef VERBOSE_DEBUG
		fprintf(stderr, "  (%c, %s -> %s)\n", p->status, p->one->path, p->two->path);
#endif
		possibly_cache_new_pair(renames, p, side_index, NULL);
		if (p->status != 'A' && p->status != 'R') {
#if USE_MEMORY_POOL
			diff_free_filepair_data(p);
#else
			diff_free_filepair(p);
#endif
			continue;
		}
		new_path = check_for_directory_rename(opt, p->two->path,
						      side_index,
						      dir_renames_for_side,
						      rename_exclusions,
						      &collisions,
						      &clean);
#ifdef VERBOSE_DEBUG
		fprintf(stderr, "    new_path: %s\n", new_path);
#endif
		if (p->status != 'R' && !new_path) {
#if USE_MEMORY_POOL
			diff_free_filepair_data(p);
#else
			diff_free_filepair(p);
#endif
			continue;
		}
		possibly_cache_new_pair(renames, p, side_index, new_path);
		if (new_path)
			apply_directory_rename_modifications(opt, p, new_path);

		p->score = side_index;
		result->queue[result->nr++] = p;
	}

	strmap_for_each_entry(&collisions, &iter, entry) {
		struct collision_info *info = entry->item.util;
		string_list_clear(&info->source_files, 0);
	}
	/*
	 * In compute_collisions(), we set collisions.strdup_strings to 0
	 * so that we wouldn't have to make another copy of the new_path
	 * allocated by apply_dir_rename().  But now that we've used them
	 * and have no other references to these strings, it is time to
	 * deallocate them, which we do by just setting strdup_string = 1
	 * before the strmaps is cleared.
	 */
	collisions.strdup_strings = 1;
	strmap_free(&collisions, 1);
	return clean;
}

static int detect_and_process_renames(struct merge_options *opt,
				      struct diff_queue_struct *combined,
				      struct tree *merge_base,
				      struct tree *side1,
				      struct tree *side2)
{
	struct strmap *dir_renames[3]; /* Entry 0 unused */
	struct rename_info *renames = opt->priv->renames;
	int need_dir_renames, s, clean = 1;
	struct hashmap_iter iter;
	struct str_entry *entry;
	unsigned detection_run = 0;

	memset(combined, 0, sizeof(*combined));
	if (!merge_detect_rename(opt))
		goto diff_filepair_cleanup;
	if (!possible_renames(renames, 1) && !possible_renames(renames, 2))
		goto diff_filepair_cleanup;

	trace2_region_enter("merge", "regular renames", opt->repo);
	detection_run |= detect_regular_renames(opt, 1);
	detection_run |= detect_regular_renames(opt, 2);
	if (renames->redo_after_renames && detection_run) {
		trace2_region_leave("merge", "regular renames", opt->repo);
		goto diff_filepair_cleanup;
	}
	use_cached_pairs(opt, &renames->cached_pairs[1], &renames->pairs[1]);
	use_cached_pairs(opt, &renames->cached_pairs[2], &renames->pairs[2]);
	trace2_region_leave("merge", "regular renames", opt->repo);

	trace2_region_enter("merge", "directory renames", opt->repo);
	need_dir_renames =
	  !opt->priv->call_depth &&
	  (opt->detect_directory_renames == MERGE_DIRECTORY_RENAMES_TRUE ||
	   opt->detect_directory_renames == MERGE_DIRECTORY_RENAMES_CONFLICT);

	if (need_dir_renames) {
#ifdef VERBOSE_DEBUG
		struct hashmap_iter iter;
		struct str_entry *entry;
#endif

		for (s = 1; s <= 2; s++)
			dir_renames[s] = get_directory_renames(opt, s, &clean);
#ifdef VERBOSE_DEBUG
		for (s = 1; s <= 2; s++) {
			fprintf(stderr, "dir renames[%d]:\n", s);
			strmap_for_each_entry(dir_renames[s], &iter, entry) {
				struct dir_rename_info *info = entry->item.util;
				fprintf(stderr, "    %s -> %s:\n",
					entry->item.string, info->new_dir.buf);
			}
		}
		fprintf(stderr, "Done.\n");
#endif
		handle_directory_level_conflicts(opt, dir_renames[1],
						 dir_renames[2]);

	} else {
		for (s = 1; s <= 2; s++) {
			dir_renames[s] = xmalloc(sizeof(*dir_renames[s]));
			strmap_init(dir_renames[s], 0);
		}
	}

	ALLOC_GROW(combined->queue,
		   renames->pairs[1].nr + renames->pairs[2].nr,
		   combined->alloc);
	clean &= collect_renames(opt, combined, 1,
				 dir_renames[2], dir_renames[1]);
	clean &= collect_renames(opt, combined, 2,
				 dir_renames[1], dir_renames[2]);
	QSORT(combined->queue, combined->nr, compare_pairs);
	trace2_region_leave("merge", "directory renames", opt->repo);

#ifdef VERBOSE_DEBUG
	printf("=== Processing %d renames ===\n", combined->nr);
#endif
	trace2_region_enter("merge", "process renames", opt->repo);
	clean &= process_renames(opt, combined);
	trace2_region_leave("merge", "process renames", opt->repo);

	/*
	 * Free memory for side[12]_dir_renames.
	 *
	 * In get_directory_renames(), we set side[12].strdup_strings to 0
	 * so that we wouldn't have to make another copy of the old_path
	 * allocated by get_renamed_dir_portion().  But now that we've used
	 * it and have no other references to these strings, it is time to
	 * deallocate them, which we do by just setting strdup_string = 1
	 * before the strmaps are cleared.
	 */
	for (s = 1; s <= 2; s++) {
		strmap_for_each_entry(dir_renames[s], &iter, entry) {
			struct dir_rename_info *info = entry->item.util;
			strbuf_release(&info->new_dir);
		}
		dir_renames[s]->strdup_strings = 1;
		strmap_free(dir_renames[s], 1);
		FREE_AND_NULL(dir_renames[s]);
	}

	goto cleanup; /* collect_renames() handles diff_filepair_cleanup */

 diff_filepair_cleanup:
	/*
	 * Free now unneeded filepairs, which would have been handled
	 * in collect_renames() normally but we're about to skip that
	 * code...
	 */
	for (s = 1; s <= 2; s++) {
		struct diff_queue_struct *side_pairs;
		int i;

		side_pairs = &opt->priv->renames->pairs[s];
		for (i = 0; i < side_pairs->nr; ++i) {
			struct diff_filepair *p = side_pairs->queue[i];
#if USE_MEMORY_POOL
			diff_free_filepair_data(p);
#else
			diff_free_filepair(p);
#endif
		}
	}
 cleanup:
	/* Free memory for renames->pairs[] */
	for (s = 1; s <= 2; s++) {
		free(renames->pairs[s].queue);
		DIFF_QUEUE_CLEAR(&renames->pairs[s]);
	}

	/*
	 * We cannot deallocate combined yet; strings contained in it were
	 * used inside opt->priv->paths, so we need to wait to deallocate it.
	 */
	return clean;
}

static int read_oid_strbuf(struct merge_options *opt,
			   const struct object_id *oid,
			   struct strbuf *dst)
{
	void *buf;
	enum object_type type;
	unsigned long size;
	buf = read_object_file(oid, &type, &size);
	if (!buf)
		return err(opt, _("cannot read object %s"), oid_to_hex(oid));
	if (type != OBJ_BLOB) {
		free(buf);
		return err(opt, _("object %s is not a blob"), oid_to_hex(oid));
	}
	strbuf_attach(dst, buf, size, size + 1);
	return 0;
}

static int blob_unchanged(struct merge_options *opt,
			  const struct version_info *base,
			  const struct version_info *side,
			  const char *path)
{
	struct strbuf basebuf = STRBUF_INIT;
	struct strbuf sidebuf = STRBUF_INIT;
	int ret = 0; /* assume changed for safety */
	const struct index_state *idx = &opt->priv->attr_index;

	if (base->mode != side->mode)
		return 0;
	if (oideq(&base->oid, &side->oid))
		return 1;

	if (read_oid_strbuf(opt, &base->oid, &basebuf) ||
	    read_oid_strbuf(opt, &side->oid, &sidebuf))
		goto error_return;
	/*
	 * Note: binary | is used so that both renormalizations are
	 * performed.  Comparison can be skipped if both files are
	 * unchanged since their sha1s have already been compared.
	 */
	if (renormalize_buffer(idx, path, basebuf.buf, basebuf.len, &basebuf) |
	    renormalize_buffer(idx, path, sidebuf.buf, sidebuf.len, &sidebuf))
		ret = (basebuf.len == sidebuf.len &&
		       !memcmp(basebuf.buf, sidebuf.buf, basebuf.len));

error_return:
	strbuf_release(&basebuf);
	strbuf_release(&sidebuf);
	return ret;
}

struct directory_versions {
	struct string_list versions;
	struct string_list offsets;
	const char *last_directory;
	unsigned last_directory_len;
};

static void write_tree(struct object_id *result_oid,
		       struct string_list *versions,
		       unsigned int offset)
{
	size_t maxlen = 0;
	unsigned int nr = versions->nr - offset;
	struct strbuf buf = STRBUF_INIT;
	struct string_list relevant_entries = STRING_LIST_INIT_NODUP;
	int i;

	/*
	 * We want to sort the last (versions->nr-offset) entries in versions.
	 * Do so by abusing the string_list API a bit: make another string_list
	 * that contains just those entries and then sort them.
	 *
	 * We won't use relevant_entries again and will let it just pop off the
	 * stack, so there won't be allocation worries or anything.
	 */
#ifdef VERBOSE_DEBUG
	printf("Called write_tree with offset = %d\n", offset);
	printf("  versions->nr = %d\n", versions->nr);
#endif
	relevant_entries.items = versions->items + offset;
	relevant_entries.nr = versions->nr - offset;
	string_list_sort(&relevant_entries);

	/* Pre-allocate some space in buf */
	for (i = 0; i < nr; i++) {
		maxlen += strlen(versions->items[offset+i].string) + 34;
	}
	strbuf_reset(&buf);
	strbuf_grow(&buf, maxlen);

	/* Write each entry out to buf */
#ifdef VERBOSE_DEBUG
	printf("  Writing a tree using:\n");
#endif
	for (i = 0; i < nr; i++) {
		struct merged_info *mi = versions->items[offset+i].util;
		struct version_info *ri = &mi->result;
#ifdef VERBOSE_DEBUG
		printf("%06o %s %s\n", ri->mode, versions->items[offset+i].string,
		       oid_to_hex(&ri->oid));
#endif
		strbuf_addf(&buf, "%o %s%c",
			    ri->mode,
			    versions->items[offset+i].string, '\0');
		strbuf_add(&buf, ri->oid.hash, the_hash_algo->rawsz);
	}

	/* Write this object file out, and record in result_oid */
	write_object_file(buf.buf, buf.len, tree_type, result_oid);
	strbuf_release(&buf);
}

static void record_entry_for_tree(struct directory_versions *dir_metadata,
				  const char *path,
				  struct conflict_info *ci)
{
	const char *basename;

	if (ci->merged.is_null)
		/* nothing to record */
		return;

	/*
	 * Note: write_completed_directories() already added
	 * entries for directories to dir_metadata->versions,
	 * so no need to handle ci->filemask == 0 again.
	 */
	if (!ci->merged.clean && !ci->filemask)
		return;

	basename = path + ci->merged.basename_offset;
	assert(strchr(basename, '/') == NULL);
	string_list_append(&dir_metadata->versions,
			   basename)->util = &ci->merged.result;
#ifdef VERBOSE_DEBUG
	printf("Added %s (%s) to dir_metadata->versions (now length %d)\n",
	       basename, path, dir_metadata->versions.nr);
#endif
}

static void write_completed_directories(struct merge_options *opt,
					const char *new_directory_name,
					struct directory_versions *info)
{
	const char *prev_dir;
	struct merged_info *dir_info = NULL;
	unsigned int offset;
	int wrote_a_new_tree = 0;
#ifdef VERBOSE_DEBUG
	int i;
#endif

	if (new_directory_name == info->last_directory)
		return;

	/*
	 * If we are just starting (last_directory is NULL), or last_directory
	 * is a prefix of the current directory, then we can just update
	 * last_directory and record the offset where we started this directory.
	 */
	if (info->last_directory == NULL ||
	    !strncmp(new_directory_name, info->last_directory,
		     info->last_directory_len)) {
		uintptr_t offset = info->versions.nr;

		info->last_directory = new_directory_name;
		info->last_directory_len = strlen(info->last_directory);
		string_list_append(&info->offsets,
				   info->last_directory)->util = (void*)offset;
#ifdef VERBOSE_DEBUG
		for (i = 0; i<info->offsets.nr; i++)
			printf("    %d: %s (%p)\n", i,
			       info->offsets.items[i].string,
			       info->offsets.items[i].string);
		printf("Incremented offsets to %d; new_directory_name=%s; appended (%s, %p, %lu) to offsets)\n",
		       info->offsets.nr, new_directory_name,
		       info->last_directory, info->last_directory, offset);
#endif
		return;
	}

	/*
	 * At this point, ne (next entry) is within a different directory
	 * than the last entry, so we need to create a tree object for all
	 * the entires in info->versions that are under info->last_directory.
	 */
	dir_info = strmap_get(&opt->priv->paths, info->last_directory);
#ifdef VERBOSE_DEBUG
	fprintf(stderr, "*** Looking up '%s'\n", info->last_directory);
#endif
	assert(dir_info);
	offset = (uintptr_t)info->offsets.items[info->offsets.nr-1].util;
	if (offset == info->versions.nr) {
		dir_info->is_null = 1;
	} else {
		dir_info->result.mode = S_IFDIR;
		write_tree(&dir_info->result.oid, &info->versions, offset);
		wrote_a_new_tree = 1;
#ifdef VERBOSE_DEBUG
		printf("New tree:\n");
#endif
	}

	/*
	 * We've now used several entries from info->versions and one entry
	 * from info->offsets, so we get rid of those values.
	 */
	info->offsets.nr--;
	info->versions.nr = offset;
#ifdef VERBOSE_DEBUG
	printf("  Decremented info->offsets.nr to %d\n", info->offsets.nr);
	printf("  Decreased info->versions.nr to %d\n", info->versions.nr);
#endif

	/*
	 * Now we've got an OID for last_directory in dir_info.  We need to
	 * add it to info->versions for it to be part of the computation of
	 * its parent directories' OID.  But first, we have to find out what
	 * its' parent name was and whether that matches the previous
	 * info->offsets or we need to set up a new one.
	 */
	prev_dir = info->offsets.nr == 0 ? NULL :
		   info->offsets.items[info->offsets.nr-1].string;
#ifdef VERBOSE_DEBUG
	printf("Ptr comping %p (%s) to %p (%s)\n",
	       new_directory_name, new_directory_name, prev_dir, prev_dir);
#endif
	if (new_directory_name != prev_dir) {
		uintptr_t c = info->versions.nr;
		string_list_append(&info->offsets,
				   new_directory_name)->util = (void*)c;
#ifdef VERBOSE_DEBUG
		for (i = 0; i<info->offsets.nr; i++)
			printf("    %d: %s (%p)\n", i,
			       info->offsets.items[i].string,
			       info->offsets.items[i].string);
		printf("  Incremented offsets to %d; appended (%s, %p, %lu) to info->offsets\n",
		       info->offsets.nr,
		       new_directory_name, new_directory_name, c);
	} else {
		printf("Comparing '%s' (%p) to '%s' (%p)\n",
		       new_directory_name, new_directory_name,
		       prev_dir, prev_dir);
		assert(!strcmp(new_directory_name, prev_dir));
#endif
	}

	/*
	 * Okay, finally record OID for last_directory in info->versions,
	 * and update last_directory.
	 */
	if (wrote_a_new_tree) {
		const char *dir_name = strrchr(info->last_directory, '/');
		dir_name = dir_name ? dir_name+1 : info->last_directory;
		string_list_append(&info->versions, dir_name)->util = dir_info;
#ifdef VERBOSE_DEBUG
		printf("  Finally, added (%s, dir_info:%s) to info->versions\n",
		       info->last_directory, oid_to_hex(&dir_info->result.oid));
#endif
	}
	info->last_directory = new_directory_name;
	info->last_directory_len = strlen(info->last_directory);
}

/* Per entry merge function */
static void process_entry(struct merge_options *opt,
			  struct string_list_item *e,
			  struct directory_versions *dir_metadata)
{
	char *path = e->string;
	struct conflict_info *ci = e->util;
	int df_file_index = 0;

#ifdef VERBOSE_DEBUG
	printf("Processing %s; filemask = %d\n", e->string, ci->filemask);
#endif
	assert(!ci->merged.clean && !ci->processed);
	ci->processed = 1;
	assert(ci->filemask >= 0 && ci->filemask <= 7);
	if (ci->filemask == 0) {
		/*
		 * This is a placeholder for directories that were recursed
		 * into; nothing to do in this case.
		 */
		return;
	}
	if (ci->df_conflict && ci->merged.result.mode == 0) {
		int i;

		/*
		 * directory no longer in the way, but we do have a file we
		 * need to place here so we need to clean away the "directory
		 * merges to nothing" result.
		 */
		ci->df_conflict = 0;
		assert(ci->filemask != 0);
		ci->merged.clean = 0;
		ci->merged.is_null = 0;
		/* and we want to zero out any directory-related entries */
		ci->match_mask = (ci->match_mask & ~ci->dirmask);
		ci->dirmask = 0;
		for (i=0; i<3; i++) {
			if (ci->filemask & (1 << i))
				continue;
			ci->stages[i].mode = 0;
			oidcpy(&ci->stages[i].oid, &null_oid);
		}
	} else if (ci->df_conflict && ci->merged.result.mode != 0) {
		/*
		 * This started out as a D/F conflict, and the entries in
		 * the competing directory were not removed by the merge as
		 * evidenced by write_completed_directories() writing a value
		 * to ci->merged.result.mode.
		 */
		struct conflict_info *new_ci;
		const char *branch;
		int i;

		assert(ci->merged.result.mode == S_IFDIR);

		/*
		 * If filemask is 1, we can just ignore the file as having
		 * been deleted on both sides.  We do not want to overwrite
		 * ci->merged.result, since it stores the tree for all the
		 * files under it.
		 */
		if (ci->filemask == 1) {
			ci->filemask = 0;
			return;
		}

		/*
		 * This file still exists on at least one side, and we want
		 * the directory to remain here, so we need to move this
		 * path to some new location.
		 */
		new_ci = xcalloc(1, sizeof(*ci));
		/* We don't really want new_ci->merged.result copied, but it'll
		 * be overwritten below so it doesn't matter.  We also don't
		 * want any directory mode/oid values copied, but we'll zero
		 * those out immediately.  We do want the rest of ci copied.
		 */
		memcpy(new_ci, ci, sizeof(*ci));
		new_ci->match_mask = (new_ci->match_mask & ~new_ci->dirmask);
		new_ci->dirmask = 0;
		for (i=0; i<3; i++) {
			if (new_ci->filemask & (1 << i))
				continue;
			/* zero out any entries related to directories */
			new_ci->stages[i].mode = 0;
			oidcpy(&new_ci->stages[i].oid, &null_oid);
		}

		/*
		 * Find out which side this file came from; note that we
		 * cannot just use ci->filemask, because renames could cause
		 * the filemask to go back to 7.  So we use dirmask, then
		 * pick the opposite side's index.
		 */
		df_file_index = (ci->dirmask & (1 << 1)) ? 2 : 1;
		branch = (df_file_index == 1) ? opt->branch1 : opt->branch2;
		path = unique_path(&opt->priv->paths, path, branch);
		strmap_put(&opt->priv->paths, path, new_ci);

		path_msg(opt, path, 0,
			 _("CONFLICT (file/directory): directory in the way "
			   "of %s from %s; moving it to %s instead."),
			 e->string, branch, path);

		/*
		 * Zero out the filemask for the old ci.  At this point, ci
		 * was just an entry for a directory, so we don't need to
		 * do anything more with it.
		 */
		ci->filemask = 0;

		/* Point e and ci at the new entry so it can be worked on */
		e->string = path;
		e->util = new_ci;
		ci = new_ci;
	}
	if (ci->match_mask) {
		ci->merged.clean = 1;
		if (ci->match_mask == 6) {
			/* stages[1] == stages[2] */
			ci->merged.result.mode = ci->stages[1].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
		} else {
			/* determine the mask of the side that didn't match */
			unsigned int othermask = 7 & ~ci->match_mask;
			int side = (othermask == 4) ? 2 : 1;

#ifdef VERBOSE_DEBUG
			printf("filemask: %d, matchmask: %d, othermask: %d, side: %d\n",
			       ci->filemask, ci->match_mask, othermask, side);
#endif
			ci->merged.is_null = (ci->filemask == ci->match_mask);
			ci->merged.result.mode = ci->stages[side].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);

#ifdef VERBOSE_DEBUG
			printf("ci->merged.result.mode: %06o, is_null: %d\n",
			       ci->merged.result.mode, ci->merged.is_null);
#endif
			assert(othermask == 2 || othermask == 4);
			assert(ci->merged.is_null == !ci->merged.result.mode);
		}
	} else if (ci->filemask >= 6 &&
		   (S_IFMT & ci->stages[1].mode) !=
		   (S_IFMT & ci->stages[2].mode)) {
		if (opt->priv->call_depth) {
			ci->merged.clean = 0;
			oidcpy(&ci->merged.result.oid, &ci->stages[0].oid);
			ci->merged.result.mode = ci->stages[0].mode;
			ci->merged.is_null = (ci->merged.result.mode == 0);
		} else {
			/*
			 * Two different items from (file/submodule/symlink)
			 *
			 * Handle by renaming one or both to separate paths.
			 */
			unsigned o_mode = ci->stages[0].mode;
			unsigned a_mode = ci->stages[1].mode;
			unsigned b_mode = ci->stages[2].mode;
			struct conflict_info *new_ci = xmalloc(sizeof(*new_ci));
			char *a_path = NULL, *b_path = NULL;
			int rename_a = 0, rename_b = 0;

			if (S_ISREG(a_mode))
				rename_a = 1;
			else if (S_ISREG(b_mode))
				rename_b = 1;
			else {
				rename_a = 1;
				rename_b = 1;
			}

			path_msg(opt, path, 0,
				 _("CONFLICT (distinct types): %s had different "
				   "types on each side; renamed %s of them so "
				   "each can be recorded somewhere."),
				 path,
				 (rename_a && rename_b) ? _("both") : _("one"));

			ci->merged.clean = 0;
			memcpy(new_ci, ci, sizeof(*new_ci));

			/* Put b into new_ci, removing a from stages */
			new_ci->merged.result.mode = ci->stages[2].mode;
			oidcpy(&new_ci->merged.result.oid, &ci->stages[2].oid);
			new_ci->stages[1].mode = 0;
			oidcpy(&new_ci->stages[1].oid, &null_oid);
			new_ci->filemask = 5;
			if ((S_IFMT & b_mode) != (S_IFMT & o_mode)) {
				new_ci->stages[0].mode = 0;
				oidcpy(&new_ci->stages[0].oid, &null_oid);
				new_ci->filemask = 4;
			}

			/* Leave only a in ci, fixing stages. */
			ci->merged.result.mode = ci->stages[1].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
			ci->stages[2].mode = 0;
			oidcpy(&ci->stages[2].oid, &null_oid);
			ci->filemask = 3;
			if ((S_IFMT & a_mode) != (S_IFMT & o_mode)) {
				ci->stages[0].mode = 0;
				oidcpy(&ci->stages[0].oid, &null_oid);
				ci->filemask = 2;
			}

			/* Insert entries into opt->priv_paths */
			assert(rename_a || rename_b);
			if (rename_a) {
				a_path = unique_path(&opt->priv->paths,
						     path, opt->branch1);
				strmap_put(&opt->priv->paths, a_path, ci);
			}

			if (rename_b)
				b_path = unique_path(&opt->priv->paths,
						     path, opt->branch2);
			else
				b_path = path;
			strmap_put(&opt->priv->paths, b_path, new_ci);

			if (rename_a && rename_b) {
				strmap_remove(&opt->priv->paths, path, 0);
#if !USE_MEMORY_POOL
				/*
				 * We removed path from opt->priv->paths.  path
				 * will also eventually need to be freed, but
				 * it may still be used by e.g.  ci->pathnames.
				 * So, store it in another string-list for now.
				 */
				string_list_append(&opt->priv->paths_to_free,
						   path);
#endif
			}

			/*
			 * Do special handling for b_path since process_entry()
			 * won't be called on it specially.
			 */
			strmap_put(&opt->priv->unmerged, b_path, new_ci);
			record_entry_for_tree(dir_metadata, b_path, new_ci);


			/*
			 * Remaining code for processing this entry should
			 * think in terms of processing a_path.
			 */
			if (a_path)
				path = a_path;
		}
	} else if (ci->filemask >= 6) {
		struct version_info merged_file;
		unsigned clean_merge;
		struct version_info *o = &ci->stages[0];
		struct version_info *a = &ci->stages[1];
		struct version_info *b = &ci->stages[2];

		clean_merge = handle_content_merge(opt, path, o, a, b,
						   ci->pathnames,
						   opt->priv->call_depth * 2,
						   &merged_file);
		ci->merged.clean = clean_merge &&
				   !ci->df_conflict && !ci->path_conflict;
		ci->merged.result.mode = merged_file.mode;
		ci->merged.is_null = (merged_file.mode == 0);
		oidcpy(&ci->merged.result.oid, &merged_file.oid);
#ifdef VERBOSE_DEBUG
		printf("Content merging %s (%s); mode: %06o, hash: %s\n",
		       path, ci->merged.clean ? "clean" : "unclean",
		       ci->merged.result.mode, oid_to_hex(&ci->merged.result.oid));
#endif
		if (clean_merge && ci->df_conflict) {
			assert(df_file_index == 1 || df_file_index == 2);
			ci->filemask = 1 << df_file_index;
			ci->stages[df_file_index].mode = merged_file.mode;
			oidcpy(&ci->stages[df_file_index].oid, &merged_file.oid);
		}
		if (!clean_merge) {
			const char *reason = _("content");
			if (ci->filemask == 6)
				reason = _("add/add");
			if (S_ISGITLINK(merged_file.mode))
				reason = _("submodule");
			path_msg(opt, path, 0,
				 _("CONFLICT (%s): Merge conflict in %s"),
				 reason, path);
		}
	} else if (ci->filemask == 3 || ci->filemask == 5) {
		/* Modify/delete */
		const char *modify_branch, *delete_branch;
		int side = (ci->filemask == 5) ? 2 : 1;
		int index = opt->priv->call_depth ? 0 : side;

		ci->merged.result.mode = ci->stages[index].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[index].oid);
		ci->merged.clean = 0;

		modify_branch = (side == 1) ? opt->branch1 : opt->branch2;
		delete_branch = (side == 1) ? opt->branch2 : opt->branch1;

		if (opt->renormalize &&
		    blob_unchanged(opt, &ci->stages[0], &ci->stages[side],
				   path)) {
			ci->merged.is_null = 1;
			ci->merged.clean = 1;
		} else if (ci->path_conflict &&
			   oideq(&ci->stages[0].oid, &ci->stages[side].oid)) {
			/*
			 * This came from a rename/delete; no action to take,
			 * but avoid printing "modify/delete" conflict notice.
			 */
		} else {
			path_msg(opt, path, 0,
				 _("CONFLICT (modify/delete): %s deleted in %s "
				   "and modified in %s.  Version %s of %s left "
				   "in tree."),
				 path, delete_branch, modify_branch,
				 modify_branch, path);
		}
	} else if ((ci->filemask == 2 || ci->filemask == 4)) {
		/* Added on one side */
		int side = (ci->filemask == 4) ? 2 : 1;
		ci->merged.result.mode = ci->stages[side].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
		ci->merged.clean = !ci->df_conflict && !ci->path_conflict;
	} else if (ci->filemask == 1) {
		/* Deleted on both sides */
		ci->merged.is_null = 1;
		ci->merged.result.mode = 0;
		oidcpy(&ci->merged.result.oid, &null_oid);
		ci->merged.clean = !ci->path_conflict;
	}
	if (!ci->merged.clean)
		strmap_put(&opt->priv->unmerged, path, ci);
	record_entry_for_tree(dir_metadata, path, ci);
}

static void process_entries(struct merge_options *opt,
			    struct object_id *result_oid)
{
	struct hashmap_iter iter;
	struct str_entry *e;
	struct string_list plist = STRING_LIST_INIT_NODUP;
	struct string_list_item *entry;
	struct directory_versions dir_metadata;

	trace2_region_enter("merge", "process_entries setup", opt->repo);
	if (strmap_empty(&opt->priv->paths)) {
		oidcpy(result_oid, opt->repo->hash_algo->empty_tree);
		return;
	}

	/* Hack to pre-allocate plist to the desired size */
	trace2_region_enter("merge", "plist grow", opt->repo);
	ALLOC_GROW(plist.items, strmap_get_size(&opt->priv->paths), plist.alloc);
	trace2_region_leave("merge", "plist grow", opt->repo);

	/* Put every entry from paths into plist, then sort */
	trace2_region_enter("merge", "plist copy", opt->repo);
	strmap_for_each_entry(&opt->priv->paths, &iter, e) {
		string_list_append(&plist, e->item.string)->util = e->item.util;
	}
	trace2_region_leave("merge", "plist copy", opt->repo);

	trace2_region_enter("merge", "plist sort", opt->repo);
	QSORT(plist.items, plist.nr, sort_dirs_next_to_their_children);
	trace2_region_leave("merge", "plist special sort", opt->repo);

	/*
	 * Iterate over the items in both in reverse order, so we can handle
	 * contained directories before the containing directory.
	 */
	string_list_init(&dir_metadata.versions, 0);
	string_list_init(&dir_metadata.offsets, 0);
	dir_metadata.last_directory = NULL;
	dir_metadata.last_directory_len = 0;
	trace2_region_leave("merge", "process_entries setup", opt->repo);
	trace2_region_enter("merge", "processing", opt->repo);
	for (entry = &plist.items[plist.nr-1]; entry >= plist.items; --entry) {
		/*
		 * WARNING: If ci->merged.clean is true, then ci does not
		 * actually point to a conflict_info but a struct merge_info.
		 */
		struct conflict_info *ci = entry->util;

#ifdef VERBOSE_DEBUG
		printf("==>Handling %s\n", entry->string);
#endif

		write_completed_directories(opt, ci->merged.directory_name,
					    &dir_metadata);
		if (ci->merged.clean)
			record_entry_for_tree(&dir_metadata, entry->string, ci);
		else
			process_entry(opt, entry, &dir_metadata);
	}
	trace2_region_leave("merge", "processing", opt->repo);
	trace2_region_enter("merge", "finalize", opt->repo);
	if (dir_metadata.offsets.nr != 1 ||
	    (uintptr_t)dir_metadata.offsets.items[0].util != 0) {
		printf("dir_metadata.offsets.nr = %d (should be 1)\n",
		       dir_metadata.offsets.nr);
		printf("dir_metadata.offsets.items[0].util = %lu (should be 0)\n",
		       (uintptr_t)dir_metadata.offsets.items[0].util);
		fflush(stdout);
		BUG("dir_metadata accounting completely off; shouldn't happen");
	}
	write_tree(result_oid, &dir_metadata.versions, 0);
	string_list_clear(&plist, 0);
	string_list_clear(&dir_metadata.versions, 0);
	string_list_clear(&dir_metadata.offsets, 0);
	trace2_region_leave("merge", "finalize", opt->repo);
}

static int checkout(struct merge_options *opt,
		    struct tree *prev,
		    struct tree *next)
{
	/* Switch the index/working copy from old to new */
	int ret;
	struct tree_desc trees[2];
	struct unpack_trees_options unpack_opts;

	memset(&unpack_opts, 0, sizeof(unpack_opts));
	unpack_opts.head_idx = -1;
	unpack_opts.src_index = opt->repo->index;
	unpack_opts.dst_index = opt->repo->index;

#ifdef VERBOSE_DEBUG
	printf("Switching over to tree %s\n", oid_to_hex(&next->object.oid));
#endif
	setup_unpack_trees_porcelain(&unpack_opts, "merge");

	/* FIXME: Do I need to refresh the index?? */
	refresh_index(opt->repo->index, REFRESH_QUIET, NULL, NULL, NULL);

	/*
	 * FIXME: Isn't checking for an unmerged_index unnecessary overhead if
	 * sequencer/merge check that index is clean first?
	 */
	if (unmerged_index(opt->repo->index)) {
		error(_("you need to resolve your current index first"));
		return -1;
	}

	/* 2-way merge to the new branch */
	unpack_opts.update = 1;
	unpack_opts.merge = 1;
	unpack_opts.quiet = 0; /* FIXME: sequencer might want quiet? */
	unpack_opts.verbose_update = (opt->verbosity > 2);
	unpack_opts.fn = twoway_merge;
	if (1/* FIXME: opts->overwrite_ignore*/) {
		unpack_opts.dir = xcalloc(1, sizeof(*unpack_opts.dir));
		unpack_opts.dir->flags |= DIR_SHOW_IGNORED;
		setup_standard_excludes(unpack_opts.dir);
	}
	parse_tree(prev);
	init_tree_desc(&trees[0], prev->buffer, prev->size);
	parse_tree(next);
	init_tree_desc(&trees[1], next->buffer, next->size);

	ret = unpack_trees(2, trees, &unpack_opts);
#ifdef VERBOSE_DEBUG
	printf("ret from unpack_trees: %d\n", ret);
#endif
	clear_unpack_trees_porcelain(&unpack_opts);
#ifdef VERBOSE_DEBUG
	printf("after clear_unpack_trees_porcelain()\n");
#endif
	dir_clear(unpack_opts.dir);
	FREE_AND_NULL(unpack_opts.dir);
	return ret;
}

static int record_unmerged_index_entries(struct merge_options *opt,
					 struct index_state *index,
					 struct strmap *paths,
					 struct strmap *unmerged)
{
	struct hashmap_iter iter;
	struct str_entry *e;
	struct checkout state = CHECKOUT_INIT;
	int errs = 0;
	int original_cache_nr;

	if (strmap_empty(unmerged))
		return 0;

#ifdef VERBOSE_DEBUG
	for (int i=0; i<index->cache_nr; ++i) {
		fprintf(stderr, "  cache[%d] = (%s, %s, %o, %d, %u, %d)\n",
		       i,
		       index->cache[i]->name,
		       oid_to_hex(&index->cache[i]->oid),
		       index->cache[i]->ce_mode,
		       ce_stage(index->cache[i]),
		       index->cache[i]->ce_flags,
		       index->cache[i]->index);
	}
	fprintf(stderr, "... AFTER ...\n");
#endif

	/* If any entries have skip_worktree set, we'll have to check 'em out */
	state.force = 1;
	state.quiet = 1;
	state.refresh_cache = 1;
	state.istate = index;
	original_cache_nr = index->cache_nr;

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(unmerged, &iter, e) {
		const char *path = e->item.string;
		struct conflict_info *ci = e->item.util;
		int pos;
		struct cache_entry *ce;
		int i;

		/*
		 * The index will already have a stage=0 entry for this path,
		 * because we created an as-merged-as-possible version of the
		 * file and checkout() moved the working copy and index over
		 * to that version.
		 *
		 * However, previous iterations through this loop will have
		 * added unstaged entries to the end of the cache which
		 * ignore the standard alphabetical ordering of cache
		 * entries and break invariants needed for index_name_pos()
		 * to work.  However, we know the entry we want is before
		 * those appended cache entries, so do a temporary swap on
		 * cache_nr to only look through entries of interest.
		 */
		SWAP(index->cache_nr, original_cache_nr);
		pos = index_name_pos(index, path, strlen(path));
		SWAP(index->cache_nr, original_cache_nr);
#ifdef VERBOSE_DEBUG
		fprintf(stderr, "Found pos %d for %s\n", pos, path);
#endif
		if (pos < 0) {
			if (ci->filemask == 1)
				cache_tree_invalidate_path(index, path);
			else
				BUG("Unmerged %s but nothing in basic working tree or index; this shouldn't happen", path);
		} else {
			ce = index->cache[pos];

			/*
			 * If this cache entry had the skip_worktree bit set,
			 * then it isn't present in the working tree..but since
			 * it corresponds to a merge conflict we need it to be.
			 */
			if (ce_skip_worktree(ce)) {
				struct stat st;

				if (!lstat(path, &st)) {
					char *new_name = unique_path(paths,
								     path,
								     "cruft");

					path_msg(opt, path, 1,
						 _("Note: %s not up to date and in way of checking out conflicted version; old copy renamed to %s"),
						 path, new_name);
					errs |= rename(path, new_name);
					free(new_name);
				}
				errs |= checkout_entry(ce, &state, NULL, NULL);
			}

			/*
			 * Mark this cache entry for removal and instead add
			 * new stage > 0 entries corresponding to the
			 * conflicts.  We just add the new cache entries to
			 * the end and re-sort later to avoid O(NM) memmove'd
			 * entries (N=num cache entries, M=num unmerged
			 * entries) if there are several unmerged entries.
			 */
			ce->ce_flags |= CE_REMOVE;
		}

		for (i = 0; i < 3; i++) {
			struct version_info *vi;
			if (!(ci->filemask & (1ul << i)))
				continue;
			vi = &ci->stages[i];
			ce = make_cache_entry(index, vi->mode, &vi->oid,
					      path, i+1, 0);
			add_index_entry(index, ce, ADD_CACHE_JUST_APPEND);
		}
	}

	/*
	 * Remove the unused cache entries (and invalidate the relevant
	 * cache-trees), then sort the index entries to get the unmerged
	 * entries we added to the end into their right locations.
	 */
	remove_marked_cache_entries(index, 1);
	QSORT(index->cache, index->cache_nr, cmp_cache_name_compare);
#ifdef VERBOSE_DEBUG
	for (int i=0; i<index->cache_nr; ++i) {
		fprintf(stderr, "  cache[%d] = (%s, %s, %o, %d, %u, %d)\n",
		       i,
		       index->cache[i]->name,
		       oid_to_hex(&index->cache[i]->oid),
		       index->cache[i]->ce_mode,
		       ce_stage(index->cache[i]),
		       index->cache[i]->ce_flags,
		       index->cache[i]->index);
	}
#endif

	return errs;
}

static void reset_maps(struct merge_options_internal *opt, int reinitialize);

/*
 * Originally from merge_trees_internal(); heavily adapted, though.
 */
static void merge_ort_nonrecursive_internal(struct merge_options *opt,
					    struct tree *merge_base,
					    struct tree *head,
					    struct tree *merge,
					    struct merge_result *result)
{
	struct diff_queue_struct pairs;
	struct object_id working_tree_oid;

	if (opt->subtree_shift) {
		merge = shift_tree_object(opt->repo, head, merge,
					  opt->subtree_shift);
		merge_base = shift_tree_object(opt->repo, head, merge_base,
					       opt->subtree_shift);
	}

redo:
	trace2_region_enter("merge", "collect_merge_info", opt->repo);
	if (collect_merge_info(opt, merge_base, head, merge) != 0) {
		err(opt, _("collecting merge info for trees %s and %s failed"),
		    oid_to_hex(&head->object.oid),
		    oid_to_hex(&merge->object.oid));
		result->clean = -1;
		return;
	}
	trace2_region_leave("merge", "collect_merge_info", opt->repo);

	trace2_region_enter("merge", "renames", opt->repo);
	result->clean = detect_and_process_renames(opt, &pairs, merge_base,
						   head, merge);
	trace2_region_leave("merge", "renames", opt->repo);
	if (opt->priv->renames->redo_after_renames == 2) {
		trace2_region_enter("merge", "reset_maps", opt->repo);
		reset_maps(opt->priv, 1);
		trace2_region_leave("merge", "reset_maps", opt->repo);
		goto redo;
	}

	trace2_region_enter("merge", "process_entries", opt->repo);
	process_entries(opt, &working_tree_oid);
	trace2_region_leave("merge", "process_entries", opt->repo);

	/* FIXME: Only show this if showing hints, and only after other output */
	diff_warn_rename_limit("merge.renamelimit",
			       opt->priv->needed_rename_limit, 0);

	trace2_region_enter("merge", "cleanup", opt->repo);
	/* unmerged entries => unclean */
	result->clean &= strmap_empty(&opt->priv->unmerged);

	if (pairs.nr) {
		int i;
		for (i = 0; i < pairs.nr; i++)
#if USE_MEMORY_POOL
			diff_free_filepair_data(pairs.queue[i]);
#else
			diff_free_filepair(pairs.queue[i]);
#endif
		free(pairs.queue);
	}
	trace2_region_leave("merge", "cleanup", opt->repo);

	/* Set return values */
	result->tree = parse_tree_indirect(&working_tree_oid);
	if (!opt->priv->call_depth) {
		result->priv = opt->priv;
		opt->priv = NULL;
	}
}

/*
 * Originally from merge_recursive_internal(); somewhat adapted, though.
 */
static void merge_ort_internal(struct merge_options *opt,
			       struct commit_list *merge_bases,
			       struct commit *h1,
			       struct commit *h2,
			       struct merge_result *result)
{
	struct commit_list *iter;
	struct commit *merged_merge_bases;
	const char *ancestor_name;
	struct strbuf merge_base_abbrev = STRBUF_INIT;
#ifdef VERBOSE_DEBUG
	struct strbuf sb;
	unsigned cnt;
#endif

#ifdef VERBOSE_DEBUG
	strbuf_addstr(&sb, _("Merging:"));
	format_commit(&sb, 2 * opt->priv->call_depth, h1);
	format_commit(&sb, 2 * opt->priv->call_depth, h2);
	printf("%s", sb.buf);
	strbuf_reset(&sb);
#endif

	if (!merge_bases) {
		merge_bases = get_merge_bases(h1, h2);
		merge_bases = reverse_commit_list(merge_bases);
	}

#ifdef VERBOSE_DEBUG
	strbuf_addstr(&sb, _("Merging:\n"));
	cnt = commit_list_count(merge_bases);
	strbuf_addf(&sb, Q_("found %u common ancestor:",
			    "found %u common ancestors:", cnt), cnt);
	for (iter = merge_bases; iter; iter = iter->next)
		format_commit(&sb, 2 * opt->priv->call_depth, iter->item);
	printf("%s", sb.buf);
	strbuf_reset(&sb);
#endif

	merged_merge_bases = pop_commit(&merge_bases);
	if (merged_merge_bases == NULL) {
		/* if there is no common ancestor, use an empty tree */
		struct tree *tree;

		tree = lookup_tree(opt->repo, opt->repo->hash_algo->empty_tree);
		merged_merge_bases = make_virtual_commit(opt->repo, tree,
							 "ancestor");
		ancestor_name = "empty tree";
	} else if (opt->ancestor && !opt->priv->call_depth) {
		ancestor_name = opt->ancestor;
	} else if (merge_bases) {
		ancestor_name = "merged common ancestors";
	} else {
		strbuf_add_unique_abbrev(&merge_base_abbrev,
					 &merged_merge_bases->object.oid,
					 DEFAULT_ABBREV);
		ancestor_name = merge_base_abbrev.buf;
	}

	for (iter = merge_bases; iter; iter = iter->next) {
		const char *saved_b1, *saved_b2;
		struct commit *prev = merged_merge_bases;

		opt->priv->call_depth++;
		/*
		 * When the merge fails, the result contains files
		 * with conflict markers. The cleanness flag is
		 * ignored (unless indicating an error), it was never
		 * actually used, as result of merge_trees has always
		 * overwritten it: the committed "conflicts" were
		 * already resolved.
		 */
		saved_b1 = opt->branch1;
		saved_b2 = opt->branch2;
		opt->branch1 = "Temporary merge branch 1";
		opt->branch2 = "Temporary merge branch 2";
		merge_ort_internal(opt, NULL, prev, iter->item, result);
		if (result->clean < 0)
			return;
		opt->branch1 = saved_b1;
		opt->branch2 = saved_b2;
		opt->priv->call_depth--;

		merged_merge_bases = make_virtual_commit(opt->repo,
							 result->tree,
							 "merged tree");
		commit_list_insert(prev, &merged_merge_bases->parents);
		commit_list_insert(iter->item,
				   &merged_merge_bases->parents->next);

		reset_maps(opt->priv, 1);
	}

	opt->ancestor = ancestor_name;
	merge_ort_nonrecursive_internal(opt,
					repo_get_commit_tree(opt->repo,
							     merged_merge_bases),
					repo_get_commit_tree(opt->repo, h1),
					repo_get_commit_tree(opt->repo, h2),
					result);
	strbuf_release(&merge_base_abbrev);
	opt->ancestor = NULL;  /* avoid accidental re-use of opt->ancestor */
}

static void merge_start(struct merge_options *opt, struct merge_result *result)
{
	/* Sanity checks on opt */
	trace2_region_enter("merge", "sanity checks", opt->repo);
	assert(opt->repo);

	assert(opt->branch1 && opt->branch2);

	assert(opt->detect_renames >= -1 &&
	       opt->detect_renames <= DIFF_DETECT_COPY);
	assert(opt->detect_directory_renames >= MERGE_DIRECTORY_RENAMES_NONE &&
	       opt->detect_directory_renames <= MERGE_DIRECTORY_RENAMES_TRUE);
	assert(opt->rename_limit >= -1);
	assert(opt->rename_score >= 0 && opt->rename_score <= MAX_SCORE);
	assert(opt->show_rename_progress >= 0 && opt->show_rename_progress <= 1);

	/* FIXME: This is a super hacky way to "default" to histogram diff */
	opt->xdl_opts = DIFF_WITH_ALG(opt, HISTOGRAM_DIFF);
	assert(opt->xdl_opts >= 0);
	assert(opt->recursive_variant >= MERGE_VARIANT_NORMAL &&
	       opt->recursive_variant <= MERGE_VARIANT_THEIRS);

	/* verbosity, buffer_output, and obuf are ignored */
	assert(opt->verbosity >= 0 && opt->verbosity <= 5);
	assert(opt->buffer_output <= 2);
	assert(opt->obuf.len == 0);

	assert(opt->priv == NULL);
	if (result->priv) {
		/*
		 * result->priv non-NULL means results from previous run; do a
		 * few sanity checks that user didn't just give us
		 * uninitialized garbage.
		 */
		opt->priv = result->priv;
		result->priv = NULL;
		assert(opt->priv->call_depth == 0);
		assert(!opt->priv->toplevel_dir ||
		       0 == strlen(opt->priv->toplevel_dir));
	}
	trace2_region_leave("merge", "sanity checks", opt->repo);

	if (opt->priv) {
		trace2_region_enter("merge", "reset_maps", opt->repo);
		reset_maps(opt->priv, 1);
		trace2_region_leave("merge", "reset_maps", opt->repo);
	} else {
		struct rename_info *renames;
		int i;
#if USE_MEMORY_POOL
		struct mem_pool *pool;
#endif

		trace2_region_enter("merge", "allocate/init", opt->repo);
		opt->priv = xcalloc(1, sizeof(*opt->priv));
#if USE_MEMORY_POOL
		pool = &opt->priv->pool;
		mem_pool_init(pool, 0);
#endif
		opt->priv->renames = renames = xcalloc(1, sizeof(*renames));
		for (i=1; i<3; i++) {
#if USE_MEMORY_POOL
			strintmap_init_with_mem_pool(&renames->relevant_sources[i],
						     pool, 0);
			strintmap_init_with_mem_pool(&renames->dirs_removed[i],
						     pool, 0);
			strintmap_init_with_mem_pool(&renames->possible_trivial_merges[i],
						  pool, 0);
			strset_init_with_mem_pool(&renames->target_dirs[i],
						  pool, 1);
#else
			strintmap_init(&renames->relevant_sources[i], 0);
			strintmap_init(&renames->dirs_removed[i], 0);
			strintmap_init(&renames->possible_trivial_merges[i], 0);
			strset_init(&renames->target_dirs[i], 1);
#endif
			strmap_init(&renames->cached_pairs[i], 1);
			strset_init(&renames->cached_target_names[i], 0);
			renames->trivial_merges_okay[i] = 1; /* 1 == maybe */
		}

#if USE_MEMORY_POOL
		strmap_init_with_mem_pool(&opt->priv->paths, pool, 0);
		strmap_init_with_mem_pool(&opt->priv->unmerged, pool, 0);
#else
		strmap_init(&opt->priv->paths, 0);
		strmap_init(&opt->priv->unmerged, 0);
		/*
		 * Although we initialize opt->priv->paths_to_free and
		 * opt->priv->paths with strdup_strings = 0, that's just to
		 * avoid making an extra copy of an allocated string.  Both
		 * of these store strings that we will later need to free.
		 */
		string_list_init(&opt->priv->paths_to_free, 0);
#endif
		strmap_init(&opt->priv->output, 1);
		trace2_region_leave("merge", "allocate/init", opt->repo);
	}

	if (opt->renormalize)
		git_attr_set_direction(GIT_ATTR_CHECKOUT);
}

void merge_switch_to_result(struct merge_options *opt,
			    struct tree *head,
			    struct merge_result *result,
			    int update_worktree_and_index,
			    int display_update_msgs)
{
	assert(opt->priv == NULL);
	if (result->clean >= 0 && update_worktree_and_index) {
		struct merge_options_internal *opti = result->priv;
		const char *filename;
		FILE *fp;

		trace2_region_enter("merge", "checkout", opt->repo);
		if (checkout(opt, head, result->tree)) {
			/* failure to function */
			result->clean = -1;
			return;
		}
		trace2_region_leave("merge", "checkout", opt->repo);

		trace2_region_enter("merge", "record_unmerged", opt->repo);
		if (record_unmerged_index_entries(opt, opt->repo->index,
						  &opti->paths,
						  &opti->unmerged)) {
			/* failure to function */
			result->clean = -1;
			return;
		}
		trace2_region_leave("merge", "record_unmerged", opt->repo);

		trace2_region_enter("merge", "write_auto_merge", opt->repo);
		filename = git_path_auto_merge(opt->repo);
		fp = xfopen(filename, "w");
		fprintf(fp, "%s\n", oid_to_hex(&result->tree->object.oid));
		fclose(fp);
		trace2_region_leave("merge", "write_auto_merge", opt->repo);
	}

	if (display_update_msgs) {
		struct merge_options_internal *opti = result->priv;
		struct hashmap_iter iter;
		struct str_entry *e;
		struct string_list olist = STRING_LIST_INIT_NODUP;
		int i;

		trace2_region_enter("merge", "display messages", opt->repo);

		/* Hack to Pre-allocate olist to the desired size */
		ALLOC_GROW(olist.items, strmap_get_size(&opti->output),
			   olist.alloc);

		/* Put every entry from output into olist, then sort */
		strmap_for_each_entry(&opti->output, &iter, e) {
			string_list_append(&olist, e->item.string)->util = e->item.util;
		}
		string_list_sort(&olist);

		/* Iterate over the items, printing them */
		for (i = 0; i < olist.nr; ++i) {
			struct strbuf *sb = olist.items[i].util;

			printf("%s", sb->buf);
		}
		string_list_clear(&olist, 0);

		trace2_region_leave("merge", "display messages", opt->repo);
	}

	merge_finalize(opt, result);
}

void merge_finalize(struct merge_options *opt,
		    struct merge_result *result)
{
	struct merge_options_internal *opti = result->priv;

	if (opt->renormalize)
		git_attr_set_direction(GIT_ATTR_CHECKIN);
	assert(opt->priv == NULL);

	reset_maps(opti, 0);
	FREE_AND_NULL(opti->renames);
	FREE_AND_NULL(opti);
}

static void reset_maps(struct merge_options_internal *opti, int reinitialize)
{
	struct rename_info *renames = opti->renames;
	int i;
	void (*strmap_func)(struct strmap *, int) =
		reinitialize ? strmap_clear : strmap_free;
	void (*strintmap_func)(struct strintmap *) =
		reinitialize ? strintmap_clear : strintmap_free;
	void (*strset_func)(struct strset *) =
		reinitialize ? strset_clear : strset_free;

	/*
	 * We marked opti->paths with strdup_strings = 0, so that we
	 * wouldn't have to make another copy of the fullpath created by
	 * make_traverse_path from setup_path_info().  But, now that we've
	 * used it and have no other references to these strings, it is time
	 * to deallocate them, which we do by just setting strdup_string = 1
	 * before the strmaps are cleared.
	 */
	opti->paths.strdup_strings = 1;
#if USE_MEMORY_POOL
	strmap_func(&opti->paths, 0);
#else
	strmap_func(&opti->paths, 1);
#endif
	opti->paths.strdup_strings = 0;

#if !USE_MEMORY_POOL
	/* opti->paths_to_free is similar to opti->paths. */
	opti->paths_to_free.strdup_strings = 1;
	string_list_clear(&opti->paths_to_free, 0);
	opti->paths_to_free.strdup_strings = 0;
#endif

	if (!reinitialize) {
		struct hashmap_iter iter;
		struct str_entry *e;

		/* Put every entry from output into olist, then sort */
		strmap_for_each_entry(&opti->output, &iter, e) {
			struct strbuf *sb = e->item.util;
			strbuf_release(sb);
			/*
			 * We don't need to free(sb) here; we could pass
			 * free_util=1 when free'ing opti->output instead, but
			 * that's require another strmap_for_each_entry() loop,
			 * and it's cheaper to free it here while we have it.
			 */
			free(sb);
		}
		strmap_free(&opti->output, 0);
	}
	if (opti->attr_index.cache_nr) /* true iff opt->renormalize */
		discard_index(&opti->attr_index);

	/*
	 * All strings and util fields in opti->unmerged are a subset
	 * of those in opti->paths.  We don't want to deallocate
	 * anything twice, so we don't set strdup_strings and we pass 0 for
	 * free_util.
	 */
	strmap_func(&opti->unmerged, 0);

	/* Free memory used by various renames maps */
	for (i=1; i<3; ++i) {
		strintmap_func(&renames->relevant_sources[i]);
		strintmap_func(&renames->dirs_removed[i]);
		strintmap_func(&renames->possible_trivial_merges[i]);
		strset_func(&renames->target_dirs[i]);
		renames->trivial_merges_okay[i] = 1; /* 1 == maybe */
		if (i != renames->cached_pairs_valid_side &&
		    -1 != renames->cached_pairs_valid_side) {
			strset_func(&renames->cached_target_names[i]);
			strmap_func(&renames->cached_pairs[i], 1);
		}
	}
	renames->cached_pairs_valid_side = 0;
	renames->dir_rename_mask = 0;

#if USE_MEMORY_POOL
	mem_pool_discard(&opti->pool, 0);
#endif

	/* Clean out callback_data as well. */
	FREE_AND_NULL(renames->callback_data);
	renames->callback_data_nr = renames->callback_data_alloc = 0;
}

static void merge_check_renames_reusable(struct merge_options *opt,
					 struct merge_result *result,
					 struct tree *merge_base,
					 struct tree *side1,
					 struct tree *side2)
{
	struct rename_info *renames;
	struct tree **merge_trees;
	struct hashmap_iter iter;
	struct str_entry *entry;
	int s;

	if (!result->priv)
		return;

	renames = ((struct merge_options_internal *)result->priv)->renames;
	merge_trees = renames->merge_trees;
	/* merge_trees[0..2] will only be NULL if result->priv is */
	assert(merge_trees[0] && merge_trees[1] && merge_trees[2]);

	/* Check if we meet a condition for re-using cached_pairs */
	if (     oideq(&merge_base->object.oid, &merge_trees[2]->object.oid) &&
		 oideq(     &side1->object.oid, &result->tree->object.oid))
		renames->cached_pairs_valid_side = 1;
	else if (oideq(&merge_base->object.oid, &merge_trees[1]->object.oid) &&
		 oideq(     &side2->object.oid, &result->tree->object.oid))
		renames->cached_pairs_valid_side = 2;
	else
		renames->cached_pairs_valid_side = 0;

	/* If we can't re-use the cache pairs, return now */
	if (!renames->cached_pairs_valid_side)
		return;

	/* Populate cache_target_names from cached_pairs */
	s = renames->cached_pairs_valid_side;
	strmap_for_each_entry(&renames->cached_pairs[s], &iter, entry)
		if (entry->item.util)
			strset_add(&renames->cached_target_names[s],
				   entry->item.util);
}

void merge_inmemory_nonrecursive(struct merge_options *opt,
				 struct tree *merge_base,
				 struct tree *side1,
				 struct tree *side2,
				 struct merge_result *result)
{
	trace2_region_enter("merge", "inmemory_nonrecursive", opt->repo);
	assert(opt->ancestor != NULL);

	trace2_region_enter("merge", "merge_start", opt->repo);
	merge_check_renames_reusable(opt, result, merge_base, side1, side2);
	merge_start(opt, result);
	/*
	 * Record the trees used in this merge, so if there's a next merge in
	 * a cherry-pick or rebase sequence it might be able to take advantage
	 * of the cached_pairs in that next merge.
	 */
	opt->priv->renames->merge_trees[0] = merge_base;
	opt->priv->renames->merge_trees[1] = side1;
	opt->priv->renames->merge_trees[2] = side2;
	trace2_region_leave("merge", "merge_start", opt->repo);

	merge_ort_nonrecursive_internal(opt, merge_base, side1, side2, result);
	trace2_region_leave("merge", "inmemory_nonrecursive", opt->repo);
}

void merge_inmemory_recursive(struct merge_options *opt,
			      struct commit_list *merge_bases,
			      struct commit *side1,
			      struct commit *side2,
			      struct merge_result *result)
{
	trace2_region_enter("merge", "inmemory_recursive", opt->repo);
	assert(opt->ancestor == NULL ||
	       !strcmp(opt->ancestor, "constructed merge base"));

	trace2_region_enter("merge", "merge_start", opt->repo);
	merge_start(opt, result);
	trace2_region_leave("merge", "merge_start", opt->repo);

	merge_ort_internal(opt, merge_bases, side1, side2, result);
	trace2_region_leave("merge", "inmemory_recursive", opt->repo);
}
