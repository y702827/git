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
#include "cache-tree.h"
#include "commit.h"
#include "commit-reach.h"
#include "diff.h"
#include "diffcore.h"
#include "dir.h"
#include "object-store.h"
#include "strmap.h"
#include "unpack-trees.h"

struct merge_options_internal {
	struct strmap merged;   /* maps path -> version_info */
	struct strmap unmerged; /* maps path -> conflict_info */
	struct strmap possible_dir_rename_bases; /* set of paths */
	struct strmap submodule_directory_conflicts; /* set of paths */
	const char *current_dir_name;
	unsigned nr_dir_only_entries; /* unmerged also tracks directory names */
	int call_depth;
	int needed_rename_limit;
	unsigned inside_possibly_renamed_dir:1;
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
	unsigned result_is_null;
};

struct conflict_info {
	struct merged_info merged;
	struct version_info stages[3];
	unsigned df_conflict:1;
	unsigned filemask:3;
	unsigned processed:1;
	unsigned clean:1;
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

#if 0
static inline int merge_detect_rename(struct merge_options *opt)
{
	return (opt->detect_renames >= 0) ? opt->detect_renames : 1;
}
#endif

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

/***** End copy-paste static functions from merge-recursive.c *****/

static void setup_path_info(struct string_list_item *result,
			    struct traverse_info *info,
			    struct name_entry *names,
			    const char *current_dir_name,
			    unsigned is_null,     /* boolean */
			    unsigned df_conflict, /* boolean */
			    unsigned filemask,
			    int resolved          /* boolean */)
{
	size_t len = traverse_path_len(info, names->pathlen);
	char *fullpath = xmalloc(len+1);  /* +1 to include the NUL byte */
	struct conflict_info *path_info = xcalloc(1, sizeof(*path_info));

	assert(!is_null || resolved);      /* is_null implies resolved */
	assert(!df_conflict || !resolved); /* df_conflict implies !resolved */

	make_traverse_path(fullpath, len, info, names->path, names->pathlen);
	path_info = xcalloc(1, resolved ? sizeof(struct merged_info) :
					  sizeof(struct conflict_info));
	path_info->merged.directory_name = current_dir_name;
	path_info->merged.basename_offset = info->pathlen + !!info->pathlen;
	if (resolved) {
		path_info->merged.result.mode = names->mode;
		oidcpy(&path_info->merged.result.oid, &names->oid);
		path_info->merged.result_is_null = !!is_null;
	} else {
		int i;

		for (i = 0; i < 3; i++) {
			if (!(filemask & (1ul << i)))
				continue;
			path_info->stages[i].mode = names[i].mode;
			oidcpy(&path_info->stages[i].oid, &names[i].oid);
		}
		path_info->filemask = filemask;
		path_info->df_conflict = !!df_conflict;
	}
	result->string = fullpath;
	result->util = path_info;
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
	struct merge_options_internal *opt = info->data;
	struct string_list_item pi;  /* Path Info */
	unsigned prev_iprd = opt->inside_possibly_renamed_dir; /* prev value */
	unsigned filemask = mask & ~dirmask;
	unsigned mbase_null = !(mask & 1);
	unsigned side1_null = !(mask & 2);
	unsigned side2_null = !(mask & 4);
	unsigned side1_is_tree = (dirmask & 2);
	unsigned side2_is_tree = (dirmask & 4);
	unsigned side1_matches_mbase = oideq(&names[0].oid, &names[1].oid);
	unsigned side2_matches_mbase = oideq(&names[0].oid, &names[2].oid);
	unsigned sides_match = oideq(&names[1].oid, &names[2].oid);
	/*
	 * Note: We only label files with df_conflict, not directories.
	 * Since directories stay where they are, and files move out of the
	 * way to make room for a directory, we don't care if there was a
	 * directory/file conflict for a parent directory of the current path.
	 */
	unsigned df_conflict = (filemask != 0) && (dirmask != 0);

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
	 * If mbase, side1, and side2 all match, we can resolve early.  Even
	 * if these are trees, there will be no renames or anything
	 * underneath.
	 */
	if (side1_matches_mbase && side2_matches_mbase) {
		/* mbase, side1, & side2 all match; use mbase as resolution */
		setup_path_info(&pi, info, names+0, opt->current_dir_name,
				mbase_null, 0, filemask, 1);
		strmap_put(&opt->merged, pi.string, pi.util);
		return mask;
	}

	/*
	 * If all three paths are files, then there will be no renames
	 * either for or under this path.  If additionally the sides match,
	 * we can take either as the resolution.
	 */
	if (filemask == 7 && sides_match) {
		/* use side1 (== side2) version as resolution */
		setup_path_info(&pi, info, names+1, opt->current_dir_name,
				0, 0, filemask, 1);
		strmap_put(&opt->merged, pi.string, pi.util);
		return mask;
	}

	/*
	 * If side1 matches mbase, and side2 is not a tree, we can resolve
	 * early because side1 matching mbase implies:
	 *   - side1 has no interesting contents or changes; use side2 versions
	 *   - side1 has no content changes to include in renames on side2 side
	 *   - side1 contains no new files to move with side2's directory renames
	 * We can't resolve early if side2 is a tree though, because there
	 * may be new files on side2's side that are rename targets that need
	 * to be merged with changes from elsewhere on side1's side of history.
	 */
	if (!opt->inside_possibly_renamed_dir &&
	    !side1_null && side1_matches_mbase && !side2_is_tree) {
		/* use side2 version as resolution */
		setup_path_info(&pi, info, names+2, opt->current_dir_name,
				side2_null, 0, filemask, 1);
		strmap_put(&opt->merged, pi.string, pi.util);
		return mask;
	}

	/*
	 * If side2 matches mbase, and side1 is not a tree, we can resolve
	 * early.  Same reasoning as for above but with side1 and side2
	 * swapped.
	 */
	if (!opt->inside_possibly_renamed_dir &&
	    !side2_null && side2_matches_mbase && !side1_is_tree) {
		/* use side1 version as resolution */
		setup_path_info(&pi, info, names+1, opt->current_dir_name,
				side1_null, 0, filemask, 1);
		strmap_put(&opt->merged, pi.string, pi.util);
		return mask;
	}

	/*
	 * None of the special cases above matched, so we have a
	 * provisional conflict.  (Rename detection might allow us to
	 * unconflict some more cases, but that comes later so all we can
	 * do now is record the different non-null file hashes.)
	 */
	setup_path_info(&pi, info, names, opt->current_dir_name,
			0, df_conflict, filemask, 0);
	strmap_put(&opt->unmerged, pi.string, pi.util);
	if (filemask == 0)
		opt->nr_dir_only_entries += 1;

	/*
	 * Record directories which could possibly have been renamed.  Notes:
	 *   - Directory has to exist in mbase to have been renamed (i.e.
	 *     dirmask & 1 must be true)
	 *   - Directory cannot exist on both sides or it isn't renamed
	 *     (i.e. !(dirmask & 2) or !(dirmask & 4) must be true)
	 *   - If directory exists in neither side1 nor side2, then
	 *     there are no new files to send along with the directory
	 *     rename so there's no point detecting it[1].  (Thus, either
	 *     dirmask & 2 or dirmask & 4 must be true)
	 *   - If the side that didn't rename a directory also didn't
	 *     modify it at all (i.e. the par[12]_matches_mbase cases
	 *     checked above were true), then we don't need to detect the
	 *     directory rename as there are not either any new files or
	 *     file modifications to send along with the rename.  Thus,
	 *     it's okay that we returned early for the
	 *     par[12]_matches_mbase cases above.
	 *
	 * [1] When neither side1 nor side2 has the directory then at
	 *     best, both sides renamed it to the same place (which will be
	 *     handled by all individual files being renamed to the same
	 *     place and no dir rename detection is needed).  At worst,
	 *     they both renamed it differently (but all individual files
	 *     are renamed to different places which will flag errors so
	 *     again no dir rename detection is needed.)
	 */
	if (dirmask == 3 || dirmask == 5) {
		/*
		 * For directory rename detection, we can ignore any rename
		 * whose source path doesn't start with one of the directory
		 * paths in possible_dir_rename_bases.
		 */
		strmap_put(&opt->possible_dir_rename_bases, pi.string, NULL);
		opt->inside_possibly_renamed_dir = 1;
	}

	/* If dirmask, recurse into subdirectories */
	if (dirmask) {
		struct traverse_info newinfo;
		struct name_entry *p;
		struct tree_desc t[3];
		void *buf[3] = {NULL,};
		const char *original_dir_name;
		int ret;
		int i;

		p = names;
		while (!p->mode)
			p++;

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

		original_dir_name = opt->current_dir_name;
		opt->current_dir_name = pi.string;
		ret = traverse_trees(NULL, 3, t, &newinfo);
		opt->current_dir_name = original_dir_name;
		opt->inside_possibly_renamed_dir = prev_iprd;

		for (i = 0; i < 3; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}
	return mask;
}

static int collect_merge_info(struct merge_options *opt,
			      struct tree *merge_base,
			      struct tree *side1,
			      struct tree *side2)
{
	int ret;
	struct tree_desc t[3];
	struct traverse_info info;

	setup_traverse_info(&info, "");
	info.fn = collect_merge_info_callback;
	info.data = opt;
	info.show_all_errors = 1;

	parse_tree(merge_base);
	parse_tree(side1);
	parse_tree(side2);
	init_tree_desc(t+0, merge_base->buffer, merge_base->size);
	init_tree_desc(t+1, side1->buffer, side1->size);
	init_tree_desc(t+2, side2->buffer, side2->size);

	trace_performance_enter();
	ret = traverse_trees(NULL, 3, t, &info);
	trace_performance_leave("traverse_trees");

	return ret;
}

/* Per entry merge function */
static void process_entry(struct merge_options *opt, struct str_entry *entry)
			  /*
			  const char *path, struct conflict_info *entry)
			  */
{
	/* const char *path = entry->item.string; */
	struct conflict_info *ci = entry->item.util;

	/* int normalize = opt->renormalize; */

#if 0
	struct version_info *o = &ci->stages[1];
	struct version_info *a = &ci->stages[2];
	struct version_info *b = &ci->stages[3];
#endif

	assert(!ci->processed);
	ci->processed = 1;
	assert(ci->filemask >=0 && ci->filemask < 8);
	if (ci->filemask == 0) {
		/*
		 * This is a placeholder for directories that were recursed into.
		 * Nothing to do.
		 */
		return;
	}
	if (ci->df_conflict) {
		assert(ci->filemask >= 1 && ci->filemask <= 6);
		/*
		 * FIXME: Move the files out of the way, then fall-through to
		 * below entries for the alternate record.
		 */
	}
	if (ci->filemask >= 6) {
#if 0
		struct merge_file_info *mfi;
		unsigned clean_merge;
#endif

		assert(!ci->df_conflict);
#if 0
		clean_merge = handle_content_merge(&mfi, opt, path,
						   o, a, b, NULL);
		if (clean_merge) {
			/* FIXME: implement */
		}
#endif
	}
	if (ci->filemask == 3 || ci->filemask == 5) {
		/* FIXME: Handle modify/delete */
		int side = (ci->filemask == 5) ? 3 : 2;
		ci->merged.result.mode = ci->stages[side].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
		ci->clean = 0;
	} else if (ci->filemask == 2 || ci->filemask == 4) {
		/* Added on one side */
		int side = (ci->filemask == 4) ? 3 : 2;
		ci->merged.result.mode = ci->stages[side].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
		ci->clean = 1;
	} else if (ci->filemask == 1) {
		/* Deleted on both sides */
		ci->merged.result_is_null = 1;
		ci->merged.result.mode = 0;
		oidcpy(&ci->merged.result.oid, &null_oid);
		ci->clean = 1;
	}
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
	char root_dir[1] = "\0";

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

	opt->priv->current_dir_name = root_dir;
	code = collect_merge_info(opt, merge_base, head, merge);

	if (code != 0) {
		if (show(opt, 4) || opt->priv->call_depth)
			err(opt, _("collecting merge info for trees %s and %s failed"),
			    oid_to_hex(&head->object.oid),
			    oid_to_hex(&merge->object.oid));
		return -1;
	}

	if (strmap_get_size(&opt->priv->unmerged) ==
	    opt->priv->nr_dir_only_entries) {
		clean = 1;
	} else {
		struct hashmap_iter iter;
		struct str_entry *entry;
		clean = 0;
		strmap_for_each_entry(&opt->priv->unmerged, &iter, entry) {
			process_entry(opt, entry);
		}
	}

	return clean;
}

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
			      struct commit **result)
{
	struct commit_list *iter;
	struct commit *merged_merge_bases;
	struct tree *result_tree;
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
		if (merge_ort_internal(opt, merged_merge_bases, iter->item,
				       NULL, &merged_merge_bases) < 0)
			return -1;
		opt->branch1 = saved_b1;
		opt->branch2 = saved_b2;
		opt->priv->call_depth--;

