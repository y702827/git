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

  * Idea: git log --supercheck (alternate name for remerge-diff?)
    * Does much the same thing but ALSO:
      * checks if user resolution differs from these
      * also looks at cherry-picks and reverts, not just merges
        * testcase?: https://grsecurity.net/teardown_of_a_failed_linux_lts_spectre_fix.php

