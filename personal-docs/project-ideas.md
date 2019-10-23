* Leftover bits:
  * Modernize testcases for rebase -{i,m} with strategy options
    * https://public-inbox.org/git/CABPp-BF_HDW-ie1ZaTrnniWuE7FxThiukk0xJ7P5rfUiwXBGug@mail.gmail.com/
  * Make merge.renames/diff.renames/-Xfind-renames affect diffstat at end
    * https://public-inbox.org/git/CABPp-BGGtLGKVGY_ry8=sdi=s=EDphzTADt+0UR1F4NJpSqmFw@mail.gmail.com/
  * Document special value of 0 for diff.renameLimit
    * https://public-inbox.org/git/20180426162339.db6b4855fedb5e5244ba7dd1@google.com/
  * Get rid of splits: {diff,merge}_detect_rename, {diff,merge}_rename_limit
  * Anything on the 'misc' or 'wip' branches

* Merge issues to address
  * Separate ideas listed below:
    * perf/correctness -- ort merge stratgy
    * Maximize Accumulated Similarities (-Xmas)
    * diff3 merging improvements (see below)
    * remerge-diff stuff
  * Individual issues
    * Ambiguous renames
      * See https://public-inbox.org/git/CABPp-BF4yjZrD7-bXLa9tHgEws2zvN2zJxqnMqgtY6zm2Lvk3A@mail.gmail.com/
      * Idea: use ort to avoid doing rename detection when one side didn't change
      * Idea (cont): if both sides changed, warn/conflict on ambiguous renames

* diff3 merging improvements
  * Docs on diff3 merging style:
    * https://stackoverflow.com/questions/1203725/three-way-merge-algorithms-for-text (Provided good links, including some of those below)
    * https://en.wikipedia.org/wiki/Merge_(version_control)  (alternatives-weave)
    * https://blog.jcoglan.com (really good explanations of diff3)
      * https://blog.jcoglan.com/2017/05/08/merging-with-diff3/
      * https://blog.jcoglan.com/2017/06/19/why-merges-fail-and-what-can-be-done-about-it/
      * https://blog.jcoglan.com/2017/09/19/the-patience-diff-algorithm/
      * https://blog.jcoglan.com/2017/09/28/implementing-patience-diff/
    * http://www.guiffy.com/SureMergeWP.html (Awesome testcases)
    * http://www.cis.upenn.edu/~bcpierce/papers/diff3-short.pdf (academic paper)
    * https://stackoverflow.com/questions/32365271/whats-the-difference-between-git-diff-patience-and-git-diff-histogram

    * http://www.cse.chalmers.se/~vassena/publications_files/msc-thesis.pdf
    * https://pdfs.semanticscholar.org/71e7/87ca774ffb12d01e996e8b1ebd022bbd45e9.pdf
    * https://www.microsoft.com/en-us/research/wp-content/uploads/2015/02/paper-full.pdf

  * New diff3 alternative:
    * Biggest failures seem to be with "swapping" locations, so...
    * Only works with histogram diff, for multiple reasons (see below)
    * line appears n times in ours or theirs => appears n times in result
      * appearing in a conflict block counts as 0.5 appearances
      * Example: unique text moved elsewhere, target modified on other side
        * illegal to show unconflicted in one location and conflicted elsewhere
        * must show BOTH locations as conflicted
    * Also keep track of where pairs appear,
      * add "see hunk near line 68" or "see hunks near lines 68, 87", e.g.
        "<<<<<< master, see hunk near line 392" (line 187)
        ">>>>>>develop, see hunk near line 187" (line 392)
      * plural form used if multiple unique separated lines moved together
    * Avoid matching internally on high frequency lines
      * Remove whitespace from lines before counting frequency
      * Would avoid matching on: whitespace lines, opening braces, closing braces
      * Reason to avoid:
        * If one side adds to a function the lines
	     A
	     B
          and the other at the same location adds the lines
	     B
	     C
	  the diff3 stuff views it as likely that both added B, then they went
	  and made further edits, thus act like we add line B to the base version
	  and don't conflict but just give the result
	     A
	     B
	     C
	  This is great if B is a low-frequency line, but bad if B is e.g.
	  whitespace.

  * Idea: git merge --cautious
    * Compare different algorithms (and report if they differ):
      * myers
      * patience
      * minimal
      * histogram
      * ignore-whitespace/ignore-space-change
      * MAS (maximally accumulated similarities; see below)
    * Most interesting when some succeed and others fail
    * Also interesting if the unconflicted portions do not match

  * Idea: git log --supercheck
    * Does much the same thing but ALSO:
      * checks if user resolution differs from these
      * also looks at cherry-picks and reverts, not just merges
        * testcase?: https://grsecurity.net/teardown_of_a_failed_linux_lts_spectre_fix.php

