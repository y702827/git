* Ideas put in separate file
  * ort merge stratgy (also covers performance)
  * Maximize Accumulated Similarities (-Xmas)
  * diff3 merging improvements (see below)
  * remerge-diff stuff

* Add testcases for conflict types we don't handle well
  * rename issues (??)
    * (suspected problems) rename/rename(1to2) with multiple D/F conflicts
    * chain r/r types not just to each other but to adds/deletes as well
      (e.g. del/ren/ren(2to1) + ren/ren1to2 + ren/ren2to1 + ren/ren(1to2)/add)
  * issues with submodules
    * clean deletion -- does it actually delete?  (There is code suggesting not)

* Recreate mass merge comparison script
  * Partially described at https://public-inbox.org/git/CABPp-BG632JPG0CMfjgDu6kLx-QK9o0B5D-Xpp-0hDCN2X9X=A@mail.gmail.com/

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

* avoid splitting conflict information feedback in merge
  * "CONFLICT(rename/add)" and "CONFLICT(content)" for the same path can
    be separated by many unreleated intermediate output messages
  * This comes from a combination of printing rename/add conflict as soon as
    we notice, but deferring working copy update and thus the merge_content
    call until much later
  * https://public-inbox.org/git/CABPp-BGJiS96_wXTp4dpVG4CpTEt--KGELykofimP-Wh4nFhdg@mail.gmail.com/

* Ambiguous renames
  * See https://public-inbox.org/git/CABPp-BF4yjZrD7-bXLa9tHgEws2zvN2zJxqnMqgtY6zm2Lvk3A@mail.gmail.com/
  * Idea: use ort to avoid doing rename detection when one side didn't change
  * Idea (cont): if both sides changed, warn/conflict on ambiguous renames

* future merge pie-in-the-sky ideas
  * break detection
    * depends on:
      * Make break and rename detection safe to use together
  * partial rename -- PICKAXE_BLAME_MOVE
  * copy detection makes no sense (https://public-inbox.org/git/CABPp-BHwM1jx2+VTxt7hga7v-E6gvHuxVNPqm-MPRXYe5CDVtA@mail.gmail.com/)
  * better handling for D/F in recursive merge (is it even possible?)
    * https://public-inbox.org/git/AANLkTimwUQafGDrjxWrfU9uY1uKoFLJhxYs=vssOPqdf@mail.gmail.com/
