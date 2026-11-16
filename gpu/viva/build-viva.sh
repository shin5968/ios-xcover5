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
