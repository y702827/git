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
#include "unpack-trees.h"
#include "xdiff-interface.h"

#if 0
#define VERBOSE_DEBUG
#endif

struct rename_info {
	unsigned possible_dir_renames:1;
	/* For the next eight vars, the 0th entry is ignored and unused */
	struct diff_queue_struct pairs[3];
	struct diff_queue_struct renames[3];
	struct strmap src_basenames[3]; /* basename->unsorted fullnames list */
	struct strmap dst_basenames[3]; /* basename->unsorted fullnames list */
	struct oidmap src_hashes[3];    /* oid->unsorted fullnames list */
	struct oidmap dst_hashes[3];    /* oid->unsorted fullnames list */
	int nr_sources[3];
	int nr_targets[3];
	/*
	 * dir_rename_mask:
	 *   0: optimization removing unmodified potential rename source okay
	 *   2 or 4: optimization okay, but must check for files added to dir
	 *   7: optimization forbidden; need rename source in case of dir rename
	 */
	unsigned dir_rename_mask:3;
};

struct merge_options_internal {
	struct strmap paths;    /* maps path -> (merged|conflict)_info */
	struct strmap unmerged; /* maps path -> conflict_info */
	struct string_list paths_to_free; /* list of strings to free */
	struct rename_info *renames;
	const char *current_dir_name;
	char *toplevel_dir; /* see merge_info.directory_name comment */
	int call_depth;
	int needed_rename_limit;
};

struct version_info {
	unsigned short mode;
	struct object_id oid;
};

struct merged_info {
	struct version_info result;
	 /*
	  * Containing directory name.  Note that we assume directory_name is
	  * constructed such that
	  *    strcmp(dir1_name, dir2_name) == 0 iff dir1_name == dir2_name,
	  * i.e. string equality is equivalent to pointer equality.  For this
	  * to hold, we have to be careful setting directory_name.
	  */
	const char *directory_name;
	size_t basename_offset;
	unsigned is_null:1;
	unsigned clean:1;
};

#if 0
enum rename_conflict_type {
	RENAME_NONE = 0,
	RENAME_VIA_DIR,
	RENAME_ADD,
	RENAME_ADD_DELETE,
	RENAME_DELETE,
	// RENAME_ONE_FILE_TO_ONE,  // FIXME
	RENAME_ONE_FILE_TO_TWO,
	RENAME_TWO_FILES_TO_ONE
};
#endif

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

static int show(struct merge_options *opt, int v)
{
	return (!opt->priv->call_depth && opt->verbosity >= v) ||
		opt->verbosity >= 5;
}

static void flush_output(struct merge_options *opt)
{
	if (opt->buffer_output < 2 && opt->obuf.len) {
		fputs(opt->obuf.buf, stdout);
		strbuf_reset(&opt->obuf);
	}
}

__attribute__((format (printf, 3, 4)))
static void output(struct merge_options *opt, int v, const char *fmt, ...)
{
	va_list ap;

	if (!show(opt, v))
		return;

	strbuf_addchars(&opt->obuf, ' ', opt->priv->call_depth * 2);

	va_start(ap, fmt);
	strbuf_vaddf(&opt->obuf, fmt, ap);
	va_end(ap);

	strbuf_addch(&opt->obuf, '\n');
	if (!opt->buffer_output)
		flush_output(opt);
}

static int err(struct merge_options *opt, const char *err, ...)
{
	va_list params;

	if (opt->buffer_output < 2)
		flush_output(opt);
	else {
		strbuf_complete(&opt->obuf, '\n');
		strbuf_addstr(&opt->obuf, "error: ");
	}
	va_start(params, err);
	strbuf_vaddf(&opt->obuf, err, params);
	va_end(params);
	if (opt->buffer_output > 1)
		strbuf_addch(&opt->obuf, '\n');
	else {
		error("%s", opt->obuf.buf);
		strbuf_reset(&opt->obuf);
	}

	return -1;
}

static void output_commit_title(struct merge_options *opt, struct commit *commit)
{
	struct merge_remote_desc *desc;

	strbuf_addchars(&opt->obuf, ' ', opt->priv->call_depth * 2);
	desc = merge_remote_util(commit);
	if (desc)
		strbuf_addf(&opt->obuf, "virtual %s\n", desc->name);
	else {
		strbuf_add_unique_abbrev(&opt->obuf, &commit->object.oid,
					 DEFAULT_ABBREV);
		strbuf_addch(&opt->obuf, ' ');
		if (parse_commit(commit) != 0)
			strbuf_addstr(&opt->obuf, _("(bad commit)\n"));
		else {
			const char *title;
			const char *msg = get_commit_buffer(commit, NULL);
			int len = find_commit_subject(msg, &title);
			if (len)
				strbuf_addf(&opt->obuf, "%.*s\n", len, title);
			unuse_commit_buffer(commit, msg);
		}
	}
	flush_output(opt);
}

static void print_commit(struct commit *commit)
{
	struct strbuf sb = STRBUF_INIT;
	struct pretty_print_context ctx = {0};
	ctx.date_mode.type = DATE_NORMAL;
	format_commit_message(commit, " %h: %m %s", &sb, &ctx);
	fprintf(stderr, "%s\n", sb.buf);
	strbuf_release(&sb);
}

