#!/bin/bash

set -x

scale_test() {
    n=$1
    shift

    git init -q scale/$n
    cd scale/$n
    ../../write_export_file.py $n | git fast-import --quiet
    git checkout -q A^0
    (time git merge B^0) 2>&1 >/dev/null | grep real >>/tmp/timing
    cd ../..
}

rm /tmp/timing
rm -rf scale/

scale_test 1
scale_test 10
scale_test 100
scale_test 1000
cat /tmp/timing
