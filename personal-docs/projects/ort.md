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
  * avoid repeated O(N^2) rename detection
    * during cherry-pick of series or a rebase, when we detect that foo->bar
      on HEAD side and pick side modifies foo, record somewhere that
      HEAD:foo -> (3-way-merged):bar is a rename, so when we go to do the
      next pick we don't have to re-detect the foo->bar rename.
    * Note: any further changes to bar other than a delete/rename shouldn't
      break the rename pairing either; modifications of files whose names
      don't change never lead to an assumption that the file is unrelated to
      a file with the same name from long ago no matter the number of
      intervening commits (because we don't do break detection).
    * similar, if old/foo->new/foo on HEAD side and pick does NOT modify
      old/foo, we still (except for trivial tree merge cases) need to detect
      the rename.  But we could record it after the first pick so that we
      don't need to re-detect it for subsequent commits.
    * Note: If foo->bar on pick side, then the next pick will have all three
      versions of the file named 'bar' (unless the next pick renames it again),
      so there is no need to store any rename info.
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

