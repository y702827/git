#!/usr/bin/env python

import os
import sys

mark=1
def write_mark():
  global mark
  print("mark :{}".format(mark))
  mark+=1

def write_file(n):
  global mark
  branches = ["refs/heads/master", "refs/heads/A", "refs/heads/B"]
  parent = None
  for b in range(0, 3):
    for i in range(0, n):
      print("blob")
      write_mark()
      middle = 5 if b != 1 else 5.5
      final = 10 if b != 2 else 10.5
      content="{}\n2\n3\n4\n{}\n6\n7\n8\n9\n{}\n".format(i, middle, final)
      print("data {}".format(len(content)))
      print(content)
    extras = 1000000 if (b==0) else 0
    for i in range(0, extras):
      print("blob")
      write_mark()
      content="{}\n".format(i)
      print("data {}\n{}".format(len(content), content))
    print("reset {}".format(branches[b]))
    print("commit {}".format(branches[b]))
    write_mark()
    print("author Who Cares <i@mean.really> 123456789 -0700")
    print("committer It Just <does@not.matter> 123456789 -0700")
    commit_msg = os.path.basename(branches[b])
    print("data {}\n{}".format(len(commit_msg)+1, commit_msg))
    if b == 0:
      parent = mark-1
    else:
      print("from :{}".format(parent))
    total = n+extras
    for i in range(0, n):
      print("M 100644 :{} {}/{}/{}".format(mark-1-total+i, i/100%100,i%100,i))
    for i in range(0, extras):
      print("M 100644 :{} unchanged/{}/{}/{}".format(mark-1-extras+i, i/100%100,i%100,i))
    print("")

write_file(int(sys.argv[1]))