* Make git diff throw warning messages on various range operators
  (A..B, merge_commit^!, --first-parent, --pretty=fuller)
  * https://public-inbox.org/git/CABPp-BGg_iSx3QMc-J4Fov97v9NnAtfxZGMrm3WfrGugOThjmA@mail.gmail.com/
  * https://public-inbox.org/git/CABPp-BECj___HneAYviE3SB=wU6OTcBi3S=+Un1sP6L4WJ7agA@mail.gmail.com/
  * https://public-inbox.org/git/CABPp-BE1fQs99ipi9Y8gfQO3QHkxzQhn1uriEbj6YjdYH839eQ@mail.gmail.com/

* Avoid checkout during in-progress commands
  * https://public-inbox.org/git/CABPp-BHramOjqpH0Rz-PEKbi0TX_sKOYvLiZ2Pb=hEpViaShmw@mail.gmail.com/

* Recreate mass merge comparison script
  * Partially described at https://public-inbox.org/git/CABPp-BG632JPG0CMfjgDu6kLx-QK9o0B5D-Xpp-0hDCN2X9X=A@mail.gmail.com/

* Merge Performance
  * Old stuff
    * When we find exact match, don't recompare looking for something better
      (Note: helps when file unmodified on renamed side.)
    * (Minor fixes like avoiding unnecessary string list lookups)
    * Avoid rename detection when unmodified on one side
      (Note: helps when file unmodified on unrenamed side.)
    * filter rename_src list when possible
  * avoid O(N^2) rename detection, if file basename is good enough hint
    * have to avoid breaking directory rename detection
  * avoid O(N^2) rename detection: Use Bloom Filters, MinHash, LSH, or SimHash
    See https://public-inbox.org/git/nycvar.QRO.7.76.6.1901211635080.41@tvgsbejvaqbjf.bet/
        https://en.wikipedia.org/wiki/MinHash
        https://mccormickml.com/2015/06/12/minhash-tutorial-with-python-code/
        etc.
  * avoid O(N^2) index removals part 1: let unpack_trees do more trivial cases
  * avoid O(N^2) index removals part 2: smarter index removal handling
  * <Do Profiling work to find out if there are other slow bits>
  * avoid unnecessary work: trivial tree merges (see old and new notes below)
  * avoid O(N), part 1: don't load or store index in bare-tree merge
  * avoid O(N), part 2: take advantage of in-memory partial index -- rebase
  * avoid O(N), part 3: store and use partial indexes (proof of concept only)

* Old notes on avoiding breaking directory renames despite optimizations:
  * In diffcore_rename:  Per side:
    * List of "skippable paths"
    * List of "exception directories" (directories that disappeared on one side,
      but only e.g. a/ not both a/ and a/b/)
    * Do 100% rename check
    * If any 100% renames begin with exception directory, remove exception
      directory
    * Go through list of skippable paths, remove those starting with except.
      dir.'s
    * Remove remaining skippable paths from consideration on base side
    * Filter list so we only have to iterate over relevant paths

    * For each path in base:
      * Add basename->fullname mapping
    * For each path in dst:
      * If basename in basename->fullname mapping, check similiarity
      * If similar, record rename mapping base->dst
    * Filter list again, so we only have to iterate over relevant paths

    * Do normal rename checks

