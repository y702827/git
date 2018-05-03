* Leftover bits:
  * Modernize testcases for rebase -{i,m} with strategy options
    * https://public-inbox.org/git/CABPp-BF_HDW-ie1ZaTrnniWuE7FxThiukk0xJ7P5rfUiwXBGug@mail.gmail.com/
  * Make merge.renames/diff.renames/-Xfind-renames affect diffstat at end
    * https://public-inbox.org/git/CABPp-BGGtLGKVGY_ry8=sdi=s=EDphzTADt+0UR1F4NJpSqmFw@mail.gmail.com/
  * Document special value of 0 for diff.renameLimit
    * https://public-inbox.org/git/20180426162339.db6b4855fedb5e5244ba7dd1@google.com/
  * Get rid of splits: {diff,merge}_detect_rename, {diff,merge}_rename_limit
  * Anything on the 'misc' or 'wip' branches

* GVFS-like experience
  * git-bomb repo (add object, add tree with 10 entries all pointing to that
    object with different names, then another tree with 10 entries all pointing
    to that tree with 10 different names, etc. until we have 1M trees)
  * sparse checkout pattern that ignores nearly all paths
  * small modification to oid_object_info_extended() to add a short pause
  * Use this repo to time operations; checkout new branch, merge, etc.

* flag/command-name consistency
  * Implement cherry-pick --skip
  * Make commit error message during rebase or cherry-pick show 'rebase --skip'
    or 'cherry-pick --skip' as advice rather than 'If you wish to skip this...
    git reset'
  * sequencer.c:create_seq_dir():
      error(_("a cherry-pick or revert is already in progress"));
      advise(_("try \"git cherry-pick (--continue | --quit | --abort)\""));
    Advising to 'cherry-pick --continue' for a revert??  Which message is wrong?
  * When rebase [--am] halts, it shouldn't show 'git am --show-current-patch'
    as hint; it should show 'git rebase --show-current-patch'.

* rebase consistency
  * Make git commit mention {rebase,cherry-pick} --skip instead of reset
  * Add rebase --empty={drop,halt,keep}
  * Implement missing flags
    * Implement --ignore-space-change/--ignore-whitespace by transliterating
      to -Xignore-space-change
    * Implement --whitespace
    * Implement --comitter-date-is-author-date
    * Implement -C4
  * Add --am flag
  * Do heavy testing of performance of am vs. interactive machinery
  * Change default to interactive machinery

* Add testcases for conflict types we don't handle well
  * rename issues (??)
    * (suspected problems) rename/rename(1to2) with multiple D/F conflicts
    * chain r/r types not just to each other but to adds/deletes as well
      (e.g. del/ren/ren(2to1) + ren/ren1to2 + ren/ren2to1 + ren/ren(1to2)/add)
  * issues with submodules
    * clean deletion -- does it actually delete?  (There is code suggesting not)

* Script or program that will test every commit in next/pu
  * Things to check
    * builds
    * tests pass
    * linux/scripts/checkpatch.pl
    * 'git grep -e '\<inline\>' -and -not -e 'static inline' -- \*.h
      * https://public-inbox.org/git/xmqqefist8xr.fsf@gitster-ct.c.googlers.com/
    * submissions/sanity_check_test_scripts.sh
    * commit_msg =~ /[^ ]*: [A-Z]/; I always miss that capital...
    * Stuff from
      https://public-inbox.org/git/20180710044456.GA1870@sigill.intra.peff.net/
  * run it on IL

* Merge output cleanup
  * Print all detected directory renames (at directory level, not file level)
  * Avoid rename/rename(1to2) per-file warnings when several files in the
    same directory are all part of the same overall rename/rename(1to2)
    directory conflict

* super-quiet merge output
  * pass option to allow turning all normal output off, just returns exit
    status and tree.  (Caller might print exit status and/or tree, depending on
    whether being used by remerge-diff or bare-repo merge.)
  * should avoid "Auto-merging", conflict messages, submodule merging, etc.
  * should have tests to enforce this as much as possible

* skipping unnecessary updates
  * file collision conflicts (add/add, rename/add, rename/rename(2to1)) can
    result in unnecessary updates if both files are identical.

* merge file collision consistency
  * make rename/add and rename/rename(2to1) act like add/add
  * https://public-inbox.org/git/xmqq7eqf1tc9.fsf@gitster-ct.c.googlers.com/
  * github/merge-file-collision-consistency-v3

