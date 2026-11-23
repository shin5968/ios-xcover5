#!/bin/bash
# Builds viva-gpu's Metal driver (VivaMTLDriver.bundle) for the Galaxy XCover5 on a macOS runner with Xcode.
#
# viva-gpu (https://github.com/Nik300/viva-gpu, GPL-3.0-or-later with an attribution term: keep its NOTICE) is
# fetched at a fixed commit and built as its own CI does (ios/build.sh mesa + driver), with one change: the vgpu
# back-end is gpu/viva/vgpu_xc5.c, which speaks the XCover5 kernel's XC5GPU user client instead of viva's own
# VivaGPU kernel driver. The Valhall ISA compiler stays built but is off (VMG_ISA unset): the XCover5's Mali-G52 is
# Bifrost, so shaders go AIR -> NIR -> Panfrost's Bifrost compiler.
#
# Usage: bash gpu/viva/build-viva.sh [workdir]   output: <workdir>/out/VivaMTLDriver.bundle (+ NOTICE, LICENSE)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
W="${1:-$HERE/work}"
VIVA_URL=https://github.com/Nik300/viva-gpu
VIVA_REV=332d26a          # README rewritten for the driver as it runs on the phone (2026-11-16)
mkdir -p "$W"
W="$(cd "$W" && pwd)"
V="$W/viva-gpu"
# (the CI cache restores mesa/ and .venv inside it before the clone: init in place)
mkdir -p "$V"
if [ ! -d "$V/.git" ]; then
  git -C "$V" init -q && git -C "$V" remote add origin "$VIVA_URL"
fi
git -C "$V" fetch -q origin
git -C "$V" checkout -q -f "$VIVA_REV"
git -C "$V" clean -q -fd -e mesa/src -e mesa/build-macos -e mesa/build-ios -e mesa/out -e .venv -e ios/out -e libdrm-xnu/out

# ---- the XCover5 back-end
cp "$HERE/vgpu_xc5.c" "$V/vgpu/vgpu_xc5.c"
python3 - "$V" <<'PY'
import sys, re, pathlib
v = pathlib.Path(sys.argv[1])
h = v / 'vgpu/vgpu.h'
s = h.read_text()
if 'vgpu_backend_xc5' not in s:
    s = s.replace('extern const struct vgpu_backend vgpu_backend_mock;',
                  'extern const struct vgpu_backend vgpu_backend_mock;\nextern const struct vgpu_backend vgpu_backend_xc5;   // ios-xcover5: XC5GPU')
    h.write_text(s)
c = v / 'vgpu/vgpu.c'
s = c.read_text()
if 'vgpu_backend_xc5' not in s:
    s = s.replace('        g_backend = &vgpu_backend_iokit;\n#else',
                  '        g_backend = &vgpu_backend_xc5;     // ios-xcover5: the XCover5 kernel (XC5GPU)\n#else', 1)
    s = s.replace('        if (env && !strcmp(env, "mock")) g_backend = &vgpu_backend_mock;',
                  '        if (env && !strcmp(env, "mock")) g_backend = &vgpu_backend_mock;\n'
                  '        if (env && !strcmp(env, "xc5")) g_backend = &vgpu_backend_xc5;', 1)
    assert 'vgpu_backend_xc5' in s, 'vgpu.c: backend selection not found'
    c.write_text(s)
b = v / 'ios/build.sh'
s = b.read_text()
if 'vgpu_xc5.c' not in s:
    s = s.replace('"$GPU/vgpu/vgpu_iokit.c"; do', '"$GPU/vgpu/vgpu_iokit.c" "$GPU/vgpu/vgpu_xc5.c"; do', 1)
    s = s.replace('"$GPU/vgpu/vgpu_iokit.c" \\\n      -framework IOKit', '"$GPU/vgpu/vgpu_iokit.c" "$GPU/vgpu/vgpu_xc5.c" \\\n      -framework IOKit', 1)
    assert s.count('vgpu_xc5.c') >= 1, 'ios/build.sh: vgpu sources not found'
    b.write_text(s)
# Indexed draws: hand Panfrost the CPU copy of the index buffer (has_user_indices) instead of the resource.
# backboardd (QuartzCore) crashed in panfrost_batch_read_rsrc <- panfrost_get_index_buffer_bounded on the
# resource path (SIGSEGV at 0xcca8, viva6 log); the user-index path uploads the indices and never touches it.
e = v / 'driver/vmg_exec.c'
s = e.read_text()
old = '        info.index.resource = d->index_buffer->res;\n'
if 'xc5: user indices' not in s:
    assert old in s, 'vmg_exec.c: index resource line not found'
    s = s.replace(old,
        '        {   // xc5: user indices (CPU copy) unless the GPU writes it or the draw is indirect\n'
        '            vmg_resource *ib = d->index_buffer;\n'
        '            const void *cpu = ib->user_ptr && !ib->cpu_access ? ib->user_ptr : ib->map;\n'
        '            if (!ib->res) return;\n'
        '            if (cpu && !ib->gpu_writes && !d->indirect) { info.has_user_indices = true; info.index.user = cpu; }\n'
        '            else info.index.resource = ib->res;\n'
        '        }\n', 1)
    e.write_text(s)
