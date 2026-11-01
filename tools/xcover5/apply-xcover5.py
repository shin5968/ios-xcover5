#!/usr/bin/env python3
"""Turn the Galaxy S8 (DREAMLTE) xnu tree into the Galaxy XCover5 build.

Input: an xnu-8796.141.3 tree with xnu/patches 0001-0124 applied (the S8
port). The A5XELTE machine config stays; patch 0124 defined DREAMLTE in
A5XELTE.h, this script defines XCOVER5 there instead and adds XCOVER5 cases
where the S8 code is board specific. Every replacement asserts its expected
match count. The diff of the result is xnu/patches/0125-xcover5.patch.

Board facts: stock DT of G525NKXSFEYL1 (docs/xcover5-port.md).
Usage: apply-xcover5.py XNU_TREE
"""
import sys
from pathlib import Path

tree = Path(sys.argv[1])


def edit(rel, pairs):
    path = tree / rel
    text = path.read_text()
    for old, new, count in pairs:
        found = text.count(old)
        if found != count:
            raise SystemExit(f'{rel}: expected {count}x {old!r}, found {found}')
        text = text.replace(old, new)
    path.write_text(text)
    print(f'edited {rel}')


S8_OR_XC5 = '(defined(DREAMLTE) || defined(XCOVER5))'

edit('pexpert/pexpert/arm64/A5XELTE.h', [(
    '#define DREAMLTE                      1\n'
    '#if defined(DREAMLTE)\n',
    '/*\n'
    ' * Samsung Galaxy XCover5 (SM-G525F/N, Exynos 850 "s5e3830") build. Same\n'
    ' * S-Boot handoff, GIC-400 and 26 MHz timer as the S8; Cortex-A55 cores\n'
    ' * (MT: MPIDR Aff1 = core), eMMC instead of UFS, video-mode panel.\n'
    ' * Values: stock DT of G525NKXSFEYL1 (boot.img dtb + dtbo).\n'
    ' */\n'
    '#define XCOVER5                       1\n'
    '#if defined(XCOVER5)\n'
    '#define A5_FB_PHYS                    0xfa000000  /* reserved-memory bootloader_fb */\n'
    '#define A5_FB_WIDTH                   720\n'
    '#define A5_FB_HEIGHT                  1480\n'
    '#define A5_FB_STRIDE                  2880        /* bytes per row */\n'
    '#define A5_FB_2M_BLOCKS               3           /* 720*1480*4 bytes */\n'
    '#define A5_ENTRY_MARKER_PHYS          0xb007f000  /* slot in the loader\'s RAM log A */\n'
    '#define A5_BOOT_MPIDR                 0x0         /* MPIDR[15:0] of the first A55 */\n'
    '#define A5_KEYS_DEFAULT               1           /* GPA0/GPA1 volume + XCover key */\n'
    '#define A5_TOUCH_DEFAULT              0           /* NT36525 is on SPI: no driver yet */\n'
    '#define A5_KLOG_PHYS                  0xb0080000  /* loader RAM log B (shim/xcover5) */\n'
    '#define A5_KLOG_SIZE                  0x80000\n'
    '#define A5_EMMC_PHYS                  0x12100000  /* dwmmc0 */\n'
    '#define A5_LOG_PARTITION              "CACHE"     /* eMMC log: first 4 MiB of CACHE */\n'
    '#define A5_SMP_TRAMPOLINE_PHYS        0xb0100000  /* low-bank page for PSCI CPU_ON */\n'
    '/* DT arch timer "use-physical-timer" (as on the S8): PPI 14 = INTID 30. */\n'
    '#define A5_USE_PHYS_TIMER             1\n'
    '#undef A5XELTE_VTIMER_INTID\n'
    '#define A5XELTE_VTIMER_INTID         30\n'
    '#elif defined(DREAMLTE)\n', 1)])

edit('osfmk/arm64/arm_vm_init.c', [
    ('#if defined(DREAMLTE)\n', f'#if {S8_OR_XC5}\n', 2),
    ('#if defined(DREAMLTE) && !XNU_MONITOR\n', f'#if {S8_OR_XC5} && !XNU_MONITOR\n', 1),
])

