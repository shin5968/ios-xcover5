#!/bin/bash
# Count/print strings in the 16.7.16 shared cache matching a regex (needs mntsys.sh first).
#   bash tools/xcover5/dev/dsgrep.sh REGEX [max]
D=$HOME/xc5/sysos/root/System/Library/Caches/com.apple.dyld
strings -n 6 "$D"/dyld_shared_cache_arm64* 2>/dev/null | grep -E "$1" | sort | uniq -c | sort -rn | head -"${2:-30}"
