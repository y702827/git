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
	int call_depth;
	int needed_rename_limit;
};

struct version_info {
	unsigned short mode;
	struct object_id oid;
};

struct merged_info {
	struct version_info result;
	size_t path_len;
	size_t basename_len;
};

struct conflict_info {
	struct merged_info merged;
	struct version_info stages[3];
	unsigned *df_counter[3];
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

static void init_tree_desc_from_tree(struct tree_desc *desc, struct tree *tree)
{
	parse_tree(tree);
	init_tree_desc(desc, tree->buffer, tree->size);
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

/***** End copy-paste static functions from merge-recursive.c *****/

static void setup_path_info(struct string_list_item *result,
			    struct traverse_info *info,
			    struct name_entry *names,
			    unsigned filemask,
			    int resolved)
{
	size_t len = traverse_path_len(info, names->pathlen);
	char *fullpath = xmalloc(len+1);  /* +1 to include the NUL byte */
	struct conflict_info *path_info = xcalloc(1, sizeof(*path_info));
	int i;

	make_traverse_path(fullpath, len, info, names->path, names->pathlen);
	path_info = xcalloc(1, resolved ? sizeof(struct merged_info) :
					  sizeof(struct conflict_info));
	path_info->merged.path_len = len;
	path_info->merged.basename_len = names->pathlen;
	if (resolved) {
		path_info->merged.result.mode = names->mode;
		oidcpy(&path_info->merged.result.oid, &names->oid);
	} else {
		for (i = 0; i < 3; i++) {
			if (!(filemask & (1ul << i)))
				continue;
			path_info->stages[i].mode = names[i].mode;
			oidcpy(&path_info->stages[i].oid, &names[i].oid);
		}
		path_info->filemask = filemask;
		/* path_info->df_conflicts = ????; */
	}
	result->string = fullpath;
	result->util = path_info;
}

static int threeway_simple_merge_callback(int n,
					  unsigned long mask,
					  unsigned long dirmask,
					  struct name_entry *names,
					  struct traverse_info *info)
{
	/*
	 * n is 3.  Or BUG, see below.
	 * common ancestor (base) stored in index 0, uses a mask of 1
	 * parent 1        (par1) stored in index 1, uses a mask of 2
	 * parent 2        (par2) stored in index 2, uses a mask of 3
	 */
#if 0
	unsigned long conflicts = info->df_conflicts | dirmask;
#endif
	unsigned base_null, par1_null, par2_null;
	unsigned par1_or_par2_are_trees;
	unsigned long filemask = mask & ~dirmask;
	unsigned par1_match = oideq(&names[0].oid, &names[1].oid);
	unsigned par2_match = oideq(&names[0].oid, &names[2].oid);
	struct string_list_item entry;
	struct merge_options_internal *opt = info->data;

	if (n != 3)
		BUG("Called threeway_simple_merge_callback wrong");

	base_null = !(mask & 1);
	par1_null = !(mask & 2);
	par2_null = !(mask & 4);
	par1_or_par2_are_trees = (dirmask & 6);
	/* FIXME: Remove these sanity checks at some point */
	assert(base_null == is_null_oid(&names[0].oid));
	assert(par1_null == is_null_oid(&names[1].oid));
	assert(par2_null == is_null_oid(&names[2].oid));

	/*
	 * If par1 matches base, we can resolve early.  We can ignore base
	 * (and everything under it if it's a tree) as a possible rename
	 * source for something on par2's side of history because a three
	 * way merge of base matching par1 will just take whatever par2
	 * had anyway.
	 */
	if (!base_null && !par1_null) {

		if (par1_match) {
			/* par1_match => use par2 version as resolution */
			setup_path_info(&entry, info, names+2, filemask, 1);
			strmap_put(&opt->merged, entry.string, entry.util);
		}
		return mask;
	}

	/*
	 * If par2 matches base, we can resolve early.  As with the previous
	 * case, we can ignore base as a possible rename for anything on
	 * par1's side of history.
	 */
	if (!base_null && !par2_null) {
		if (par2_match) {
			/* par2_match => use par1 version as resolution */
			setup_path_info(&entry, info, names+1, filemask, 1);
			strmap_put(&opt->merged, entry.string, entry.util);
		}
		return mask;
	}

	/*
	 * If par1 & par2 are files and match, we can resolve.  Any renames
	 * which have either of these two as targets can be ignored because
	 * a three-way merge would end up matching these two files.  Also,
	 * in the rare case of finding different matching bases, we'd end
	 * up with a rename/rename(2to1) case but which wouldn't conflict
	 * because the contents of both sides match so we can again ignore
	 * the rename.
	 */
	if (!par1_null && !par2_null && !par1_or_par2_are_trees &&
	    oideq(&names[1].oid, &names[2].oid)) {
		/*
		 * par1 & par2_match and both are files =>
		 *   use par1 (==par2) version as resolution, but don't return
		 *   early (may need to recurse into base if it's a tree)
		 */
		setup_path_info(&entry, info, names+1, filemask, 1);
		strmap_put(&opt->merged, entry.string, entry.util);
	/*
	 * None of the special cases above matched, so we have a
	 * provisional conflict.  (Rename detection might allow us to
	 * unconflict some more cases, but that comes later; for now just
	 * record the different non-null file hashes.)
	 */
	} else {
		setup_path_info(&entry, info, names, filemask, 0);
		strmap_put(&opt->unmerged, entry.string, entry.util);
	}

	/* If dirmask, recurse into subdirectories */
	if (dirmask) {
		struct traverse_info newinfo;
		struct name_entry *p;
		struct tree_desc t[3];
		void *buf[3] = {NULL,};
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
		newinfo.df_conflicts |= (mask & ~dirmask);

		for (i = 0; i < 3; i++, dirmask >>= 1) {
			if (i > 0 && oideq(&names[i].oid, &names[i - 1].oid))
				t[i] = t[i - 1];
			else {
				const struct object_id *oid = NULL;
				if (dirmask & 1)
					oid = &names[i].oid;
				buf[i] = fill_tree_descriptor(the_repository, t + i, oid);
			}
		}

		ret = traverse_trees(NULL, 3, t, &newinfo);

		for (i = 0; i < 3; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}
	return mask;
}

static int preliminary_merge_trees(struct merge_options *opt,
				   struct tree *base,
				   struct tree *parent1,
				   struct tree *parent2)
{
	int ret;
	struct tree_desc t[3];
	struct traverse_info info;

	setup_traverse_info(&info, "");
	info.fn = threeway_simple_merge_callback;
	info.data = opt;
	info.show_all_errors = 1;

	init_tree_desc_from_tree(t+0, base);
	init_tree_desc_from_tree(t+1, parent1);
	init_tree_desc_from_tree(t+2, parent2);

	trace_performance_enter();
	ret = traverse_trees(NULL, 3, t, &info);
	trace_performance_leave("traverse_trees");

	return ret;
}

#if 0
static int merge_entry_contents(struct merge_options *opt,
				const struct cache_entry *o,
				const struct cache_entry *a,
				const struct cache_entry *b,
				const char *branch1,
				const char *branch2,
				const int extra_marker_size,
				struct cache_entry *result)
{
	BUG("Not yet implemented!");
}

static const struct cache_entry *get_next(struct index_state *index,
					  int *cur)
{
	if (*cur < 0)
		++*cur;
	else /* Skip unmerged entries with the same name */
		while (++*cur < index->cache_nr &&
		       index->cache[*cur-1]->ce_namelen ==
			 index->cache[*cur]->ce_namelen &&
		       !strncmp(index->cache[*cur-1]->name,
				index->cache[*cur]->name,
				index->cache[*cur]->ce_namelen))
			; /* skip */

	return (*cur < index->cache_nr) ? index->cache[*cur] : NULL;
}
#endif

/* Per entry merge function */
static int process_entry(struct merge_options *opt,
			 const char *path, struct conflict_info *entry)
{
	int clean_merge = 1;
	/* int normalize = opt->renormalize; */

	//struct version_info *o = &entry->stages[1];
	struct version_info *a = &entry->stages[2];
	//struct version_info *b = &entry->stages[3];
	/* o->path = a->path = b->path = (char*)path; */

	entry->processed = 1;
	/* FIXME: Handle renamed cases */
	/* FIXME: Next two blocks implements an 'ours' strategy. */
	if (a->mode != 0 && !is_null_oid(&a->oid)) {
		entry->stages[0].mode = a->mode;
		oidcpy(&entry->stages[0].oid, &a->oid);
		entry->clean = 1;
	} else {
		entry->stages[0].mode = 0;
		oidcpy(&entry->stages[0].oid, &null_oid);
		entry->clean = 1;
	}

	return clean_merge;
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

	if (opt->priv->call_depth) {
		discard_index(opt->repo->index);
	}

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

	code = preliminary_merge_trees(opt, merge_base, head, merge);

	if (code != 0) {
		if (show(opt, 4) || opt->priv->call_depth)
			err(opt, _("merging of trees %s and %s failed"),
			    oid_to_hex(&head->object.oid),
			    oid_to_hex(&merge->object.oid));
		return -1;
	}

	if (strmap_empty(&opt->priv->unmerged)) {
		clean = 1;
	} else {
		struct hashmap_iter iter;
		struct str_entry *entry;
		clean = 0;
		strmap_for_each_entry(&opt->priv->unmerged, &iter, entry) {
			const char *path = entry->item.string;
			struct conflict_info *e = entry->item.util;
			if (!e->processed) {
				int ret = process_entry(opt, path, e);
				if (!ret)
					clean = 0;
				else if (ret < 0) {
					return ret;
				}
			}
		}
	}

	if (opt->priv->call_depth &&
	    !(*result = write_in_core_index_as_tree(opt->repo)))
		return -1;

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