static inline int merge_detect_rename(struct merge_options *opt)
{
	/*
	 * We do not have logic to handle the detection of copies.  In
	 * fact, it may not even make sense to add such logic: would we
	 * really want a change to a base file to be propagated through
	 * multiple other files by a merge?
	 */

	/* FIXME: More words here... */
	return !!opt->detect_renames;
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

static int string_list_df_name_compare(const char *one, const char *two)
{
	int onelen = strlen(one);
	int twolen = strlen(two);
	/*
	 * Here we only care that entries for D/F conflicts are adjacent,
	 * in particular with the file of the D/F conflict appearing before
	 * files below the corresponding directory.  The order of the rest
	 * of the list is irrelevant for us.
	 *
	 * To achieve this, we sort with df_name_compare and provide the
	 * mode S_IFDIR so that D/F conflicts will sort correctly.  We use
	 * the mode S_IFDIR for everything else for simplicity, since in
	 * other cases any changes in their order due to sorting cause no
	 * problems for us.
	 */
	int cmp = df_name_compare(one, onelen, S_IFDIR,
				  two, twolen, S_IFDIR);
	/*
	 * Now that 'foo' and 'foo/bar' compare equal, we have to make sure
	 * that 'foo' comes before 'foo/bar'.
	 */
	if (cmp)
		return cmp;
	return onelen - twolen;
}

/***** End copy-paste static functions from merge-recursive.c *****/

static struct traverse_trees_callback_data {
	unsigned long mask;
	unsigned long dirmask;
	struct name_entry names[3];
} *callback_data;
static int callback_data_nr, callback_data_alloc;
static char *callback_data_traverse_path;

static int traverse_trees_wrapper_callback(int n,
					   unsigned long mask,
					   unsigned long dirmask,
					   struct name_entry *names,
					   struct traverse_info *info)
{
	struct merge_options *opt = info->data;
	unsigned filemask = mask & ~dirmask;

	assert(n==3);

	if (!callback_data_traverse_path)
		callback_data_traverse_path = xstrdup(info->traverse_path);

	if (filemask == opt->priv->renames->dir_rename_mask)
		opt->priv->renames->dir_rename_mask = 0x07;

	ALLOC_GROW(callback_data, callback_data_nr + 1, callback_data_alloc);
	callback_data[callback_data_nr].mask = mask;
	callback_data[callback_data_nr].dirmask = dirmask;
	memcpy(callback_data[callback_data_nr].names, names, 3*sizeof(*names));
	callback_data_nr++;

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

	assert(opt->priv->renames->dir_rename_mask == 2 ||
	       opt->priv->renames->dir_rename_mask == 4);

	old_callback_data_traverse_path = callback_data_traverse_path;
	old_fn = info->fn;
	old_offset = callback_data_nr;

	callback_data_traverse_path = NULL;
	info->fn = traverse_trees_wrapper_callback;
	ret = traverse_trees(istate, n, t, info);
	if (ret < 0)
		return ret;

	info->traverse_path = callback_data_traverse_path;
	info->fn = old_fn;
	for (i = old_offset; i < callback_data_nr; ++i) {
		info->fn(n,
			 callback_data[i].mask,
			 callback_data[i].dirmask,
			 callback_data[i].names,
			 info);

	}

	callback_data_nr = old_offset;
	free(callback_data_traverse_path);
	callback_data_traverse_path = old_callback_data_traverse_path;
	info->traverse_path = NULL;
	return 0;
}

static void setup_path_info(struct string_list_item *result,
			    const char *current_dir_name,
			    int current_dir_name_len,
			    char *fullpath, /* we'll take over ownership */
			    struct rename_info *rename_maps,
			    struct name_entry *names,
			    struct name_entry *merged_version,
			    unsigned is_null,     /* boolean */
			    unsigned df_conflict, /* boolean */
			    unsigned filemask,
			    unsigned dirmask,
			    int resolved          /* boolean */)
{
	struct conflict_info *path_info;

	assert(!is_null || resolved);      /* is_null implies resolved */
	assert(!df_conflict || !resolved); /* df_conflict implies !resolved */
	assert(resolved == (merged_version != NULL));

	path_info = xcalloc(1, resolved ? sizeof(struct merged_info) :
					  sizeof(struct conflict_info));
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
			if (!(filemask & (1ul << i)))
				continue;
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

static void setup_maps(struct strmap *basenames, const char *basename,
		       struct oidmap *hashes,    struct object_id *oid,
		       const char *fullname)
{
	struct string_list *sl;
	struct oidmap_string_list_entry *osle;

	sl = strmap_get(basenames, basename);
	if (sl)
		string_list_append(sl, fullname);
	else {
		sl = xmalloc(sizeof(*sl));
		string_list_init(sl, 0);
		string_list_append(sl, fullname);
		strmap_put(basenames, basename, sl);
	}

	osle = oidmap_get(hashes, oid);
	if (osle)
		string_list_append(&osle->fullpaths, fullname);
	else {
		osle = xmalloc(sizeof(*osle));
		oidcpy(&osle->entry.oid, oid);
		string_list_init(&osle->fullpaths, 0);
		string_list_append(&osle->fullpaths, fullname);
		oidmap_put(hashes, osle);
	}
}

static void collect_rename_info(struct rename_info *renames,
				struct name_entry *names,
				const char *fullname,
				const char *basename,
				unsigned filemask,
				unsigned dirmask)
{
	unsigned side;

	/*
	 * Update:
	 *   - possible_dir_renames (a boolean for whether directory
	 *     renames might have occurred)
	 *   - dir_rename_mask (a mask for determining whether it is safe to
	 *     ignore source paths that match on one side for inclusion in
	 *     rename detection)
	 *   - the if-condition here is explained inside the loop for both
	 *     vars
	 */
	if ((dirmask == 3 || dirmask == 5) && renames->dir_rename_mask != 0x07) {
		/*
		 * Simple rule for possible_dir_renames: set it whenever
		 * dirmask is 3 or 5.  (If dir_rename_mask was 0x07, that
		 * means a previous call to this function already set
		 * possible_dir_renames.)
		 */
		renames->possible_dir_renames = 1;

		/*
		 * Complicated rules for dir_rename_mask (based on how we can
		 * know whether a directory might be involved in a directory
		 * rename):
		 *   - Directory has to exist in mbase to have been renamed
		 *     (i.e. dirmask & 1 must be true)
		 *   - Directory cannot exist on both sides or it isn't renamed
		 *     (i.e. !(dirmask & 2) or !(dirmask & 4) must be true)
		 *   - If directory exists in neither side1 nor side2, then
		 *     there are no new files to send along with the directory
		 *     rename so there's no point detecting it[1].  (Thus,
		 *     either dirmask & 2 or dirmask & 4 must be true)
		 *   - The above rules mean dirmask is either 3 or 5, as checked
		 *     above.  If dir_rename_mask is 0x07 we've already ruled
		 *     the optimization completely unsafe, so we don't need to
		 *     mark it as we need to be careful with it.
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
		assert(renames->dir_rename_mask == 0 ||
		       renames->dir_rename_mask == (dirmask & ~1));
		renames->dir_rename_mask = (dirmask & ~1);
	}

	if (filemask == 0 || filemask == 7)
		return;

	for (side = 1; side <= 2; ++side) {
		struct diff_filespec *one, *two;
		unsigned side_mask = (side << 1);
		int need_pair = 0;

		if ((filemask & 1) && !(filemask & side_mask)) {
			/* File deletion; may be rename source */
			renames->nr_sources[side] += 1;
			need_pair = 1;
			one = alloc_filespec(fullname);
			two = alloc_filespec(fullname);
			fill_filespec(one, &names[0].oid, 1, names[0].mode);
			// fprintf(stderr, "Side %d deletion: %s\n", side, fullname);
			setup_maps(&renames->src_basenames[side],
				   basename,
				   &renames->src_hashes[side],
				   &names[0].oid,
				   fullname);
		}

		if (!(filemask & 1) && (filemask & side_mask)) {
			/* File addition; may be rename target */
			renames->nr_targets[side] += 1;
			need_pair = 1;
			one = alloc_filespec(fullname);
			two = alloc_filespec(fullname);
			fill_filespec(two, &names[side].oid, 1, names[side].mode);
			// fprintf(stderr, "Side %d addition: %s\n", side, fullname);
			setup_maps(&renames->dst_basenames[side],
				   basename,
				   &renames->dst_hashes[side],
				   &names[side].oid,
				   fullname);
		}

		if (need_pair)
			diff_queue(&renames->pairs[side], one, two);
	}
}

static inline void mark_rename_source_unused(struct rename_info *renames,
					     int side)
{
	struct diff_filepair *pair;
	pair = renames->pairs[side].queue[renames->pairs[side].nr-1];
	pair->one->rename_used = -1;
	renames->nr_sources[side] -= 1;
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
	struct string_list_item pi;  /* Path Info */
	struct name_entry *p;
	size_t len;
	char *fullpath;
	const char *dirname = opti->current_dir_name;
	unsigned prev_dir_rename_mask = opti->renames->dir_rename_mask;
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
	fullpath = xmalloc(len+1);
	make_traverse_path(fullpath, len+1, info, p->path, p->pathlen);

	/*
	 * If mbase, side1, and side2 all match, we can resolve early.  Even
	 * if these are trees, there will be no renames or anything
	 * underneath.
	 */
	if (side1_matches_mbase && side2_matches_mbase) {
		/* mbase, side1, & side2 all match; use mbase as resolution */
		setup_path_info(&pi, dirname, info->pathlen, fullpath,
				opti->renames, names,
				names+0, mbase_null, 0, filemask, dirmask, 1);
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
		setup_path_info(&pi, dirname, info->pathlen, fullpath,
				opti->renames, names,
				names+1, 0, 0, filemask, dirmask, 1);
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
	collect_rename_info(opt->priv->renames, names, fullpath, p->path,
			    filemask, dirmask);

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
	if (side1_matches_mbase && opti->renames->dir_rename_mask != 0x07) {
		if (side2_null) {
			/* Ignore this path, nothing to do. */
#ifdef VERBOSE_DEBUG
			printf("Path 1.A for %s\n", names[0].path);
#endif
			if (filemask)
				mark_rename_source_unused(opt->priv->renames, 2);
			return mask;
		} else if (!side1_is_tree && !side2_is_tree) {
			/* use side2 version as resolution */
			assert(filemask == 0x07);
			setup_path_info(&pi, dirname, info->pathlen, fullpath,
					opti->renames, names,
					names+2, side2_null, 0, filemask,
					dirmask, 1);
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
	if (side2_matches_mbase && opti->renames->dir_rename_mask != 0x07) {
		if (side1_null) {
			/* Ignore this path, nothing to do. */
#ifdef VERBOSE_DEBUG
			printf("Path 2.A for %s\n", names[0].path);
#endif
			if (filemask)
				mark_rename_source_unused(opt->priv->renames, 1);
			return mask;
		} else if (!side1_is_tree && !side2_is_tree) {
			/* use side1 version as resolution */
			assert(filemask == 0x07);
			setup_path_info(&pi, dirname, info->pathlen, fullpath,
					opti->renames, names,
					names+1, side1_null, 0, filemask,
					dirmask, 1);
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
	setup_path_info(&pi, dirname, info->pathlen, fullpath, opti->renames,
			names, NULL, 0, df_conflict, filemask, dirmask, 0);
#ifdef VERBOSE_DEBUG
	printf("Path 3 for %s, iprd = %u\n", pi.string,
	       opti->renames->dir_rename_mask);
	printf("Stats:\n");
#endif
	if (filemask) {
		struct conflict_info *ci = pi.util;
		if (side1_matches_mbase && !side1_is_tree)
			ci->match_mask = 3;
		else if (side2_matches_mbase && !side2_is_tree)
			ci->match_mask = 5;
		else if (sides_match && !side1_is_tree)
			ci->match_mask = 6;
		/* else ci->match_mask is already 0; no need to set it */
#ifdef VERBOSE_DEBUG
		printf("  matchmask: %u\n", ci->match_mask);
#endif
	}
#ifdef VERBOSE_DEBUG
	printf("  opti->renames->dir_rename_mask: %u\n",
	       opti->renames->dir_rename_mask);
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
		int ret;
		int i;

		newinfo = *info;
		newinfo.prev = info;
		newinfo.name = p->path;
		newinfo.namelen = p->pathlen;
		newinfo.mode = p->mode;
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
				buf[i] = fill_tree_descriptor(the_repository,
							      t + i, oid);
			}
		}

		original_dir_name = opti->current_dir_name;
		opti->current_dir_name = pi.string;
		if (opti->renames->dir_rename_mask == 0 ||
		    opti->renames->dir_rename_mask == 0x07)
			ret = traverse_trees(NULL, 3, t, &newinfo);
		else
			ret = traverse_trees_wrapper(NULL, 3, t, &newinfo);
		opti->current_dir_name = original_dir_name;
		opti->renames->dir_rename_mask = prev_dir_rename_mask;

		for (i = 0; i < 3; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}
	return mask;
}

static int collect_merge_info(struct merge_options *opt,
			      /* FIXME: We do NOT need the trees anymore */
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

static char *unique_path(struct merge_options *opt,
			 const char *path,
			 const char *branch)
{
	struct strbuf newpath = STRBUF_INIT;
	int suffix = 0;
	size_t base_len;

	strbuf_addf(&newpath, "%s~", path);
	add_flattened_path(&newpath, branch);

	base_len = newpath.len;
	while (strmap_contains(&opt->priv->paths, newpath.buf)) {
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

	int i;
	int search = !opt->priv->call_depth;

	/* store a in result in case we fail */
	/* FIXME: This is the WRONG resolution for the recursive case when
	 * we need to be careful to avoid accidentally matching either side.
	 * Should probably use o instead there, much like we do for merging
	 * binaries.
	 */
	oidcpy(result, a);

	/* we can not handle deletion conflicts */
	if (is_null_oid(o))
		return 0;
	if (is_null_oid(a))
		return 0;
	if (is_null_oid(b))
		return 0;

	if (add_submodule_odb(path)) {
		output(opt, 1, _("Failed to merge submodule %s (not checked out)"), path);
		return 0;
	}

	if (!(commit_o = lookup_commit_reference(opt->repo, o)) ||
	    !(commit_a = lookup_commit_reference(opt->repo, a)) ||
	    !(commit_b = lookup_commit_reference(opt->repo, b))) {
		output(opt, 1, _("Failed to merge submodule %s (commits not present)"), path);
		return 0;
	}

	/* check whether both changes are forward */
	if (!in_merge_bases(commit_o, commit_a) ||
	    !in_merge_bases(commit_o, commit_b)) {
		output(opt, 1, _("Failed to merge submodule %s (commits don't follow merge-base)"), path);
		return 0;
	}

	/* Case #1: a is contained in b or vice versa */
	if (in_merge_bases(commit_a, commit_b)) {
		oidcpy(result, b);
		if (show(opt, 3)) {
			output(opt, 3, _("Fast-forwarding submodule %s to the following commit:"), path);
			output_commit_title(opt, commit_b);
		} else if (show(opt, 2))
			output(opt, 2, _("Fast-forwarding submodule %s"), path);
		else
			; /* no output */

		return 1;
	}
	if (in_merge_bases(commit_b, commit_a)) {
		oidcpy(result, a);
		if (show(opt, 3)) {
			output(opt, 3, _("Fast-forwarding submodule %s to the following commit:"), path);
			output_commit_title(opt, commit_a);
		} else if (show(opt, 2))
			output(opt, 2, _("Fast-forwarding submodule %s"), path);
		else
			; /* no output */

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
		output(opt, 1, _("Failed to merge submodule %s (merge following commits not found)"), path);
		break;

	case 1:
		output(opt, 1, _("Failed to merge submodule %s (not fast-forward)"), path);
		output(opt, 2, _("Found a possible merge resolution for the submodule:\n"));
		print_commit((struct commit *) merges.objects[0].item);
		output(opt, 2, _(
		       "If this is correct simply add it to the index "
		       "for example\n"
		       "by using:\n\n"
		       "  git update-index --cacheinfo 160000 %s \"%s\"\n\n"
		       "which will accept this suggestion.\n"),
		       oid_to_hex(&merges.objects[0].item->oid), path);
		break;

	default:
		output(opt, 1, _("Failed to merge submodule %s (multiple merges found)"), path);
		for (i = 0; i < merges.nr; i++)
			print_commit((struct commit *) merges.objects[i].item);
	}

	object_array_clear(&merges);
	return 0;
}

static int merge_3way(struct merge_options *opt,
		      const char *path,
		      const struct version_info *o,
		      const struct version_info *a,
		      const struct version_info *b,
		      const char *pathnames[3],
		      const int extra_marker_size,
		      mmbuffer_t *result_buf)
{
	mmfile_t orig, src1, src2;
	struct ll_merge_options ll_opts = {0};
	char *base, *name1, *name2;
	int merge_status;

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

	read_mmblob(&orig, &o->oid);
	read_mmblob(&src1, &a->oid);
	read_mmblob(&src2, &b->oid);

	merge_status = ll_merge(result_buf, path, &orig, base,
				&src1, name1, &src2, name2,
				opt->repo->index, &ll_opts);

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

	if ((S_IFMT & a->mode) != (S_IFMT & b->mode)) {
		/* Not both files, not both submodules, not both symlinks */
		/*
		 * FIXME: this is a retarded resolution; it'd be better to
		 * rename paths elsewhere.
		 */
		clean = 0;
		if (S_ISGITLINK(a->mode)) {
			result->mode = a->mode;
			oidcpy(&result->oid, &a->oid);
		} else if (S_ISGITLINK(b->mode)) {
			result->mode = b->mode;
			oidcpy(&result->oid, &b->oid);
		} else if (S_ISREG(a->mode)) {
			result->mode = a->mode;
			oidcpy(&result->oid, &a->oid);
		} else {
			result->mode = b->mode;
			oidcpy(&result->oid, &b->oid);
		}
	} else {
		/*
		 * FIXME:
		 * If we ensure to set up match_mask in handle rename,
		 * then we can assert the following:
		    assert(!oideq(&a->oid, &o->oid) || !oideq(&b->oid, &o->oid));
		 * Getting here means a & b are both (files OR submodules OR
		 * symlinks); they do not differ in type.
		 */

		/*
		 * Merge modes
		 */
		if (a->mode == b->mode || a->mode == o->mode)
			result->mode = b->mode;
		else {
			/* must be the 100644/100755 case */
			assert(S_ISREG(a->mode));
			result->mode = a->mode;
			clean = (b->mode == o->mode);
		}

		/* FIXME: can remove next four lines based on match_mask too */
		if (oideq(&a->oid, &b->oid) || oideq(&a->oid, &o->oid))
			oidcpy(&result->oid, &b->oid);
		else if (oideq(&b->oid, &o->oid))
			oidcpy(&result->oid, &a->oid);
		/* Remaining merge rules depends on file vs. submodule vs. symlink. */
		/* FIXME: What if o is different type than a & b? */
		else if (S_ISREG(a->mode)) {
			mmbuffer_t result_buf;
			int ret = 0, merge_status;

			merge_status = merge_3way(opt, path, o, a, b,
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
		} else if (S_ISGITLINK(a->mode)) {
			clean = merge_submodule(opt, pathnames[0],
						&o->oid, &a->oid, &b->oid,
						&result->oid);
		} else if (S_ISLNK(a->mode)) {
			switch (opt->recursive_variant) {
			case MERGE_VARIANT_NORMAL:
				oidcpy(&result->oid, &a->oid);
				if (!oideq(&a->oid, &b->oid))
					clean = 0;
				break;
			case MERGE_VARIANT_OURS:
				oidcpy(&result->oid, &a->oid);
				break;
			case MERGE_VARIANT_THEIRS:
				oidcpy(&result->oid, &b->oid);
				break;
			}
		} else
			BUG("unsupported object type in the tree: %06o for %s",
			    a->mode, path);
	}

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
		int source_deleted, collision;

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
			/* FIXME: handle return value of handle_content_merge */
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
			handle_content_merge(opt, pair->one->path,
					     &base->stages[0],
					     &side1->stages[1],
					     &side2->stages[2],
					     pathnames, 1 + 2 * opt->priv->call_depth,
					     &merged);
			memcpy(&side1->stages[1], &merged, sizeof(merged));
			memcpy(&side2->stages[2], &merged, sizeof(merged));
			/* FIXME: Mark side1 & side2 as conflicted */
			side1->path_conflict = 1;
			side2->path_conflict = 1;
			/* FIXME: Need to report conflict to output somehow */
			//base->merged.is_null = 1;
			//base->merged.clean = 1;
			base->path_conflict = 1;
			/* FIXME: Do un-rename in recursive case */
			i++; /* We handled both renames, so skip an extra */
			continue;
		}

		assert(oldinfo);
		assert(newinfo);
		assert(!oldinfo->merged.clean);
		assert(!newinfo->merged.clean);
		target_index = pair->score; /* from append_rename_pairs() */
		assert(target_index == 1 || target_index == 2);
		other_source_index = 3-target_index;
		old_sidemask = (other_source_index << 1); /* 2 or 4 */
		source_deleted = (oldinfo->filemask == 1);
		collision = ((newinfo->filemask & old_sidemask) != 0);
#ifdef VERBOSE_DEBUG
		printf("collision: %d, source_deleted: %d\n",
		       collision, source_deleted);

		printf("  oldpath: %s, newpath: %s\n", oldpath, newpath);
		printf("source_deleted: %d\n", source_deleted);
		printf("oldinfo->filemask: %d\n", oldinfo->filemask);
		printf("old_sidemask: %d\n", old_sidemask);
#endif
		assert(source_deleted || oldinfo->filemask & old_sidemask);

		/* In all cases, mark the original as resolved by removal */
		oldinfo->merged.is_null = 1;
		oldinfo->merged.clean = 1;

		/* Need to check for special types of rename conflicts... */
		if (collision && !source_deleted) {
			/* collision: rename/add or rename/rename(2to1) */
			const char *pathnames[3];
			struct version_info merged;

			struct conflict_info *base, *side1, *side2;

			pathnames[0] = oldpath;
			pathnames[other_source_index] = oldpath;
			pathnames[target_index] = newpath;
			base = strmap_get(&opt->priv->paths, pathnames[0]);
			side1 = strmap_get(&opt->priv->paths, pathnames[1]);
			side2 = strmap_get(&opt->priv->paths, pathnames[2]);
			/* FIXME: handle return value of handle_content_merge */
			handle_content_merge(opt, pair->one->path,
					     &base->stages[0],
					     &side1->stages[1],
					     &side2->stages[2],
					     pathnames, 1 + 2 * opt->priv->call_depth,
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
			/* FIXME: Would be nicer to look like rename/add than
			   add/add. */
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
			if (!source_deleted) {
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
	struct strmap possible_new_dirs;
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
		output(opt, 1, _("CONFLICT (implicit dir rename): Existing "
			       "file/dir at %s in the way of implicit "
			       "directory rename(s) putting the following "
			       "path(s) there: %s."),
		       new_path, collision_paths.buf);
		clean = 0;
	} else if (c_info->source_files.nr > 1) {
		c_info->reported_already = 1;
		strbuf_add_separated_string_list(&collision_paths, ", ",
						 &c_info->source_files);
		output(opt, 1, _("CONFLICT (implicit dir rename): Cannot map "
			       "more than one path to %s; implicit directory "
			       "renames tried to put these paths there: %s"),
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
	dir_renames = xmalloc(sizeof(*dir_renames));
	strmap_init(dir_renames, 0);
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

		info = strmap_get(dir_renames, old_dir);
		if (info) {
			free(old_dir);
		} else {
			info = xcalloc(1, sizeof(*info));
			strbuf_init(&info->new_dir, 0);
			strmap_init(&info->possible_new_dirs, 0);
			strmap_put(dir_renames, old_dir, info);
		}

		count = strintmap_get(&info->possible_new_dirs, new_dir);
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

		strmap_for_each_entry(&info->possible_new_dirs, &pnd_iter, pnd_entry) {
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
			output(opt, 1,
			       _("CONFLICT (directory rename split): "
			       "Unclear where to rename %s to; it was renamed "
			       "to multiple other directories, with no "
			       "destination getting a majority of the files."),
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
		info->possible_new_dirs.strdup_strings = 1;
		strmap_clear(&info->possible_new_dirs, 0);
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
		output(opt, 1,
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
	char *old_path = pair->two->path;
	char *parent_name;
	char *cur_path;
	int i, len;

	item = strmap_get_item(&opt->priv->paths, old_path);
	old_path = item->string;
	ci = item->util;
	dump_conflict_info(ci, old_path);

	/* Find parent directories missing from opt->priv->paths */
	cur_path = new_path;
	while (1) {
		/* Find the parent directory of cur_path */
		char *last_slash = strrchr(cur_path, '/');
		if (last_slash)
			parent_name = xstrndup(cur_path, last_slash - cur_path);
		else {
			parent_name = opt->priv->toplevel_dir;
			break;
		}

		/* Look it up in opt->priv->paths */
		item = strmap_get_item(&opt->priv->paths, parent_name);
		if (item) {
			free(parent_name);
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

	/*
	 * Remove old_path from opt->priv->paths.  old_path also will
	 * eventually need to be freed, but it may still be used by e.g.
	 * ci->pathnames.  So, store it in another string-list for now.
	 */
	string_list_append(&opt->priv->paths_to_free, old_path);
#ifdef VERBOSE_DEBUG
	printf("Removing %s from opt->priv->paths!\n", old_path);
#endif
	assert(ci->filemask == 2 || ci->filemask == 4);
	assert(ci->dirmask == 0);
	strmap_remove(&opt->priv->paths, old_path, 0);

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

		free(ci);
	}

#if 0
	/*
	 * Record the original change status (or 'type' of change).  If it
	 * was originally an add ('A'), this lets us differentiate later
	 * between a RENAME_DELETE conflict and RENAME_VIA_DIR (they
	 * otherwise look the same).  If it was originally a rename ('R'),
	 * this lets us remember and report accurately about the transitive
	 * renaming that occurred via the directory rename detection.  Also,
	 * record the original destination name.
	 */
	re->dir_rename_original_type = pair->status;
	re->dir_rename_original_dest = old_path;

	/*
	 * We don't actually look at pair->status again, but it seems
	 * pedagogically correct to adjust it.
	 */
	pair->status = 'R';
#endif

	/*
	 * Finally, record the new location.
	 */
	pair->two->path = new_path;
}

/*** Rename stuff ***/

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


/* Call diffcore_rename() to update deleted/added pairs into rename pairs */
static void detect_regular_renames(struct merge_options *opt,
				   unsigned side_index)
{
	struct diff_options opts;
	struct rename_info *renames = opt->priv->renames;

	if (renames->nr_sources[side_index] == 0 ||
	    renames->nr_targets[side_index] == 0) {
		/*
		 * No rename detection needed for this side, but we still need
		 * adds to be marked correctly in case the other side had
		 * directory renames.
		 */
		resolve_diffpair_statuses(&renames->pairs[side_index]);
		return;
	}

	repo_diff_setup(opt->repo, &opts);
	opts.flags.recursive = 1;
	opts.flags.rename_empty = 0;
	opts.detect_rename = merge_detect_rename(opt);
	/*
	 * We do not have logic to handle the detection of copies.  In
	 * fact, it may not even make sense to add such logic: would we
	 * really want a change to a base file to be propagated through
	 * multiple other files by a merge?
	 */
	if (opts.detect_rename > DIFF_DETECT_RENAME)
		opts.detect_rename = DIFF_DETECT_RENAME;
	opts.rename_limit = (opt->rename_limit >= 0) ? opt->rename_limit : 1000;
	opts.rename_score = opt->rename_score;
	opts.show_rename_progress = opt->show_rename_progress;
	opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_setup_done(&opts);

	diff_queued_diff = renames->pairs[side_index];
	dump_pairs(&diff_queued_diff, "Before diffcore_rename");
	diffcore_rename(&opts);
	resolve_diffpair_statuses(&diff_queued_diff);
	dump_pairs(&diff_queued_diff, "After diffcore_rename");
#ifdef VERBOSE_DEBUG
	printf("Done.\n");
#endif
	if (opts.needed_rename_limit > opt->priv->needed_rename_limit)
		opt->priv->needed_rename_limit = opts.needed_rename_limit;

	renames->pairs[side_index] = diff_queued_diff;

	opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_queued_diff.nr = 0;
	diff_queued_diff.queue = NULL;
	diff_flush(&opts);
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

	side_pairs = &opt->priv->renames->pairs[side_index];
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
		if (p->status != 'A' && p->status != 'R') {
			diff_free_filepair(p);
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
			diff_free_filepair(p);
			continue;
		}
		if (new_path)
			apply_directory_rename_modifications(opt, p, new_path);

		p->score = side_index;
		result->queue[result->nr++] = p;
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
	strmap_clear(&collisions, 1);
	return clean;
}

static int detect_and_process_renames(struct merge_options *opt,
				      struct diff_queue_struct *combined,
				      struct tree *merge_base,
				      struct tree *side1,
				      struct tree *side2)
{
	struct strmap *side1_dir_renames, *side2_dir_renames;
	int need_dir_renames, clean = 1;

	memset(combined, 0, sizeof(*combined));
	if (!merge_detect_rename(opt))
		return 1;

	/*
	 * FIXME:
	 *   (1) Free'ing of opt->priv->renames->pairs at end of func might
	 *       need cleanup
	 *   (2) Free'ing of combined by caller needs to be done
	 */
	detect_regular_renames(opt, 1);
	detect_regular_renames(opt, 2);

	need_dir_renames =
	  !opt->priv->call_depth &&
	  opt->priv->renames->possible_dir_renames &&
	  (opt->detect_directory_renames == MERGE_DIRECTORY_RENAMES_TRUE ||
	   opt->detect_directory_renames == MERGE_DIRECTORY_RENAMES_CONFLICT);

	if (need_dir_renames) {
#ifdef VERBOSE_DEBUG
		struct hashmap_iter iter;
		struct str_entry *entry;
#endif

		side1_dir_renames = get_directory_renames(opt, 1, &clean);
		side2_dir_renames = get_directory_renames(opt, 2, &clean);
#ifdef VERBOSE_DEBUG
		fprintf(stderr, "Side1 dir renames:\n");
		strmap_for_each_entry(&opt->priv->renames->pairs[1], &iter, entry) {
			struct dir_rename_info *info = entry->item.util;
			fprintf(stderr, "    %s -> %s:\n",
				entry->item.string, info->new_dir.buf);
		}
		fprintf(stderr, "Side2 dir renames:\n");
		strmap_for_each_entry(&opt->priv->renames->pairs[2], &iter, entry) {
			struct dir_rename_info *info = entry->item.util;
			fprintf(stderr, "    %s -> %s:\n",
				entry->item.string, info->new_dir.buf);
		}
		fprintf(stderr, "Done.\n");
#endif
		handle_directory_level_conflicts(opt, side1_dir_renames,
						 side2_dir_renames);

	} else {
		side1_dir_renames  = xmalloc(sizeof(*side1_dir_renames));
		side2_dir_renames = xmalloc(sizeof(*side2_dir_renames));
		strmap_init(side1_dir_renames, 0);
		strmap_init(side2_dir_renames, 0);
	}

	ALLOC_GROW(combined->queue,
		   opt->priv->renames->pairs[1].nr +
		   opt->priv->renames->pairs[2].nr,
		   combined->alloc);
	clean &= collect_renames(opt, combined, 1,
				 side2_dir_renames, side1_dir_renames);
	clean &= collect_renames(opt, combined, 2,
				 side1_dir_renames, side2_dir_renames);
	QSORT(combined->queue, combined->nr, compare_pairs);

#ifdef VERBOSE_DEBUG
	printf("=== Processing %d renames ===\n", combined->nr);
#endif
	clean &= process_renames(opt, combined);

	/*
	 * In get_directory_renames(), we set side[12].strdup_strings to 0
	 * so that we wouldn't have to make another copy of the old_path
	 * allocated by get_renamed_dir_portion().  But now that we've used
	 * it and have no other references to these strings, it is time to
	 * deallocate them, which we do by just setting strdup_string = 1
	 * before the strmaps are cleared.
	 */
	side1_dir_renames->strdup_strings = 1;
	side2_dir_renames->strdup_strings = 1;
	strmap_clear(side1_dir_renames, 1);
	strmap_clear(side2_dir_renames, 1);
	opt->priv->renames->pairs[1].nr = 0;
	opt->priv->renames->pairs[2].nr = 0;
	free(opt->priv->renames->pairs[1].queue);
	free(opt->priv->renames->pairs[2].queue);
	/*
	 * We cannot deallocate combined yet; strings contained in it were
	 * used inside opt->priv->paths, so we need to wait to deallocate it.
	 */
	return clean;
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

	/* int normalize = opt->renormalize; */

#ifdef VERBOSE_DEBUG
	printf("Processing %s; filemask = %d\n", e->string, ci->filemask);
#endif
	assert(!ci->merged.clean && !ci->processed);
	ci->processed = 1;
	assert(ci->filemask >=0 && ci->filemask < 8);
	if (ci->filemask == 0) {
		/*
		 * This is a placeholder for directories that were recursed
		 * into; nothing to do in this case.
		 */
		return;
	}
	if (ci->df_conflict && ci->merged.result.mode == 0) {
		/*
		 * directory no longer in the way, but we do have a file we
		 * need to place here so we need to clean away the "directory
		 * merges to nothing" result.
		 */
		ci->df_conflict = 0;
		assert(ci->filemask != 0);
		ci->merged.clean = 0;
		ci->merged.is_null = 0;
	} else if (ci->df_conflict && ci->merged.result.mode != 0) {
		/*
		 * This started out as a D/F conflict, and the entries in
		 * the competing directory were not removed by the merge as
		 * evidenced by write_completed_directories() writing a value
		 * to ci->merged.result.mode.
		 */
		struct conflict_info *new_ci;
		const char *branch;

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
		 * be overwritten below so it doesn't matter, and we do want
		 * the rest of ci copied.
		 */
		memcpy(new_ci, ci, sizeof(*ci));

		/*
		 * Find out which side this file came from; note that we
		 * cannot just use ci->filemask, because renames could cause
		 * the filemask to go back to 7.  So we use dirmask, then
		 * pick the opposite side's index.
		 */
		df_file_index = (ci->dirmask & (1 << 1)) ? 2 : 1;
		branch = (df_file_index == 1) ? opt->branch1 : opt->branch2;
		path = unique_path(opt, path, branch);
		strmap_put(&opt->priv->paths, path, new_ci);

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
			printf("ci->merged.result.mode: %d, is_null: %d\n",
			       ci->merged.result.mode, ci->merged.is_null);
#endif
			assert(othermask == 2 || othermask == 4);
			assert(ci->merged.is_null == !ci->merged.result.mode);
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
		ci->merged.clean = clean_merge && !ci->df_conflict;
		ci->merged.result.mode = merged_file.mode;
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
		/* Handle output stuff...
		if (!clean_merge) {
			if (S_ISREG(a->mode) && S_ISREG(b->mode)) {
				output(opt, 2, _("Auto-merging %s"), filename);
			}
			const char *reason = _("content");
			if (!is_valid(o))
				reason = _("add/add");
			if (S_ISGITLINK(mfi->blob.mode))
				reason = _("submodule");
			output(opt, 1, _("CONFLICT (%s): Merge conflict in %s"),
			       reason, path);
		}
		*/
	} else if (ci->filemask == 3 || ci->filemask == 5) {
		/* Modify/delete */
		int side = (ci->filemask == 5) ? 2 : 1;
		int index = opt->priv->call_depth ? 0 : side;
		ci->merged.result.mode = ci->stages[index].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[index].oid);
		ci->merged.clean = 0;
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

	if (strmap_empty(&opt->priv->paths)) {
		oidcpy(result_oid, opt->repo->hash_algo->empty_tree);
		return;
	}

	/* Hack to pre-allocated both to the desired size */
	ALLOC_GROW(plist.items, strmap_get_size(&opt->priv->paths), plist.alloc);

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(&opt->priv->paths, &iter, e) {
		string_list_append(&plist, e->item.string)->util = e->item.util;
	}
	plist.cmp = string_list_df_name_compare;
	string_list_sort(&plist);

	/*
	 * Iterate over the items in both in reverse order, so we can handle
	 * contained directories before the containing directory.
	 */
	string_list_init(&dir_metadata.versions, 0);
	string_list_init(&dir_metadata.offsets, 0);
	dir_metadata.last_directory = NULL;
	dir_metadata.last_directory_len = 0;
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
	if (dir_metadata.offsets.nr != 1 ||
	    (uintptr_t)dir_metadata.offsets.items[0].util != 0) {
		printf("dir_metadata.offsets.nr = %d (should be 1)\n",
		       dir_metadata.offsets.nr);
		printf("dir_metadata.offsets.items[0].util = %lu (should be 0)\n",
		       (uintptr_t)dir_metadata.offsets.items[0].util);
		BUG("dir_metadata accounting completely off; shouldn't happen");
	}
	write_tree(result_oid, &dir_metadata.versions, 0);
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
	return ret;
}

static int record_unmerged_index_entries(struct merge_options *opt)
{
	struct hashmap_iter iter;
	struct str_entry *e;
	struct checkout state = CHECKOUT_INIT;
	int errs = 0;
	int original_cache_nr;

	if (strmap_empty(&opt->priv->unmerged))
		return 0;

#ifdef VERBOSE_DEBUG
	for (int i=0; i<opt->repo->index->cache_nr; ++i) {
		fprintf(stderr, "  cache[%d] = (%s, %s, %o, %d, %u, %d)\n",
		       i,
		       opt->repo->index->cache[i]->name,
		       oid_to_hex(&opt->repo->index->cache[i]->oid),
		       opt->repo->index->cache[i]->ce_mode,
		       ce_stage(opt->repo->index->cache[i]),
		       opt->repo->index->cache[i]->ce_flags,
		       opt->repo->index->cache[i]->index);
	}
	fprintf(stderr, "... AFTER ...\n");
#endif

	/* If any entries have skip_worktree set, we'll have to check 'em out */
	state.force = 1;
	state.quiet = 1;
	state.refresh_cache = 1;
	state.istate = opt->repo->index;
	original_cache_nr = opt->repo->index->cache_nr;

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(&opt->priv->unmerged, &iter, e) {
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
		SWAP(opt->repo->index->cache_nr, original_cache_nr);
		pos = index_name_pos(opt->repo->index, path, strlen(path));
		SWAP(opt->repo->index->cache_nr, original_cache_nr);
#ifdef VERBOSE_DEBUG
		fprintf(stderr, "Found pos %d for %s\n", pos, path);
#endif
		if (pos < 0) {
			if (ci->filemask == 1)
				cache_tree_invalidate_path(opt->repo->index, path);
			else
				BUG("Unmerged %s but nothing in basic working tree or index; this shouldn't happen", path);
		} else {
			ce = opt->repo->index->cache[pos];

			/*
			 * If this cache entry had the skip_worktree bit set,
			 * then it isn't present in the working tree..but since
			 * it corresponds to a merge conflict we need it to be.
			 */
			if (ce_skip_worktree(ce)) {
				struct stat st;

				if (!lstat(path, &st)) {
					char *new_name = unique_path(opt, path, "cruft");

					output(opt, 2, _("Note: %s not up to date and in way of checking out conflicted version; old copy renamed to %s"), path, new_name);
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
			ce = make_cache_entry(opt->repo->index, vi->mode,
					      &vi->oid, path, i+1, 0);
			add_index_entry(opt->repo->index, ce,
					ADD_CACHE_JUST_APPEND);
		}
	}

	/*
	 * Remove the unused cache entries (and invalidate the relevant
	 * cache-trees), then sort the index entries to get the unmerged
	 * entries we added to the end into their right locations.
	 */
	remove_marked_cache_entries(opt->repo->index, 1);
	QSORT(opt->repo->index->cache, opt->repo->index->cache_nr,
	      cmp_cache_name_compare);
#ifdef VERBOSE_DEBUG
	for (int i=0; i<opt->repo->index->cache_nr; ++i) {
		fprintf(stderr, "  cache[%d] = (%s, %s, %o, %d, %u, %d)\n",
		       i,
		       opt->repo->index->cache[i]->name,
		       oid_to_hex(&opt->repo->index->cache[i]->oid),
		       opt->repo->index->cache[i]->ce_mode,
		       ce_stage(opt->repo->index->cache[i]),
		       opt->repo->index->cache[i]->ce_flags,
		       opt->repo->index->cache[i]->index);
	}
#endif

	return errs;
}

/*
 * Drop-in replacement for merge_trees_internal().
 * Differences:
 *   1) s/merge_trees_internal/merge_ort_nonrecursive_internal/
 *   2) The handling of unmerged entries has been gutted and replaced with
 *      a BUG() call.  Will be handled later.
 */
static int merge_ort_nonrecursive_internal(struct merge_options *opt,
					   struct tree *head,
					   struct tree *merge,
					   struct tree *merge_base,
					   struct tree **result)
{
	int code, clean;
	struct diff_queue_struct pairs;
	struct object_id working_tree_oid;

	if (opt->subtree_shift) {
		merge = shift_tree_object(opt->repo, head, merge,
					  opt->subtree_shift);
		merge_base = shift_tree_object(opt->repo, head, merge_base,
					       opt->subtree_shift);
	}

	if (oideq(&merge_base->object.oid, &merge->object.oid)) {
		output(opt, 0, _("Already up to date!"));
		*result = head;
		return 1;
	}

	code = collect_merge_info(opt, merge_base, head, merge);
	if (code != 0) {
		if (show(opt, 4) || opt->priv->call_depth)
			err(opt, _("collecting merge info for trees %s and %s failed"),
			    oid_to_hex(&head->object.oid),
			    oid_to_hex(&merge->object.oid));
		return -1;
	}

	clean = detect_and_process_renames(opt, &pairs, merge_base, head, merge);

	process_entries(opt, &working_tree_oid);
	clean &= strmap_empty(&opt->priv->unmerged); /* unmerged entries => unclean */

	/*
	 * FIXME: Also need to free each diff_filepair in pairs.queue, and may
	 * also need to free each pair's one->path and/or two->path.
	 */
	if (pairs.nr)
		free(pairs.queue);
	*result = parse_tree_indirect(&working_tree_oid);
	return clean;
}

static void reset_maps(struct merge_options *opt, int reinitialize);

/*
 * Drop-in replacement for merge_recursive_internal().
 * Currently, a near wholesale copy-paste of merge_recursive_internal(); only
 * the following modifications have been made:
 *   1) s/merge_recursive_internal/merge_ort_internal/
 *   2) s/merge_trees_internal/merge_ort_nonrecursive_internal/
 */
static int merge_ort_internal(struct merge_options *opt,
			      struct commit *h1,
			      struct commit *h2,
			      struct commit_list *merge_bases,
			      struct tree **result_tree)
{
	struct commit_list *iter;
	struct commit *merged_merge_bases;
	int clean;
	const char *ancestor_name;
	struct strbuf merge_base_abbrev = STRBUF_INIT;

	if (show(opt, 4)) {
		output(opt, 4, _("Merging:"));
		output_commit_title(opt, h1);
		output_commit_title(opt, h2);
	}

	if (!merge_bases) {
		merge_bases = get_merge_bases(h1, h2);
		merge_bases = reverse_commit_list(merge_bases);
	}

	if (show(opt, 5)) {
		unsigned cnt = commit_list_count(merge_bases);

		output(opt, 5, Q_("found %u common ancestor:",
				"found %u common ancestors:", cnt), cnt);
		for (iter = merge_bases; iter; iter = iter->next)
			output_commit_title(opt, iter->item);
	}

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
		if (merge_ort_internal(opt, prev, iter->item, NULL,
				       result_tree) < 0)
			return -1;
		opt->branch1 = saved_b1;
		opt->branch2 = saved_b2;
		opt->priv->call_depth--;

		merged_merge_bases = make_virtual_commit(opt->repo,
							 *result_tree,
							 "merged tree");
		commit_list_insert(prev, &merged_merge_bases->parents);
		commit_list_insert(iter->item,
				   &merged_merge_bases->parents->next);

		reset_maps(opt, 1);
	}

	if (!opt->priv->call_depth && merge_bases != NULL) {
		discard_index(opt->repo->index);
		repo_read_index(opt->repo);
	}

	opt->ancestor = ancestor_name;
	clean = merge_ort_nonrecursive_internal(opt,
				     repo_get_commit_tree(opt->repo, h1),
				     repo_get_commit_tree(opt->repo, h2),
				     repo_get_commit_tree(opt->repo,
							  merged_merge_bases),
				     result_tree);
	strbuf_release(&merge_base_abbrev);
	opt->ancestor = NULL;  /* avoid accidental re-use of opt->ancestor */
	if (clean < 0) {
		flush_output(opt);
		return clean;
	}

	return clean;
}

static int merge_start(struct merge_options *opt, struct tree *head)
{
	struct strbuf sb = STRBUF_INIT;

	/* Sanity checks on opt */
	assert(opt->repo);

	assert(opt->branch1 && opt->branch2);

	assert(opt->detect_renames >= -1 &&
	       opt->detect_renames <= DIFF_DETECT_COPY);
	assert(opt->detect_directory_renames >= MERGE_DIRECTORY_RENAMES_NONE &&
	       opt->detect_directory_renames <= MERGE_DIRECTORY_RENAMES_TRUE);
	assert(opt->rename_limit >= -1);
	assert(opt->rename_score >= 0 && opt->rename_score <= MAX_SCORE);
	assert(opt->show_rename_progress >= 0 && opt->show_rename_progress <= 1);

	assert(opt->xdl_opts >= 0);
	assert(opt->recursive_variant >= MERGE_VARIANT_NORMAL &&
	       opt->recursive_variant <= MERGE_VARIANT_THEIRS);

	assert(opt->verbosity >= 0 && opt->verbosity <= 5);
	assert(opt->buffer_output <= 2);
	assert(opt->obuf.len == 0);

	if (head)
		assert(opt->priv == NULL);

	/* Sanity check on repo state; index must match head */
	if (head && repo_index_has_changes(opt->repo, head, &sb)) {
		err(opt, _("Your local changes to the following files would be overwritten by merge:\n  %s"),
		    sb.buf);
		strbuf_release(&sb);
		return -1;
	}

	if (opt->priv) {
		reset_maps(opt, 1);
	} else {
		int i;

		opt->priv = xcalloc(1, sizeof(*opt->priv));
		opt->priv->renames = xcalloc(1, sizeof(*opt->priv->renames));
		for (i=1; i<3; i++) {
			strmap_init(&opt->priv->renames->src_basenames[i], 1);
			strmap_init(&opt->priv->renames->dst_basenames[i], 1);
			oidmap_init(&opt->priv->renames->src_hashes[i], 0);
			oidmap_init(&opt->priv->renames->dst_hashes[i], 0);
		}

		/*
		 * Although we initialize opt->priv->paths_to_free and
		 * opt->priv->paths with strdup_strings = 0, that's just to
		 * avoid making an extra copy of an allocated string.  Both
		 * of these store strings that we will later need to free.
		 */
		string_list_init(&opt->priv->paths_to_free, 0);
		strmap_init(&opt->priv->paths, 0);
		strmap_init(&opt->priv->unmerged, 0);
	}

	return 0;
}

int switch_to_merge_result(struct merge_options *opt,
			   struct tree *head,
			   struct merge_result *result)
{
	if (checkout(opt, head, result->automerge_tree))
		return -1; /* failure to function */
	if (record_unmerged_index_entries(opt))
		return -1; /* failure to function */
	return 0;
}

void merge_finalize(struct merge_options *opt,
		    struct merge_result *result)
{
	flush_output(opt);
	if (!opt->priv->call_depth && opt->buffer_output < 2)
		strbuf_release(&opt->obuf);
	if (show(opt, 2))
		diff_warn_rename_limit("merge.renamelimit",
				       opt->priv->needed_rename_limit, 0);

	reset_maps(opt, 0);
	FREE_AND_NULL(opt->priv->renames);
	FREE_AND_NULL(opt->priv);
}

static void reset_maps(struct merge_options *opt, int reinitialize)
{
	int i;
	void (*strmap_func)(struct strmap *, int) =
		reinitialize ? strmap_clear : strmap_free;

	/*
	 * We marked opt->priv->paths with strdup_strings = 0, so that we
	 * wouldn't have to make another copy of the fullpath created by
	 * make_traverse_path from setup_path_info().  But, now that we've
	 * used it and have no other references to these strings, it is time
	 * to deallocate them, which we do by just setting strdup_string = 1
	 * before the strmaps are cleared.
	 */
	opt->priv->paths.strdup_strings = 1;
	strmap_func(&opt->priv->paths, 1);
	opt->priv->paths.strdup_strings = 0;

	/* opt->priv->paths_to_free is similar to opt->priv->paths. */
	opt->priv->paths_to_free.strdup_strings = 1;
	string_list_clear(&opt->priv->paths_to_free, 0);
	opt->priv->paths_to_free.strdup_strings = 0;

	/*
	 * All strings and util fields in opt->priv->unmerged are a subset
	 * of those in opt->priv->paths.  We don't want to deallocate
	 * anything twice, so we don't set strdup_strings and we pass 0 for
	 * free_util.
	 */
	strmap_func(&opt->priv->unmerged, 0);

	/* Zero out all files in opt->priv->renames too */
	memset(opt->priv->renames, 0, sizeof(*opt->priv->renames));
	for (i=1; i<3; ++i) {
		struct hashmap_iter sm_iter;
		struct str_entry *sm_entry;
		struct oidmap_iter om_iter;
		struct oidmap_string_list_entry *om_entry;

		/* clear src_basenames and its data */
		strmap_for_each_entry(&opt->priv->renames->src_basenames[i],
				      &sm_iter, sm_entry)
			string_list_clear(sm_entry->item.util, 0);
		strmap_func(&opt->priv->renames->src_basenames[i], 1);

		/* clear dst_basenames and its data */
		strmap_for_each_entry(&opt->priv->renames->dst_basenames[i],
				      &sm_iter, sm_entry)
			string_list_clear(sm_entry->item.util, 0);
		strmap_func(&opt->priv->renames->dst_basenames[i], 1);

		/* clear src_hashes and its data */
		oidmap_iter_init(&opt->priv->renames->src_hashes[i], &om_iter);
		while ((om_entry = oidmap_iter_next(&om_iter)))
			string_list_clear(&om_entry->fullpaths, 0);
		oidmap_free(&opt->priv->renames->src_hashes[i], 1);
		if (reinitialize)
			oidmap_init(&opt->priv->renames->src_hashes[i], 0);

		/* clear dst_hashes and its data */
		oidmap_iter_init(&opt->priv->renames->dst_hashes[i], &om_iter);
		while ((om_entry = oidmap_iter_next(&om_iter))) {
			string_list_clear(&om_entry->fullpaths, 0);
		}
		oidmap_free(&opt->priv->renames->dst_hashes[i], 1);
		if (reinitialize)
			oidmap_init(&opt->priv->renames->dst_hashes[i], 0);
	}

	/* Clean out callback_data as well. */
	FREE_AND_NULL(callback_data);
	callback_data_nr = callback_data_alloc = 0;
}

void merge_ort_inmemory_nonrecursive(struct merge_options *opt,
				     struct tree *head,
				     struct tree *merge,
				     struct tree *merge_base,
				     struct merge_result *result)
{
	int clean;

	assert(opt->ancestor != NULL);

	if (merge_start(opt, NULL)) {
		result->clean = -1;
		return;
	}
	clean = merge_ort_nonrecursive_internal(opt, head, merge, merge_base,
						&result->automerge_tree);

	result->clean = clean;
}

int merge_ort_nonrecursive(struct merge_options *opt,
			   struct tree *head,
			   struct tree *merge,
			   struct tree *merge_base)
{
	int clean;
	struct merge_result result;

	assert(opt->ancestor != NULL);

	if (merge_start(opt, head))
		return -1;
	clean = merge_ort_nonrecursive_internal(opt, head, merge, merge_base,
						&result.automerge_tree);
	if (switch_to_merge_result(opt, head, &result))
		clean = -1;
	merge_finalize(opt, &result);

	return clean;
}

int merge_ort_recursive(struct merge_options *opt,
			struct commit *side1,
			struct commit *side2,
			struct commit_list *merge_bases,
			struct tree **result)
{
	int clean;
	struct tree *head = repo_get_commit_tree(opt->repo, side1);
	struct merge_result tmp;

	assert(opt->ancestor == NULL ||
	       !strcmp(opt->ancestor, "constructed merge base"));

	if (merge_start(opt, head))
		return -1;
	clean = merge_ort_internal(opt, side1, side2, merge_bases, result);
	tmp.automerge_tree = *result;
	if (switch_to_merge_result(opt, head, &tmp))
		clean = -1;
	merge_finalize(opt, &tmp);

	return clean;
}
