* sparse checkout
  * Phantom "not uptodate. Cannot update sparse checkout":
    (stat-dirty?)
    $ git read-tree -mu HEAD
    error: Entry 'packages/pem-ui/package.json' not uptodate. Cannot update sparse checkout.
    $ git status
    On branch sparse-checkout
    Your branch is ahead of 'origin/develop' by 2 commits.
      (use "git push" to publish your local commits)

    nothing to commit, working tree clean
    $ git reset --hard
    HEAD is now at af5d3146842 WIP cleanups
    $ git read-tree -mu HEAD
  * reset --hard fails
    (stat-dirty?  marked as sparse, but entry exists in working dir?)
    $ git status
    On branch sparse-checkout
    Your branch is ahead of 'origin/develop' by 2 commits.
      (use "git push" to publish your local commits)

    nothing to commit, working tree clean
    $ git reset --hard HEAD
    error: Sparse checkout leaves no entry on working directory
    fatal: Could not reset index file to revision 'HEAD'.
    $ git reset --hard
    error: Sparse checkout leaves no entry on working directory
    fatal: Could not reset index file to revision 'HEAD'.

  * How to make a reset --hard fail:
    $ git merge origin/develop  (clean merge, adds some working tree files)
    $ git reset --hard HEAD~1
    error: Entry 'modules/foo/file-outside-sparse-paths.c' not uptodate. Cannot update sparse checkout.
    fatal: Could not reset index file to revision 'HEAD~1'.


* submit and fix ./t7107-reset-perf-avoid-rewrite.sh
  (when file 100% renamed, reset --hard could unrename instead of deleting
   new filename and then re-writing old filename.  Could matter if file was
   really big, but not sure it's really worth it...)

* really old & not re-verified:

  * git pull --rebase prints two shas, but not sha of new branch tip -- Mark
    was confused about why.  (He didn't notice rebase occurred after the
    fetch...)
