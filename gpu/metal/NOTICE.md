# gpu/metal notice

- `air/` is `kernel/tools/mesa/air/` of [stormtroopxyz/y2sios](https://github.com/stormtroopxyz/y2sios)
  (MIT, `y2sios-LICENSE`, copyright the y2sios / Darwin on redwood contributors): the AIR frontend that reads
  Apple's Metal shader bitcode into NIR, and `air_metal.c`, the `rwa_*` library a Metal driver compiles and
  dispatches through.
- `a32-air-integer-ubo.patch` is [Luminous418/hakios-a32](https://github.com/Luminous418/hakios-a32)'s
  `tools/mesa/air-integer-ubo.patch` (MIT); it is already applied to `air/air_metal.c`.
- `y2sios-air-frontend.patch`, `y2sios-air-panfrost.patch` are y2sios's build patches, kept for reference;
  `gpu/mesa/build-mesa.sh` does the same with our libdrm stand-in instead of y2sios's `redwood_drm.c`.

Plan: `docs/xcover5-metal-plan.md`.
