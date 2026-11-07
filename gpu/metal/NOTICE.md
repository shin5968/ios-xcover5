# gpu/metal notice

- `air/` is `kernel/tools/mesa/air/` of [stormtroopxyz/y2sios](https://github.com/stormtroopxyz/y2sios)
  (MIT, `y2sios-LICENSE`, copyright the y2sios / Darwin on redwood contributors): the AIR frontend that reads
  Apple's Metal shader bitcode into NIR, and `air_metal.c`, the `rwa_*` library a Metal driver compiles and
  dispatches through.
- `a32-air-integer-ubo.patch` is [Luminous418/hakios-a32](https://github.com/Luminous418/hakios-a32)'s
  `tools/mesa/air-integer-ubo.patch` (MIT); it is already applied to `air/air_metal.c`.
- `y2sios-air-frontend.patch`, `y2sios-air-panfrost.patch` are y2sios's build patches, kept for reference;
  `gpu/mesa/build-mesa.sh` does the same with our libdrm stand-in instead of y2sios's `redwood_drm.c`.
- `air/air_metal.c` XCover5 change: an IOSurface is imported through herodrm's stand-in descriptor
  (`herodrm_surface_import`, XC5GPU SurfaceBO) instead of the bare surface ID.
- `runtime/` is y2sios's `userland/src/` Metal driver (`redwoodmetal.m`, `redwooddrawable.m`, `redwood_drawable.h`,
  `m2m.h`, `metal_plugin.plist`, `metal_props.c`, `metal_drv_test.m`), `kernel/xnu/files/bsd/sys/redwood_mali.h`
  and `userland/build.sh` (as `y2sios-build.sh`), MIT. XCover5 changes: the dlopen path of libredwoodair
  (`/private/var/xc5/lib`), `m2m.h`'s fallback to XC5GPU's Log selector, and `runtime/xc5inc/IOKit/IOKitLib.h`
  (ours) that maps the driver's GPU requests onto XC5GPU. Built by `build-metal.sh` into `RedwoodMetal.bundle`.

Plan: `docs/xcover5-metal-plan.md`.
