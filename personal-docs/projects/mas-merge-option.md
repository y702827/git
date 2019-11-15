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

