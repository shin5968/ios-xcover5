#!/usr/bin/env python3
"""Build the Galaxy XCover5 (SM-G525F/N, Exynos 850 "s5e3830") ADT.

Same schema as tools/mkadt_dreamlte.py; the board facts are the XCover5's,
read from the stock DT (G525NKXSFEYL1 boot.img dtb + dtbo, docs/xcover5-port.md):
  - XNU RAM: 512 MiB of the high bank at 0x880000000 (the bank ends at
    0x8ffffffff; mfc_fw_rmem sits at 0x8a2400000). 512 MiB is the size the
    A5/S8 4 KiB kernel layout is proven with.
  - GIC-400: GICD 0x12a01000, GICC 0x12a02000; DT arch timer is
    "use-physical-timer": non-secure physical timer PPI 14 = INTID 30
  - timer 26 MHz (clock-frequency = <0x18cba80>)
  - CPUs: 8 Cortex-A55 (MT bit set), MPIDR Aff1 = core, Aff2 = cluster.
    XNU's SWEET path keys CPUs by MPIDR[15:0], so only the first cluster
    (0x000-0x300) is listed; cpus=N selects a prefix.
  - S-Boot framebuffer 720x1480 XRGB8888 at 0xfa000000 (fb_handover)
  - eMMC: DesignWare MSHC dwmmc0 at 0x12100000 (androidboot.boot_devices)
The identity stays the donor's iPhone X (D22AP).
"""
import argparse
import os
from pathlib import Path

import adt
import mkadt

DRAM_BASE = 0x880000000
DRAM_SIZE = 0x20000000
FB_PHYS, FB_W, FB_H = 0xfa000000, 720, 1480
EMMC_PHYS = 0x12100000
TIMEBASE = 26000000
NCPUS = 4


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


def cpu_node(logical):
    return [
        ('device_type', mkadt.cstr('cpu')),
        # SweetCPU's personality IONameMatches 'arm,cortex-a53' only; without it
        # no CPU registers and XNU waits forever after AppleCLCD (boot 2).
        ('compatible', mkadt.cstr('arm,cortex-a55') + mkadt.cstr('arm,cortex-a53') +
         mkadt.cstr('ARM,v8')),
        ('reg', mkadt.u32(logical << 8)),
        ('cpu-id', mkadt.u32(logical)),
        ('cluster-id', mkadt.u32(0)),
        ('cluster-type', mkadt.u32(0)),
        ('die-cluster-id', mkadt.u32(0)),
        ('cluster-core-id', mkadt.u32(logical)),
        ('l2-cache-id', mkadt.u32(0)),
        ('l3-cache-id', mkadt.u32(0)),
        ('state', mkadt.cstr('running' if logical == 0 else 'stopped')),
        ('timebase-frequency', mkadt.u32(TIMEBASE)),
        ('clock-frequency', mkadt.u32(2002000000)),
        ('bus-frequency', mkadt.u32(100000000)),
        ('memory-frequency', mkadt.u32(1539000000)),
        ('peripheral-frequency', mkadt.u32(TIMEBASE)),
        ('fixed-frequency', mkadt.u32(TIMEBASE)),
    ]


def build(canvas=(FB_W, FB_H), dram=(DRAM_BASE, DRAM_SIZE)):
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    # SweetPlatformExpert matches 'samsung,a5xelte' (KernelConfigTables.cpp).
    set_prop(root, 'compatible', mkadt.cstr('samsung,xcover5') +
             mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,exynos850') +
             mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', mkadt.u64(dram[0]))
    set_prop(chosen, 'dram-size', mkadt.u64(dram[1]))
    set_prop(chosen, 'firmware-version', mkadt.cstr('xcover5-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('xcover5-entry'))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    for key, value in cpu_node(0):
        if key != 'cpu-id':
            set_prop(cpu0, key, value)
    for logical in range(1, NCPUS):
        cpus.children.append(mkadt.node(f'cpu{logical}', cpu_node(logical)))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('xcover5-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos850-io') + mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, 0x10000000, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        ('virtual-timer-intid', mkadt.u32(30)),
        ('reg', mkadt.u64(0x2a01000, 0x1000, 0x2a02000, 0x1000)),
    ]))
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        # AppleCLCD (SweetFramebuffer.cpp) matches 'samsung,a5xelte-display'.
        ('compatible', mkadt.cstr('samsung,xcover5-display') +
         mkadt.cstr('samsung,a5xelte-display')),
        ('canvas-width', mkadt.u32(canvas[0])),
        ('canvas-height', mkadt.u32(canvas[1])),
    ]))
    root.children = [n for n in root.children if n.prop('name') not in
                     (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    # pe_serial.c looks the console up by this name on the A5XELTE config.
    root.children.append(mkadt.node('a5xelte-fbcon', [
        ('reg', mkadt.u64(FB_PHYS, FB_W * FB_H * 4)),
        ('width', mkadt.u32(FB_W)),
        ('height', mkadt.u32(FB_H)),
        ('stride', mkadt.u32(FB_W * 4)),
    ]))
    # bsd/dev/a5xelte_emmc.c (patch 0125 accepts this window on XCOVER5).
    root.children.append(mkadt.node('a5xelte-emmc', [
        ('reg', mkadt.u64(EMMC_PHYS, 0x2000)),
    ]))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--canvas', default=f'{FB_W}x{FB_H}',
                        help='display size presented to iOS, WxH')
    # Boot 10: with the iPhone X size, 512 MiB ended in xpcproxy crash loops
    # ("Too many corpses"). 0x8a2800000:0x40000000 = 1 GiB above mfc_fw_rmem.
    parser.add_argument('--dram', default=f'{DRAM_BASE:#x}:{DRAM_SIZE:#x}',
                        help='XNU RAM window BASE:SIZE (high bank, not over 0x8a2400000-0x8a2800000)')
    args = parser.parse_args()
    dram = tuple(int(v, 0) for v in args.dram.split(':'))
    blob = adt.serialize(build(tuple(int(v) for v in args.canvas.split('x')), dram))
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