* trivial tree merges; what does it miss? (old notes)
  * Conflicts I once thought that could go undetected:
    * rename/delete       [I think I was crazy.  This doesn't seem possible.]
    * rename/rename(1to2) [Also not possible.]
  * Conflicts that wouldn't be conflicts:
    * Simple rename appears as modify/delete
      [No, simple rename looks like delete & add; can't get modify/delete
       without a modify, but tree hash is same]
      (If we hit modify/delete conflicts, then note we need to look more
       closely?)

  * Other weirdness in resolution
    * Directory renames may go undetected and leave files in old paths
      (If directory disappears on either side, still need to descend, at least
       for 100% renames, for directory rename detection to work.  But we also
       need to descend into target directories, which may already exist on both
       sides...)

       [Example:
        O: a/
           a/b/{x_1,y_1,z_1}

        A: a/foo
           a/b/{x_1,y_1,z_1}

        B: c/
           c/b/{x_1,y_1,z_1}

       In this example, trivial merge says drop a/b (O matches A) and add c/b
       (O matches A).  No renames are detected.  As such, a/foo won't get
       renamed to c/foo.]

* Trivial tree merges (new notes)
  * Types of rename issues that can come up:
    * normal rename (content merge)
    * rename/delete
    * rename/rename(1to2)[/add/add]
    * rename/rename(2to1)[/delete/delete]
    * rename/add
    * rename/add/delete
    * directory rename stuff

  * If at tree level, base=merge != ours, or base=ours != merge, then:
    * Can only have rename from base to the modified side of history.  A
      file present with the same name will prevent rename detection on
      the unmodified side of history.  (Even if break detection were
      turned on, which we don't do in the merge machinery, the file
      would be 100% similar and thus we would not break the pairing and
      not get a rename.)

    * May have normal rename, but no need for content merge.  If rename
      not detected, it looks like one-sided add, which matches exactly
      what we'd get from doing a content merge anyway (since one side
      was unmodified).

    * rename/delete is impossible, because base=merge or base=ours
      implies that the source file renamed on the modified side could
      not have been deleted on the unmodified side.

    * rename/rename(1to2) impossible, because unmodified side implies
      that the source file was not renamed on the unmodified side of
      history.

    * rename/rename(2to1) impossible, because unmodified side implies
      that the target file could not have gotten there from a rename on
      the unmodified side of history.

    * rename/add is possible, but if rename not detected it looks like
      an add/add conflict; since the renamed file was unmodified on the
      other side of history, no content merge is needed and thus
      treating the rename/add as an add/add gives the same result.

    * rename/add/delete impossible since rename/delete is impposible.

    * directory rename is totally possible.  However, the point of
      directory rename detection is to send "new" files added on the
      unrenamed side of history to the same directory that the renamed
      side sent them to.  Since one side was unmodified, that means that
      not only is there no renames on that side (and that there are not
      content changes to any files the other side may have renamed), but
      that there are no new files to send anywhere either.

       base: directories A/, B/, and C/
       ours: directory B/ -> A/, so A/ is bigger.
       theirs: directory C/ -> B/, so B is bigger.

      With directory rename detection:
        * detect all the C/ -> B/ renames on theirs.  Full directory, so note
          that there is a directory rename.  Doesn't matter since there are no
          new files in C on ours to pull along with the rename.
        * detect all the B/ -> A/ renames.  Entire directory moves, so note
          that there is a directory rename.  Since theirs added a lot of files
          to B/ (via renames) there is need for a transitive rename to move
          those to A.
      With optimizations:
        * C is unmodified on one side and deleted elsewhere, so just delete C.
          No C->B rename detected.
        * detect all the B/ -> A/ renames (theirs modified B so the unmodified
          on one side optimizations don't apply).  Entire directory moves, so
        note that there is a directory rename.  Since theirs adds many new
        files to B (technically they were renames but we didn't detect them
        and think of them as new files), the directory rename causes these
        to need to move to A.
      Either way, the result is the same.

      The optimization case could only be problematic if the "new files"
      in B had content changes to merge from the other side.  But if the
      "new files" had content changes to merge across the rename, then
      the directory would not have been unmodified on one side.

* flag/command-name consistency
  * sequencer.c:create_seq_dir():
      error(_("a cherry-pick or revert is already in progress"));
      advise(_("try \"git cherry-pick (--continue | --quit | --abort)\""));
    Advising to 'cherry-pick --continue' for a revert??  Which message is wrong?
  * When rebase [--am] halts, it shouldn't show 'git am --show-current-patch'
    as hint; it should show 'git rebase --show-current-patch'.

* rebase consistency
  * Add rebase --empty={drop,halt,keep}
  * Remove (as much as possible) call to `rev-list --cherry-pick ...`
    * See https://public-inbox.org/git/nycvar.QRO.7.76.6.1901211635080.41@tvgsbejvaqbjf.bet/
  * Implement missing flags
    * Fix bugs in -Xignore-space-change (and apply's --ignore-whitespace?)
    * Implement --whitespace
    * Implement -C4 (accept it and do nothing because it makes no sense)
  * Add --am flag
  * Change default to interactive machinery
  * Make rebase rewrite commit messages that refer to old commits that were
    also rebased; rationale:
    - Leaving stale hashes around is rather unhelpful.  While branches that
      still need to be rebased won't often need to refer to earlier commits
      in the branch, sometimes we do.  The problems with avoiding hashes and
      instead using references like "two commits ago" are:
        (1) if I re-order commits during the rebase then the commit
            message still needs to be reworded, and instead of correcting
            a hash (which could have been automated) I now need to correct
            how many commits there are separating the relevant two commits
        (1b) further, if someone forgets to update the commit message, the
             stale reference is much less likely to be noted as such, which
             then will not only fail to help future readers it could actively
             confuse them.
        (2) even if the count of commits is correct, the count can still
            confuse others looking through git log later, since git log
            will intersperse commits from different branches that were
            merged based on committer timestamp (--topo-order isn't the
            default).

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

  * Miscellaneous ort stuff
    * default to histogram
    * allow running with older defaults (e.g. myers) for comparison

* remerge-diff
  * depends on:
    * merge-rewrite
    * super-quiet merge output
  * affects: merge commits, revert commits, and cherry-pick'ed commits
  * Possible good example testcase: https://grsecurity.net/teardown_of_a_failed_linux_lts_spectre_fix.php
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
    * other enhancements to virtual merge base:
      * if both sides update to the same blob id somewhere, update to that
        version (especially with no common ancestry; see https://public-inbox.org/git/DB8PR02MB5531685CAC9ABF2D5B01B436E06D0@DB8PR02MB5531.eurprd02.prod.outlook.com)
      * note that this comparison is typically done for a given filename, but
        would need to take care in the presence of renames
  * Challenges:
    * renames may mean that patch hunks are against different filename
  * Improved merges:
    * fix cherry-picked, then reverted in one branch, then branches merged
    * commit cherry-picked, then additional changes on same lines on one side

* deprecate and error on `git diff A..B`:
  * https://public-inbox.org/git/xmqqmumy6mxe.fsf@gitster-ct.c.googlers.com/

* fix gc wonkiness
  * gc fails for folks for lack of disk space.  Sometimes running prune
    _first_ works just fine.  (One user report of `git prune` cleaning out
    30 gb...)
  * gc can be exceptionally slow.  Try
      git filter-repo --replace-text <(echo driver==>pilot)
    in linux.git.  Because there are so many unreferenced blobs afterwards
    that all get unpacked before the prune step, the gc slows to a crawl.
  * gc hits the too-many-loose-objects repeatedly
    https://public-inbox.org/git/20180716203539.GD25189@sigill.intra.peff.net/
    "Loose objects and unreachable objects" in hash-function-transition.txt

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
