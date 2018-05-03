* submit and fix ./t7107-reset-perf-avoid-rewrite.sh
  (when file 100% renamed, reset --hard could unrename instead of deleting
   new filename and then re-writing old filename.  Could matter if file was
   really big, but not sure it's really worth it...)

* really old & not re-verified:

  * git pull --rebase prints two shas, but not sha of new branch tip -- Mark
    was confused about why.  (He didn't notice rebase occurred after the
    fetch...)