# viva10: backboardd's fragment jobs fault 0x5a (ADDR_RANGE) with fault_ptr at the sampler table (two valid
# Bifrost sampler descriptors, the rest of the table absent): the shader reads a sampler slot past the count bound.
# Bind all 32 slots (unused ones = the default sampler) so no index is out of range.
old = '    for (unsigned i = 0; i < scount; i++) if (!samplers[i]) samplers[i] = dev->sampler_default;\n'
if 'xc5: sampler pad' not in s:
    assert old in s, 'vmg_exec.c: sampler fill line not found'
    s = s.replace(old, '    if (!getenv("VMG_NO_SAMPLER_PAD")) scount = 32;   // xc5: sampler pad\n' + old, 1)
    e.write_text(s)
# Safari: a 'w30r' surface (4 bytes a pixel, bpr 4544) is copied into an rgba16f texture (8 bytes a pixel); the copy
# used the texture's row (9000 bytes) over the surface's pitch and read/wrote past its end: SIGSEGV in
# WebKit.GPU (util_copy_box <- vmg_texture_sync) and a kernel panic (vm_page_insert past object bounds, size =
# the surface). Skip a copy whose rows do not fit the surface's pitch.
x = v / 'driver/vmg_exec.c'
s = x.read_text()
old = '    uint64_t bytes = (uint64_t)util_format_get_stride(t->res->format, w) * util_format_get_nblocksy(t->res->format, h);\n'
if 'xc5: row fits' not in s:
    assert old in s, 'vmg_exec.c: copy size line not found'
    s = s.replace(old,
        '    if (s && (xc5_is_xr(t) || IOSurfaceGetPixelFormat(s) == 0x77333072)) {   // w30r by the surface too\n'
        '      { xc5_xr_copy(dev, t, base, bpr, to_texture);   // xc5: w30r/BGR10_XR converted\n'
        '        uint32_t u = 0; IOSurfaceUnlock(s, to_texture ? kIOSurfaceLockReadOnly : 0, &u);\n'
        '        if (!to_texture) {} t->surface_seed = to_texture ? lock_seed : u; t->surface_seed_valid = true;\n'
        '        CFRelease(s); return;\n'
        '      }\n'
        '    }\n'
        '    if (util_format_get_stride(t->res->format, w) > bpr) {   // xc5: row fits the pitch, or no copy\n'
        '        if (s) { uint32_t u = 0; IOSurfaceUnlock(s, to_texture ? kIOSurfaceLockReadOnly : 0, &u); CFRelease(s); }\n'
        '        return;\n'
        '    }\n' + old, 1)
    # BGR10_XR(_sRGB) <-> half floats, after mapple_gpu_mali 85954b3 (GPL-3.0+, viva-gpu attribution): blue bits 0-9,
    # green 10-19, red 20-29, each (code - 384) / 510; the sRGB one is extended-sRGB encoded.
    helpers = r'''
#include <math.h>
#include "util/half_float.h"
static bool xc5_is_xr(const vmg_texture *t) { return t->mtl_format == 554 || t->mtl_format == 555; }
static float xc5_srgb(float v, bool decode)
{
    float a = fabsf(v), r = decode ? (a <= 0.04045f ? a / 12.92f : powf((a + 0.055f) / 1.055f, 2.4f))
                                   : (a <= 0.0031308f ? a * 12.92f : 1.055f * powf(a, 1 / 2.4f) - 0.055f);
    return v < 0 ? -r : r;
}
static void xc5_xr_copy(vmg_device *dev, vmg_texture *t, uint8_t *base, size_t bpr, bool to_texture)
{
    uint32_t w = t->res->width0, h = t->res->height0;
    bool srgb = t->mtl_format == 555;
    struct pipe_box box;
    u_box_2d(0, 0, (int)w, (int)h, &box);
    static int said;
    if (said < 12) {   // safari5: page black, text light: what goes each way (first pixel, middle pixel)
        said++;
        uint32_t p0, pm; memcpy(&p0, base, 4); memcpy(&pm, base + (h / 2) * bpr + (w / 2) * 4, 4);
        fprintf(stderr, "vmg: xr %s %ux%u fmt %u %s surface %08x mid %08x\n", to_texture ? "in" : "out", w, h,
                t->mtl_format, util_format_short_name(t->res->format), p0, pm);
    }
    if ((size_t)w * 4 > bpr || util_format_get_blocksize(t->res->format) != 8) return;
    if (to_texture) {
        uint16_t *tmp = malloc((size_t)w * h * 8);
        if (!tmp) return;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t p; memcpy(&p, base + y * bpr + x * 4, 4);
                uint16_t *o = tmp + 4 * ((size_t)y * w + x);
                for (int c = 0; c < 3; c++) {
                    float v = ((float)((p >> (20 - 10 * c)) & 1023) - 384) / 510;
                    o[c] = _mesa_float_to_half(srgb ? xc5_srgb(v, true) : v);
                }
                o[3] = _mesa_float_to_half(1);
            }
        dev->pipe->texture_subdata(dev->pipe, t->res, 0, PIPE_MAP_WRITE, &box, tmp, w * 8, 0);
        free(tmp);
        return;
    }
    struct pipe_transfer *x = NULL;
    uint8_t *m = dev->pipe->texture_map(dev->pipe, t->res, 0, PIPE_MAP_READ, &box, &x);
    if (!m) return;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t i = 0; i < w; i++) {
            uint16_t px[4]; memcpy(px, m + y * x->stride + i * 8, 8);
            uint32_t p = 3u << 30;
            for (int c = 0; c < 3; c++) {
                float v = _mesa_half_to_float(px[c]);
                if (srgb) v = xc5_srgb(v, false);
                p |= (uint32_t)CLAMP(lroundf(v * 510 + 384), 0, 1023) << (20 - 10 * c);
            }
            memcpy(base + y * bpr + i * 4, &p, 4);
        }
    if (said < 24) {
        said++;
        uint16_t hm[4]; memcpy(hm, m + (h / 2) * x->stride + (w / 2) * 8, 8);
        uint32_t pm; memcpy(&pm, base + (h / 2) * bpr + (w / 2) * 4, 4);
        fprintf(stderr, "vmg: xr out mid half %04x %04x %04x %04x -> %08x\n", hm[0], hm[1], hm[2], hm[3], pm);
    }
    dev->pipe->texture_unmap(dev->pipe, x);
}

'''
    s = s.replace('void vmg_texture_sync(vmg_device *dev, vmg_texture *t, bool to_texture)\n{',
                  helpers + 'void vmg_texture_sync(vmg_device *dev, vmg_texture *t, bool to_texture)\n{', 1)
    assert 'xc5_xr_copy(vmg_device' in s
    x.write_text(s)
