# gpu/mesa notice

`herodrm.c`, `build-mesa.sh`, `patch-mesa.py`, `hero_hw.c`, `osmesa_hero.inc` and `osmesa-test.c`
come from wardevsa/ios-a5xelte, branch `herolte`, `gpu/mesa/` (the Galaxy S7 port, the same
SWEET/A5XELTE kernel family as this tree). Changes for the XCover5, each marked in the source:

- `herodrm.c`: the IOKit service is `XC5GPU` (tools/xcover5/XC5GPU.cpp, same selector numbers as
  HeroGPU) and the version string names the Mali-G52.
- `osmesa-test.c`: its stderr file is under `/private/var/xc5/`.
- `build-mesa.sh`: rpath `/private/var/xc5/lib`, and `libxc5test.dylib` (`xc5test.c`, ours, MIT):
  the same test run from a thread inside backboardd.

Mesa itself is fetched at build time (`MESA_TAG`, default mesa-25.0.7) under its own licenses.
Nothing of Apple's is in this directory or in the build output.