		if (!merged_merge_bases)
			return err(opt, _("merge returned no commit"));
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
				     &result_tree);
	strbuf_release(&merge_base_abbrev);
	opt->ancestor = NULL;  /* avoid accidental re-use of opt->ancestor */
	if (clean < 0) {
		flush_output(opt);
		return clean;
	}

	if (opt->priv->call_depth) {
		*result = make_virtual_commit(opt->repo, result_tree,
					      "merged tree");
		commit_list_insert(h1, &(*result)->parents);
		commit_list_insert(h2, &(*result)->parents->next);
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

	assert(opt->priv == NULL);

	/* Sanity check on repo state; index must match head */
	if (repo_index_has_changes(opt->repo, head, &sb)) {
		err(opt, _("Your local changes to the following files would be overwritten by merge:\n  %s"),
		    sb.buf);
		strbuf_release(&sb);
		return -1;
	}

	strmap_init(&opt->priv->merged, 0);
	strmap_init(&opt->priv->unmerged, 0);
	opt->priv = xcalloc(1, sizeof(*opt->priv));
	return 0;
}

static void merge_finalize(struct merge_options *opt)
{
	flush_output(opt);
	if (!opt->priv->call_depth && opt->buffer_output < 2)
		strbuf_release(&opt->obuf);
	if (show(opt, 2))
		diff_warn_rename_limit("merge.renamelimit",
				       opt->priv->needed_rename_limit, 0);

	/*
	 * possible_dir_rename_bases reuse the same strings found in
	 * opt->priv->unmerged, so they'll be freed below.
	 */
	strmap_clear(&opt->priv->possible_dir_rename_bases, 1);

	/*
	 * We marked opt->priv->[un]merged with strdup_strings = 0, so that
	 * we wouldn't have to make another copy of the fullpath created by
	 * make_traverse_path from setup_path_info().  But, now that we've
	 * used it and have no other references to these strings, it is time
	 * to deallocate them, which we do by just setting strdup_string = 1
	 * before the strmaps are cleared.
	 */
	opt->priv->unmerged.strdup_strings = 1;
	opt->priv->merged.strdup_strings = 1;
	strmap_clear(&opt->priv->unmerged, 1);
	strmap_clear(&opt->priv->merged, 1);

	FREE_AND_NULL(opt->priv);
}

int merge_ort_nonrecursive(struct merge_options *opt,
			   struct tree *head,
			   struct tree *merge,
			   struct tree *merge_base)
{
	int clean;
	struct tree *ignored;

	assert(opt->ancestor != NULL);

	if (merge_start(opt, head))
		return -1;
	clean = merge_ort_nonrecursive_internal(opt, head, merge, merge_base,
						&ignored);
	merge_finalize(opt);

	return clean;
}

int merge_ort(struct merge_options *opt,
	      struct commit *h1,
	      struct commit *h2,
	      struct commit_list *merge_bases,
	      struct commit **result)
{
	int clean;

	assert(opt->ancestor == NULL ||
	       !strcmp(opt->ancestor, "constructed merge base"));

	if (merge_start(opt, repo_get_commit_tree(opt->repo, h1)))
		return -1;
	clean = merge_ort_internal(opt, h1, h2, merge_bases, result);
	merge_finalize(opt);

	return clean;
}