# safari7: the kernel makes 'w30r' surfaces 'BGRA', but WebKit still asks for a BGR10_XR (554/555) texture over them:
# it was copied and "converted" from bytes that are BGRA8 (black page, torn text). Over a surface, the XR formats
# (550-555) are BGRA8Unorm (80), or its sRGB twin (81), as hakios's redwoodmetal does.
sc = v / 'driver/vmg_screen.c'
s = sc.read_text()
old = ('vmg_texture *vmg_texture_create_iosurface(vmg_device *dev, const vmg_texture_desc *d, uint32_t surface_id, unsigned plane)\n'
       '{\n')
if 'xc5: XR over a surface' not in s:
    assert old in s, 'vmg_screen.c: vmg_texture_create_iosurface not found'
    s = s.replace(old, old +
        '    vmg_texture_desc xc5d;   // xc5: XR over a surface is BGRA8\n'
        '    if (d->mtl_format >= 550 && d->mtl_format <= 555) {\n'
        '        xc5d = *d; xc5d.mtl_format = (d->mtl_format & 1) ? 81 : 80; d = &xc5d;\n'
        '    }\n', 1)
    sc.write_text(s)
print('viva-gpu patched for XC5GPU')
PY

# ---- build (as viva-gpu's .github/workflows/build.yml: Mesa for macOS gives the host tools, then iOS)
cd "$V"
bash mesa/build.sh macos-nosmoke
bash ios/build.sh mesa
bash ios/build.sh driver
OUT="$W/out"
rm -rf "$OUT"; mkdir -p "$OUT"
cp -R ios/out/VivaMTLDriver.bundle "$OUT/"
cp NOTICE LICENSE "$OUT/"
echo "$VIVA_URL @ $(git rev-parse HEAD) + gpu/viva/vgpu_xc5.c (ios-xcover5)" > "$OUT/SOURCE.txt"
ls -la "$OUT" "$OUT/VivaMTLDriver.bundle"