# Kernel console copy in the loader's RAM log zone (S8: TWRP's pmsg zone).
edit('pexpert/arm/pe_serial.c', [
    ('#if defined(DREAMLTE)\n', f'#if {S8_OR_XC5}\n', 3),
])

edit('iokit/Kernel/arm/SweetCPU.cpp', [
    ('#if defined(DREAMLTE)\nextern "C" vm_offset_t ml_io_map',
     f'#if {S8_OR_XC5}\nextern "C" vm_offset_t ml_io_map', 1),
    ('#if defined(DREAMLTE)\n\t/* Exynos 8895: logical 0..3',
     '#if defined(XCOVER5)\n'
     '\t/* Exynos 850: Cortex-A55 with MT, MPIDR Aff1 = core, Aff2 = cluster.\n'
     '\t * MPIDR[15:0] (the SWEET key) is unique only inside cluster 0, so the\n'
     '\t * ADT lists logical 0..3 = MPIDR 0x000..0x300. */\n'
     '\tuint32_t expected_mpidr = cpu->cpu_id < 4 ? (uint32_t)cpu->cpu_id << 8 : ~0U;\n'
     '#elif defined(DREAMLTE)\n\t/* Exynos 8895: logical 0..3', 1),
    ('\tcpuName = OSSymbol::withCStringNoCopy("Cortex-A53");\n',
     '#if defined(XCOVER5)\n'
     '\tcpuName = OSSymbol::withCStringNoCopy("Cortex-A55");\n'
     '#else\n'
     '\tcpuName = OSSymbol::withCStringNoCopy("Cortex-A53");\n'
     '#endif\n', 1),
    ('#if defined(DREAMLTE)\n\t{\n\t\t/*\n\t\t * The entry is in the high RAM bank',
     '#if defined(XCOVER5)\n'
     '\t{\n'
     '\t\t/* Entry in the high bank (0x88xxxxxxx). Samsung EL3 firmware may\n'
     '\t\t * keep the CPU_ON entry in a 32-bit register (S8), so go through a\n'
     '\t\t * low-bank trampoline: ldr x1, 8; br x1; .quad entry. Untested. */\n'
     '\t\tstatic vm_offset_t tramp;\n'
     '\t\tif (tramp == 0) {\n'
     '\t\t\ttramp = ml_io_map(A5_SMP_TRAMPOLINE_PHYS, 0x1000);\n'
     '\t\t}\n'
     '\t\tvolatile uint32_t *t = (volatile uint32_t *)tramp;\n'
     '\t\tt[0] = 0x58000041;              /* ldr x1, #8 */\n'
     '\t\tt[1] = 0xd61f0020;              /* br x1 */\n'
     '\t\tt[2] = (uint32_t)entry;\n'
     '\t\tt[3] = (uint32_t)((uint64_t)entry >> 32);\n'
     '\t\t__builtin_arm_dsb(DSB_SY);\n'
     '\t\tIOLog(CPU_PORT_NAME " CPU: cpu=%u entry 0x%llx via low trampoline 0x%llx\\n",\n'
     '\t\t    getCPUNumber(), (uint64_t)entry, (uint64_t)A5_SMP_TRAMPOLINE_PHYS);\n'
     '\t\tentry = (vm_offset_t)A5_SMP_TRAMPOLINE_PHYS;\n'
     '\t}\n'
     '#elif defined(DREAMLTE)\n\t{\n\t\t/*\n\t\t * The entry is in the high RAM bank', 1),
])