* pathname conflicts hints for worktree
  * Example: chains of rename/rename(1to2) and rename/rename(2to1)
    * File contents
      * start with files named 'one', 'three', and 'five'
      * one side:   git mv one   two && git mv three four && git mv five six
      * other side: git mv three two && git mv five  four && git mv one  six
    * Brief explanation of what happened
      * sources: one -> {two, six}, three -> {two, four} , five -> {four, six}
      * dests:   two <- {one, three}, four <- {three, five}, six <- {five, one}
    * Resolutions:
      * recursive case: content merge into original files (undo all renames)
      * remerge-diff:
        * 1) for each source
          * 1a) do three-way merge of contents
          * 1b) add conflict header for each side ("was r/r(1to2) conflict...")
        * 2) for each dest
          * two-way merge the two different files
      * normal, index: Step 1 of remerge-diff, but record in relevant stages
      * normal, worktree: See remerge-diff
  * Cases where this matters:
    * submodule conflicts???
    * directory/file
    * modify/delete
    * rename/delete
    * rename/add
    * rename/add/delete
    * rename/rename(1to2)[/add[/add]]
    * rename/rename(2to1)/delete[/delete]
    * path in way of directory rename
    * multiple colliding paths due to directory rename
    * indeterminate split for directory rename
  * Cases where it doesn't:
    * add/add
    * rename/add
    * rename/rename(2to1) (only needed if delete involved)
    * multiply-transitive renames (not a conflict, so shouldn't modify)
  * https://public-inbox.org/git/CABPp-BHxJyWsAQ3FkfdC-5Vqe3d7wWZm-hVYd0-afNY9dEgMeQ@mail.gmail.com/
  * github/conflict-handling && github/conflict-handling-extras

* merge resolution sanity
  * depends on:
    * merge file collision consistency
    * pathname conflicts hints for worktree

* Fix clean with pathspec
  * https://public-inbox.org/git/20180405193124.GA24643@sigill.intra.peff.net/
  * Might also mean cleaning up lots of callers of fill_directory()

* Make break and rename detection safe to use together
  * https://crbug.com/git/15
  * https://public-inbox.org/git/xmqqegqaahnh.fsf@gitster.dls.corp.google.com/
  * https://public-inbox.org/git/BANLkTimkm4SU8nML_6Q7Q34faBziJ=gheA@mail.gmail.com/

* avoid splitting conflict information feedback in merge
  * "CONFLICT(rename/add)" and "CONFLICT(content)" for the same path can
    be separated by many unreleated intermediate output messages
  * This comes from a combination of printing rename/add conflict as soon as
    we notice, but deferring working copy update and thus the merge_content
    call until much later
  * https://public-inbox.org/git/CABPp-BGJiS96_wXTp4dpVG4CpTEt--KGELykofimP-Wh4nFhdg@mail.gmail.com/

* merge rewrite (ort)
  * more accurate
    * more conflict types
      * rename/add/delete
      * rename/rename(2to1)/delete/delete
      * chains of rename/rename(1to2) and rename/rename(2to1)
      * issues with submodules
        * https://public-inbox.org/git/CABPp-BHDrw_dAESic3xK7kC3jMgKeNQuPQF69OpbVYhRkbhJsw@mail.gmail.com/
  * faster
    * renames:
      * work already submitted as RFC
      * ...but don't break directory rename detection
      * instead of N-way search, first try file with same basename, and accept
        as rename if >75% similarity.  (configurable?)
    * hashes instead of lists, where appropriate (might already be done)
    * O(N) update to index, not O(N^2) -- both adds and deletes
  * maintainability
    * merge o->{diff,merge}_renames and o->{diff,merge}_rename_limit
    * big high-level explanation at top of file
      * index-only unpack trees
      * rename detection, including directory renames
        * break detection not done, requires lots of fixups (add-source)
        * copy detection makes no sense
        * file-level detection only; PICKAXE_BLAME_MOVE would be awesome, but
          need lots of work to wire up
      * merge only affects the index
      * writes intermediate file merges to object store
      * after done, checks if any dirty or untracked files are in the way
      * if nothing in the way, starts updating the working copy
    * rewrite of process_renames()
    * explain point of various structs better (like struct rename)
  * see also:
    * ort-cover-letter ('ort' branch)
    * remaining-todo ('untracked' branch)

* remerge-diff
  * depends on:
    * merge-rewrite
    * super-quiet merge output
  * affects: merge commits, revert commits, and cherry-pick'ed commits
  * challenges:
    * https://public-inbox.org/git/CABPp-BEsTOZ-tCvG1y5a0qPB8xJLLa0obyTU===mBgXC1jXgFA@mail.gmail.com/
  * forcibly create tree to diff from
    * modify/delete or rename/delete: put conflicted header explaining issue
      at top of file.  (Or two-way diff with /dev/null, but then if we're just
      looking for stuff outside-of-conflict-range, then this puts everything
      in-range)
    * directory/file conflicts -- alternate location with conflicted header?
    * rename/rename(1to2) -- put conflicts in both places?
    * binary and submodules: when conflict...use merge-base?
    * other path conflict handling?
      * something in way of directory-rename detection for a path
      * too many things mapping to same path with directory-rename detection
      * directory rename split
      * (only warning?) avoiding multiply transitive renames
    * nested conflict levels?
      * rename/delete flagged for transitive rename but path in way
      * modify/delete tagged for transitive rename but path in way

* bare-repo merge
  * depends on:
    * remerge-diff
  * merge to a temporary index, or forcibly create tree to diff against and
    report the tree?

* Palantir-related stuff
  * snowflake report related
    * multi-branch git-cherry (avoid repeatedly calling git-cherry)
      * ..or just call git-patch-id instead of git-cherry and store results?
    * open source snowflake report
    * rewrite snowflake report in C?
  * cherry-pick or rebase to un-checked-out branch (needs bare-repo merge)

* New Strategy Option: Maximize Accumulated Similarities (-Xmas)
  * Highly indirect motivation from (note: completely different algorithm):
    * https://jneem.github.io/{merging,pijul}
  * Basic algorithm
    * In merge, find all merge bases
    * compute virtual merge base normally, keep original merge bases around
    * enhance virtual merge base:
      * find all ids of patch hunks in the range: HEAD --not $merge_bases
      * find all ids of patch hunks in the range: MERGE_HEAD --not $merge_bases
      * apply common patch hunks in order to virtual merge base (& create tree)
        * if any errors, just skip those hunks
      * use new tree as new virtual merge base
      * make sure to output tree somewhere so user can diff against it
  * Challenges:
    * renames may mean that patch hunks are against different filename
  * Improved merges:
    * fix cherry-picked, then reverted in one branch, then branches merged
    * commit cherry-picked, then additional changes on same lines on one side

* filter-repo: a much better replacement for filter-branch (and maybe BFG??)
  * default to rewrite all branches, and default to expunge old history
  * if any cruft temporary objects created during process, expunge those too
  * work with fast-export/fast-import; try to use --no-data if possible
  * safety: provide flag to override but otherwise abort early if any of:
    * working tree unclean
    * index doesn't match HEAD
    * $BRANCH != origin/$BRANCH for any $BRANCH
    * repo not fully repacked with garbage pruned
    * more than one branch has a reflog
    * reflog for current branch contains more than one entry
  * work in bare repos too
  * Mode that just provides statistics (e.g. object size) instead of actually
    filtering
  * Incorporate fixes from Ken Brownfield in final email from 2010 thread
  * Flags of filter-branch to implement
    * -d
    * -f/--force
    * --subdirectory-filter
    * --prune-empty
    * --tag-name-filter (but rename to --tag-name-stream-filter?)
    * --msg-filter (but should it apply to annotated tags too?)
  * Flags of filter-branch to modify a bit:
    * --tree-filter
      * Write out touched files to working tree.  Let user mess with them.
    * --parent-filter
      * Provide commit and parents, space separated, on input
      * Read back commit and new parents, space separated, on output
      * Check that user didn't introduce cycles; may be hard...
        * Early version, require that new parents are in ancestry of commit
  * Flags of filter-branch I don't know if I want:
    * --env-filter (provide alternatives, e.g. --name-filter, --email-filter,
                    --time-filter -- though those would apply to tags too)
    * --index-filter
    * --commit-filter
  * Stuff from BFG that would be good
    * -b/--strip-blobs-bigger-than $SIZE
    * --private (commit messages from revert and cherry-pick -x need to
                 reference new sha1sums)
  * Stuff from BFG to change
    * -p/--protect-blobs-from/--no-blob-protection
      * Save treeids of all branches at start (maybe in some special ref?)
      * At end, check if branches have same tree-id; if not, warn user
  * Stuff from BFG to consider
    * -B/--strip-biggest-blobs $NUM
    * -bi/--strip-blobs-with-ids $FILENAME
    * -D/--delete-files $BASENAME_GLOB
    * --delete-folders $BASENAME_GLOB
    * --convert-to-git-lfs $BASENAME_GLOB
    * --replace-text $EXPRESSIONS_FILE
    * --filter-content-including $BASENAME_GLOB (limit where other filters apply)
    * --filter-content-excluding $BASENAME_GLOB (limit where other filters apply)
    * --filter-content-size-threshold $SIZE (limit where other filters apply)
    * --massive-non-file-objects-sized-up-to $SIZE
    * $REPO ?? (possibly incompatible with revision args)
  * New filters
    * --branch-name-stream-filter (similar to --tag-name-stream-filter)
    * One version of --path-filter:
      * Create identical path structure in $TMPDIR
      * Create placeholder files whose contents are at most a few bytes in size
        (unrelated to real contents), and all with different contents
      * Run user-specified commands (e.g. mv, rm, etc.)
      * Read entire directory; and:
        * Error out if any files with unknown content are found
        * If file of certain original content not found, assume deleted
        * If file of certain original content found, record oldpath->newpath
        * error out if oldpath->newpath gives different mapping than
          run of --path-filter on a previous commit
      * If a commit only has paths for which we already have oldpath->newpath
        mappings, skip re-running the new mapping check
    * Another version of --path-filter:
      * --path-filter[-{glob,regex}[-{and,or}[-not]]]
        * or's combine with previous flags
        * and's start a new set, e.g
          --path-filter-regex-or foo.*.txt   \
            --path-filter-regex-or .*bar.*   \
            --path-filter-regex-and .*what.* \
            --path-filter-regex-or .*ever.*
          ==
          --path-stream-filter \
            'grep -e foo.*.txt -e .*bar.* | grep -e .*what.* -e .*ever.*'
      * Aborts if any filename has a newline
    * For --path-stream-filter:
      * Just pipe filenames, one per line, into command and read them back
      * Never repeat a filename
      * Aborts if any filename has a newline
    * Special flags to make these easy (sometimes hard with fast-export):
      * Rewrite only a range of commits, but keep parents outside of range
        1) git fast-export --no-data --export-marks=known --signed-tags=strip \
	   $OLDER_REVS
        2) git fast-export --no-data --import-marks=known \
	   $NEWER_REVS --not $OLDER_REVS >out
        3) cat out | transform_program | git fast-import --quiet --force \
	   --import-marks=known --force
      * Deleting biggest
           time git cat-file --batch-check --batch-all-objects
      * Rewrite a range of commits, orphan any that had parents outside range
      * Move entire repo into a subdirectory
    * --content-filter
      * Maybe? Receive object content on stdin, put new object content on stdout
      * Maybe? Receive filename as argv[1], then overwrite (exec'ed repeatedly)
    * Other ideas
      * mode handling?  deleting symlinks?

* future merge pie-in-the-sky ideas
  * break detection
    * depends on:
      * Make break and rename detection safe to use together
  * partial rename -- PICKAXE_BLAME_MOVE
  * copy detection makes no sense (https://public-inbox.org/git/CABPp-BHwM1jx2+VTxt7hga7v-E6gvHuxVNPqm-MPRXYe5CDVtA@mail.gmail.com/)
  * better handling for D/F in recursive merge (is it even possible?)
    * https://public-inbox.org/git/AANLkTimwUQafGDrjxWrfU9uY1uKoFLJhxYs=vssOPqdf@mail.gmail.com/

* Modify git revert to print "This reverts commit 247db0a965bb38b (ls-files:
  Do stupid stuff 2008-07-06)" instead of "This reverts commit
  247db0a965bb38b5b8b96ba51c41302601101e3d" (or maybe still use full sha?)

* old stuff I haven't touched in a while
  * allow importing empty tree with fast-import (for tarball import)
  * source-in-log-pretty-output

* smarter binary storage
  * blobtree -- with subtypes and versions
    * one choice: rsync-like or casync-like storage
    * another: git storage of archive-files (.jar, .tar, .zip, etc.) unarchived
