* Merge:
  * see general-merge.md and other files it links

* Miscellaneous performance:
  * fast-import O(N^2) behavior: tree_content_set() walks every entry
    in current tree to see if the path being inserted should overwrite
    an existing one; if not, appends (unsorted) to the end.  Could make use
    of strmaps to make this much faster.
  * fast-export O(N^2) behavior: see FIXME..string_list_remove()

* Leftover bits:
  * Modernize testcases for rebase -{i,m} with strategy options
    * https://public-inbox.org/git/CABPp-BF_HDW-ie1ZaTrnniWuE7FxThiukk0xJ7P5rfUiwXBGug@mail.gmail.com/
  * Make merge.renames/diff.renames/-Xfind-renames affect diffstat at end
    * https://public-inbox.org/git/CABPp-BGGtLGKVGY_ry8=sdi=s=EDphzTADt+0UR1F4NJpSqmFw@mail.gmail.com/
  * Document special value of 0 for diff.renameLimit
    * https://public-inbox.org/git/20180426162339.db6b4855fedb5e5244ba7dd1@google.com/
  * Anything on the 'misc' or 'wip' branches

* Make git diff throw warning messages on various range operators
  (A..B, merge_commit^!, --first-parent, --pretty=fuller)
  * https://public-inbox.org/git/CABPp-BGg_iSx3QMc-J4Fov97v9NnAtfxZGMrm3WfrGugOThjmA@mail.gmail.com/
  * https://public-inbox.org/git/CABPp-BECj___HneAYviE3SB=wU6OTcBi3S=+Un1sP6L4WJ7agA@mail.gmail.com/
  * https://public-inbox.org/git/CABPp-BE1fQs99ipi9Y8gfQO3QHkxzQhn1uriEbj6YjdYH839eQ@mail.gmail.com/
  * https://public-inbox.org/git/xmqqmumy6mxe.fsf@gitster-ct.c.googlers.com/
  * https://lore.kernel.org/git/xmqqtv5q66dd.fsf@gitster-ct.c.googlers.com/

* Avoid checkout during in-progress commands
  * https://public-inbox.org/git/CABPp-BHramOjqpH0Rz-PEKbi0TX_sKOYvLiZ2Pb=hEpViaShmw@mail.gmail.com/

* flag/command-name consistency
  * sequencer.c:create_seq_dir():
      error(_("a cherry-pick or revert is already in progress"));
      advise(_("try \"git cherry-pick (--continue | --quit | --abort)\""));
    Advising to 'cherry-pick --continue' for a revert??  Which message is wrong?
  * When rebase [--am] halts, it shouldn't show 'git am --show-current-patch'
    as hint; it should show 'git rebase --show-current-patch'.

* rebase consistency
  * Implement missing flags
    * Fix bugs in -Xignore-space-change (and apply's --ignore-whitespace?)
    * Implement --whitespace
    * Implement -C4 (accept it and do nothing because it makes no sense)
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

* Make break and rename detection safe to use together
  * https://crbug.com/git/15
  * https://public-inbox.org/git/xmqqegqaahnh.fsf@gitster.dls.corp.google.com/
  * https://public-inbox.org/git/BANLkTimkm4SU8nML_6Q7Q34faBziJ=gheA@mail.gmail.com/

* Palantir-related stuff
  * snowflake report related
    * multi-branch git-cherry (avoid repeatedly calling git-cherry)
      * ..or just call git-patch-id instead of git-cherry and store results?
    * open source snowflake report
    * rewrite snowflake report in C?
  * cherry-pick or rebase to un-checked-out branch (needs bare-repo merge)

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
