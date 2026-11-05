# gpu/ notice

`eagl/` and `hakios-gl/` come from wardevsa/ios-a5xelte, branch `herolte` (the Galaxy S7 port), with
their own NOTICE files kept: `eagl/` is Bergamota's EAGL shim (ry-diffusion/Bergamota, MIT) with the S7
port's changes, `hakios-gl/` is HakiOS's EAGL bridge (MIT, `hakios-gl/LICENSE.hakios`) with the S7
port's `gl_hero.c`. XCover5 changes, marked in the source: `eagl/src/hero_log.c` reads
`/private/var/xc5/gl.env`, `hakios-gl/src/gl_hero.c` names the Mali-G52, and `eagl/build-eagl.sh` builds
only the two bridges as `libxc5eagl.dylib` (HakiOS) and `libxc5eagl-b.dylib` (Bergamota) with rpath
`/private/var/xc5/lib`. `mesa/` has its own NOTICE.md.
