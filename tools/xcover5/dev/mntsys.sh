#!/bin/bash
# Mount the iOS 16.7.16 (20H392, iPhone X) cryptexes read-only with apfs-fuse, no root:
#   ~/xc5/sysos  <- ~/xc5/ipsw/SystemOS.dmg  (the dyld shared cache: System/Library/Caches/com.apple.dyld)
#   ~/xc5/osroot <- ~/xc5/ipsw/OS.dmg        (the root: apps, xpc/extensions.plist, ...)
# The IPSW pieces are the user's, under WSL ~/xc5/ipsw, and never go into git.
for pair in sysos:SystemOS.dmg osroot:OS.dmg; do
  M=$HOME/xc5/${pair%%:*}; D=$HOME/xc5/ipsw/${pair#*:}
  mkdir -p "$M"
  mountpoint -q "$M" || $HOME/apfs-fuse/build/apfs-fuse "$D" "$M" 2>&1 | tail -2
done
ls $HOME/xc5/sysos/root/System/Library/Caches/com.apple.dyld | head -2
