#!/bin/bash
# Regenerate xnu/patches/0125-xcover5.patch from tools/xcover5/apply-xcover5.py.
# Needs the base xnu tree at ../src/xnu-s8 (commit 91787b5, the ios-galaxy-s8 base) next to ios-xcover5.
# Run from WSL:  bash tools/xcover5/dev/regen.sh
set -e
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO/../src/xnu-s8"
git reset -q --hard 91787b5
git clean -qfd
python3 "$REPO/tools/xcover5/apply-xcover5.py" . | grep -c edited
# New files (XC5GPU.cpp, XC5AMFI.cpp, xc5_drop.c) must show up in the diff.
git add -N .
git -c core.autocrlf=false diff > "$REPO/xnu/patches/0125-xcover5.patch"
git diff --stat | tail -2
