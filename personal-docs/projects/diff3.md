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