# Keys: Linux gpio-keys of the XCover5 dtbo. Power is on the S2MPU12 PMIC
# (not a GPIO): the XCover (push-to-talk) key acts as Power/lock instead.
edit('bsd/dev/a5xelte_keys.c', [
    ('#if defined(DREAMLTE)\n#define A5_ALIVE_PA 0x164b0000u',
     '#if defined(XCOVER5)\n'
     '#define A5_ALIVE_PA 0x11850000u            /* Exynos 850 pinctrl ALIVE */\n'
     '#elif defined(DREAMLTE)\n#define A5_ALIVE_PA 0x164b0000u', 1),
    ('#if defined(DREAMLTE)\n/*\n * Galaxy S8 (mainline',
     '#if defined(XCOVER5)\n'
     '/*\n'
     ' * Galaxy XCover5 (stock dtbo gpio_keys, active low): Volume Down GPA1-0,\n'
     ' * Volume Up GPA0-7, XCover key GPA0-5 (used as Power). Exynos 850 ALIVE\n'
     ' * banks: GPA0 0x000, GPA1 0x020; DAT at +4. Sample = GPA0 | GPA1 << 8;\n'
     ' * bit 31 is a constant 1 (released) for the absent Home key.\n'
     ' */\n'
     'typedef uint32_t keys_t;\n'
     '#define KEYS_SAMPLE() ((keys_t)((a5_keys_read(0x04) & 0xff) | (a5_keys_read(0x24) & 0xff) << 8 | 0x80000000u))\n'
     '#define KEYS_PINS { 5, 7, 8, 31 }\n'
     '#elif defined(DREAMLTE)\n/*\n * Galaxy S8 (mainline', 1),
    ('\t\t\t\tkeys_t bit = (keys_t)(1u << pins[key]);\n',
     '\t\t\t\tkeys_t bit = (keys_t)((keys_t)1 << pins[key]);\n', 1),
])

# SweetBattery (A5XELTE config) calls the S8 MAX77865 reader, which lives in
# the DREAMLTE part of this file (CI run 41285784017: undefined symbol).
edit('bsd/dev/a5xelte_touch.c', [
    ('#endif /* DREAMLTE */\n\nvoid\na5xelte_touch_probe(void)\n',
     '#endif /* DREAMLTE */\n'
     '\n'
     '#if defined(XCOVER5)\n'
     '/* No XCover5 fuel-gauge driver yet: SweetBattery publishes a static 100 %. */\n'
     'int dreamlte_battery_read(unsigned *percent, unsigned *mv, int *ma,\n'
     '    int *external, int *charging, int *full);\n'
     'int\n'
     'dreamlte_battery_read(__unused unsigned *percent, __unused unsigned *mv,\n'
     '    __unused int *ma, __unused int *external, __unused int *charging,\n'
     '    __unused int *full)\n'
     '{\n'
     '\treturn -1;\n'
     '}\n'
     '#endif\n'
     '\n'
     'void\na5xelte_touch_probe(void)\n', 1),
])

edit('bsd/dev/a5xelte_emmc.c', [
    ('#include <pexpert/device_tree.h>\n',
     '#include <pexpert/device_tree.h>\n#include <pexpert/arm64/board_config.h>\n', 1),
    ('#define A5_LOG_SIZE   (4u << 20)\n',
     '#ifndef A5_EMMC_PHYS\n#define A5_EMMC_PHYS 0x13540000u\n#endif\n'
     '#ifndef A5_LOG_PARTITION\n#define A5_LOG_PARTITION "SYSTEM"\n#endif\n'
     '#define A5_LOG_SIZE   (4u << 20)\n', 1),
    ('reg[0] != 0x13540000u', 'reg[0] != A5_EMMC_PHYS', 1),
    ('static const char system_name[] = "SYSTEM";', 'static const char system_name[] = A5_LOG_PARTITION;', 1),
    ('\tif (emmc_log_first_lba == emmc_first_lba && emmc_log_nsectors &&\n'
     '\t    strcmp(emmc_partition_name, "SYSTEM") == 0) {',
     '\tif (emmc_log_first_lba == emmc_first_lba && emmc_log_nsectors &&\n'
     '\t    strcmp(emmc_partition_name, A5_LOG_PARTITION) == 0) {', 1),
    ('\tprintf("A5XELTE LOG: SYSTEM GPT first=%llu, %u sectors reserved\\n",\n',
     '\tprintf("A5XELTE LOG: " A5_LOG_PARTITION " GPT first=%llu, %u sectors reserved\\n",\n', 1),
])
