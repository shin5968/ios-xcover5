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

# Boot 6: launchd stalled right after its first syscall, disk idle. The A55
# (ARMv8.2) implements PAN; with SCTLR_EL1.SPAN = 0 every exception to EL1
# sets PSTATE.PAN, and this kernel (__ARM_PAN_AVAILABLE__ 0, patch 0095)
# never clears it, so syscall copyin/copyout fault and retry forever. On the
# A53 (ARMv8.0) the bit is RES1 and nothing changes.
edit('osfmk/arm64/proc_reg.h', [
    ('#define SCTLR_EL1_DEFAULT \\\n',
     '#if defined(XCOVER5) && !__ARM_PAN_AVAILABLE__\n'
     '/* Cortex-A55 has PAN: keep PSTATE.PAN unchanged on exception entry. */\n'
     '#define SCTLR_SPAN_DEFAULT      SCTLR_PAN_UNCHANGED\n'
     '#else\n'
     '#define SCTLR_SPAN_DEFAULT      0\n'
     '#endif\n'
     '\n'
     '#define SCTLR_EL1_DEFAULT \\\n', 1),
    ('\t SCTLR_ROP_KEYS_DEFAULT | SCTLR_JOP_KEYS_DEFAULT)\n',
     '\t SCTLR_ROP_KEYS_DEFAULT | SCTLR_JOP_KEYS_DEFAULT | SCTLR_SPAN_DEFAULT)\n', 1),
])

edit('osfmk/arm64/arm_vm_init.c', [
    ('#if defined(DREAMLTE)\n', f'#if {S8_OR_XC5}\n', 2),
    ('#if defined(DREAMLTE) && !XNU_MONITOR\n', f'#if {S8_OR_XC5} && !XNU_MONITOR\n', 1),
])

# cpus=4 boot: "machine_routines.c:1058 Assertion failed: phys_cluster_id <=
# MAX_CPU_CLUSTER_PHY_ID". The A55's MPIDR has the core in Aff1 (MT bit), so
# the A5 rule "cluster = Aff1" made cores 1-3 clusters 1-3. On XCOVER5 the
# four listed cores are one cluster, indexed by Aff1.
edit('osfmk/arm64/machine_routines.c', [
    ('#if HAS_CLUSTER || defined(A5XELTE)\n\t\tuint32_t phys_cluster_id = MPIDR_CLUSTER_ID(cpu->phys_id);\n',
     '#if defined(XCOVER5)\n'
     '\t\tuint32_t phys_cluster_id = 0;     /* Aff1 is the core on the A55 */\n'
     '#elif HAS_CLUSTER || defined(A5XELTE)\n'
     '\t\tuint32_t phys_cluster_id = MPIDR_CLUSTER_ID(cpu->phys_id);\n', 1),
    ('\t\tif (MPIDR_CPU_ID(cpu->phys_id) > cluster_max_cpu_phys_id[phys_cluster_id]) {\n'
     '\t\t\tcluster_max_cpu_phys_id[phys_cluster_id] = MPIDR_CPU_ID(cpu->phys_id);\n'
     '\t\t}\n',
     '#if defined(XCOVER5)\n'
     '\t\tif ((int64_t)MPIDR_CLUSTER_ID(cpu->phys_id) > cluster_max_cpu_phys_id[0]) {\n'
     '\t\t\tcluster_max_cpu_phys_id[0] = MPIDR_CLUSTER_ID(cpu->phys_id);\n'
     '\t\t}\n'
     '#else\n'
     '\t\tif (MPIDR_CPU_ID(cpu->phys_id) > cluster_max_cpu_phys_id[phys_cluster_id]) {\n'
     '\t\t\tcluster_max_cpu_phys_id[phys_cluster_id] = MPIDR_CPU_ID(cpu->phys_id);\n'
     '\t\t}\n'
     '#endif\n', 1),
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

# Boot 2 diagnostics: the screen stopped after AppleCLCD, before the keys
# line IOFindBSDRoot prints after its 30 s IOBSD wait (timer suspect), and
# the RAM log ended earlier than the screen. Heartbeat: CNTP state and the
# GIC once (busy delay), then a line every 5 s through IOSleep (needs the
# timer interrupt), with the kernel's own RAM-log byte count.
edit('pexpert/arm/pe_serial.c', [
    ('\tklog_hdr[1] = klog_start;\n\tklog_hdr[2] = klog_size;\n}\n#endif\n',
     '\tklog_hdr[1] = klog_start;\n\tklog_hdr[2] = klog_size;\n}\n'
     '#if defined(XCOVER5)\n'
     'uint32_t xcover5_klog_size(void);\n'
     'uint32_t\n'
     'xcover5_klog_size(void)\n'
     '{\n'
     '\treturn klog_hdr ? klog_hdr[2] : 0xffffffffu;\n'
     '}\n'
     '\n'
     '/* _cnputs while the screen console is muted (a5fb_noconsole). */\n'
     'void xcover5_klog_write(const char *c, int size);\n'
     'void\n'
     'xcover5_klog_write(const char *c, int size)\n'
     '{\n'
     '\tfor (int i = 0; i < size; i++) {\n'
     '\t\tif (c[i] == \'\\n\') {\n'
     '\t\t\tklog_putc(\'\\r\');\n'
     '\t\t}\n'
     '\t\tklog_putc((uint8_t)c[i]);\n'
     '\t}\n'
     '}\n'
     '#endif\n'
     '#endif\n', 1),
])

# Boot 9: with a5fb_noconsole=1 the RAM log ended at the first GUI frame,
# because klog_putc sits in the screen console that _cnputs then skips.
edit('osfmk/console/serial_console.c', [
    ('#include <arm/cpu_data_internal.h>\n',
     '#include <arm/cpu_data_internal.h>\n#include <pexpert/arm64/board_config.h>\n', 1),
    ('\tif (sweet_console_muted && !panic_active()) {\n\t\treturn;\n\t}\n',
     '\tif (sweet_console_muted && !panic_active()) {\n'
     '#if defined(XCOVER5)\n'
     '\t\textern void xcover5_klog_write(const char *c, int size);\n'
     '\t\txcover5_klog_write(c, size);\n'
     '#endif\n'
     '\t\treturn;\n\t}\n', 1),
])

edit('iokit/Kernel/arm/SweetPlatformExpert.cpp', [
    ('OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);\n',
     'OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);\n'
     '\n'
     '#if defined(XCOVER5)\n'
     '#include <mach/mach_types.h>\n'
     '#include <kern/kern_types.h>\n'
     '#include <kern/clock.h>\n'
     'extern "C" {\n'
     'uint32_t xcover5_klog_size(void);\n'
     'extern uint64_t xcover5_emmc_reads, xcover5_emmc_writes;\n'
     'extern uint64_t xcover5_fb_swaps, xcover5_fb_presents, xcover5_fb_vsyncs;\n'
     'extern int xcover5_fb_off;\n'
     'void a5xelte_gic_diagnostics(unsigned int logical, const char *stage);\n'
     'kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);\n'
     'void thread_deallocate(thread_t);\n'
     '}\n'
     '\n'
     'static void\n'
     'xcover5_timer_state(const char *when)\n'
     '{\n'
     '\tuint64_t ctl = __builtin_arm_rsr64("CNTP_CTL_EL0");\n'
     '\tuint64_t cval = __builtin_arm_rsr64("CNTP_CVAL_EL0");\n'
     '\tuint64_t now = __builtin_arm_rsr64("CNTPCT_EL0");\n'
     '\tIOLog("XC5 TIMER %s: cntp_ctl=%llx cval=%llx now=%llx daif=%llx\\n", when,\n'
     '\t    ctl, cval, now, (uint64_t)__builtin_arm_rsr64("DAIF"));\n'
     '\ta5xelte_gic_diagnostics(0, when);\n'
     '}\n'
     '\n'
     'static void\n'
     'xcover5_heartbeat(__unused void *arg, __unused wait_result_t wr)\n'
     '{\n'
     '\tIOLog("XC5 heartbeat thread up, klog=%u\\n", xcover5_klog_size());\n'
     '\txcover5_timer_state("before");\n'
     '\tIODelay(200000);\n'
     '\txcover5_timer_state("after 200ms busy");\n'
     '\tfor (unsigned n = 1;; n++) {\n'
     '\t\tIOSleep(5000);\n'
     '\t\tIOLog("XC5 heartbeat %u t=%llu klog=%u emmc rd=%llu wr=%llu "\n'
     '\t\t    "fb swap=%llu present=%llu vsync=%llu off=%d\\n", n,\n'
     '\t\t    mach_absolute_time(), xcover5_klog_size(), xcover5_emmc_reads,\n'
     '\t\t    xcover5_emmc_writes, xcover5_fb_swaps, xcover5_fb_presents,\n'
     '\t\t    xcover5_fb_vsyncs, xcover5_fb_off);\n'
     '\t}\n'
     '}\n'
     '#endif\n', 1),
    ('\tIOLog("A5XELTE: platform expert registered\\n");\n',
     '\tIOLog("A5XELTE: platform expert registered\\n");\n'
     '#if defined(XCOVER5)\n'
     '\t{\n'
     '\t\tint hb = 1;\n'
     '\t\tthread_t t;\n'
     '\t\t(void)PE_parse_boot_argn("xc5hb", &hb, sizeof(hb));\n'
     '\t\tif (hb && kernel_thread_start(xcover5_heartbeat, NULL, &t) == KERN_SUCCESS) {\n'
     '\t\t\tthread_deallocate(t);\n'
     '\t\t}\n'
     '\t}\n'
     '#endif\n', 1),
])

# Boot 8 reached the lock screen, tinted red with UIKit's "rdar:45025538"
# bar: no status bar visual provider for a 240x493 pt screen (the donor saw
# the same at 360x800 pt, NOTES 0061). Present the iPhone X size and scale
# every frame down to the 720x1480 panel (fit height, centered, nearest).
# sweetfb_native=1 keeps the old 1:1 path.
edit('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ('#if defined(DREAMLTE)\n\tif (!native) {\n\t\tfLogicalW = SWEET_FB_LOGICAL_W;',
     '#if defined(DREAMLTE) || defined(XCOVER5)\n\tif (!native) {\n\t\tfLogicalW = SWEET_FB_LOGICAL_W;', 1),
    ('void\nAppleCLCD::present(const uint8_t *rec)\n',
     '#if defined(XCOVER5)\n'
     '/* Draw layer (dx,dy,w,h) of the logical (iPhone X) display, source (sx,sy),\n'
     ' * onto the panel scaled to fit its height; nearest neighbour. */\n'
     'static void\n'
     'xcover5_scaled_blit(uintptr_t pixels, uint32_t rowBytes, uint32_t pw, uint32_t ph,\n'
     '    uint32_t lw, uint32_t lh, SweetSurface *s, int32_t sx, int32_t sy,\n'
     '    int32_t dx, int32_t dy, int32_t w, int32_t h)\n'
     '{\n'
     '\tstatic int32_t col_lx[2048];\n'
     '\tuint32_t sw = (uint32_t)((uint64_t)lw * ph / lh);\n'
     '\tif (sw > pw) {\n'
     '\t\tsw = pw;\n'
     '\t}\n'
     '\tuint32_t ox = (pw - sw) / 2;\n'
     '\tuintptr_t src = s->kernelAddress();\n'
     '\tif (src == 0 || s->memory == NULL || s->elementWidth != 1 || s->elementHeight != 1 ||\n'
     '\t    (s->bytesPerElement != 0 && s->bytesPerElement != 4) || pw > 2048) {\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tif (w <= 0 || h <= 0) {\n'
     '\t\tw = (int32_t)s->width;\n'
     '\t\th = (int32_t)s->height;\n'
     '\t}\n'
     '\tif (sx < 0 || sy < 0) {\n'
     '\t\tsx = sy = 0;\n'
     '\t}\n'
     '\tif (dx < 0 || dy < 0) {\n'
     '\t\tdx = dy = 0;\n'
     '\t}\n'
     '\tfor (uint32_t px = 0; px < sw; px++) {\n'
     '\t\tcol_lx[px] = (int32_t)((uint64_t)px * lw / sw);\n'
     '\t}\n'
     '\tuint64_t len = s->memory->getLength();\n'
     '\tfor (uint32_t py = 0; py < ph; py++) {\n'
     '\t\tvolatile uint32_t *out = (volatile uint32_t *)(pixels + (uint64_t)py * rowBytes);\n'
     '\t\tfor (uint32_t px = 0; px < ox; px++) {\n'
     '\t\t\tout[px] = 0;\n'
     '\t\t\tout[pw - 1 - px] = 0;\n'
     '\t\t}\n'
     '\t\tint32_t ly = (int32_t)((uint64_t)py * lh / ph);\n'
     '\t\tint32_t ry = sy + ly - dy;\n'
     '\t\tif (ly < dy || ly >= dy + h || ry >= (int32_t)s->height) {\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '\t\tuint64_t rowoff = s->offset + (uint64_t)ry * s->bytesPerRow;\n'
     '\t\tfor (uint32_t px = 0; px < sw; px++) {\n'
     '\t\t\tint32_t lx = col_lx[px];\n'
     '\t\t\tint32_t rx = sx + lx - dx;\n'
     '\t\t\tif (lx < dx || lx >= dx + w || rx >= (int32_t)s->width) {\n'
     '\t\t\t\tcontinue;\n'
     '\t\t\t}\n'
     '\t\t\tuint64_t o = rowoff + (uint64_t)rx * 4;\n'
     '\t\t\tif (o + 4 <= len) {\n'
     '\t\t\t\tout[ox + px] = *(const uint32_t *)(src + o);\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t}\n'
     '}\n'
     '#endif\n'
     '\n'
     'void\nAppleCLCD::present(const uint8_t *rec)\n', 1),
    # Boot 9: black after the logo; the RAM log had stopped at the first
    # present. Keep logging a swap every 100 to see what is drawn later.
    ('\t\tif (sweet_fb_swaps_logged < 16) {\n',
     '#if defined(XCOVER5)\n'
     '\t\tif (sweet_fb_swaps_logged < 16 || get32(rec, 0x50) % 100 == 0) {\n'
     '#else\n'
     '\t\tif (sweet_fb_swaps_logged < 16) {\n'
     '#endif\n', 1),
    # cpus=4: kernel and touch alive, lock screen clock frozen. Count swaps
    # (backboardd SwapEnd), presents (copies to the panel), vsync ticks and
    # power-off for the heartbeat line.
    ('static uint64_t sweet_fb_swap_count;\n',
     'static uint64_t sweet_fb_swap_count;\n'
     '#if defined(XCOVER5)\n'
     'extern "C" {\n'
     'uint64_t xcover5_fb_swaps, xcover5_fb_presents, xcover5_fb_vsyncs;\n'
     'int xcover5_fb_off;\n'
     '}\n'
     '#endif\n', 1),
    ('\tif ((++sweet_fb_swap_count % 1000) == 0) {\n',
     '#if defined(XCOVER5)\n'
     '\txcover5_fb_swaps = sweet_fb_swap_count + 1;\n'
     '#endif\n'
     '\tif ((++sweet_fb_swap_count % 1000) == 0) {\n', 1),
    ('\tIOLockLock(fPresentLock);\n',
     '\tIOLockLock(fPresentLock);\n'
     '#if defined(XCOVER5)\n'
     '\txcover5_fb_presents++;\n'
     '#endif\n', 1),
    ('\tuint64_t count = ++fb->fVsyncCount;\n',
     '\tuint64_t count = ++fb->fVsyncCount;\n'
     '#if defined(XCOVER5)\n'
     '\txcover5_fb_vsyncs = count;\n'
     '\txcover5_fb_off = fb->fPowerOff ? 1 : 0;\n'
     '#endif\n', 1),
    ('\t\tuintptr_t src = s->kernelAddress();\n'
     '\t\tuint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;\n',
     '\t\tuintptr_t src = s->kernelAddress();\n'
     '\t\tuint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;\n'
     '#if defined(XCOVER5)\n'
     '\t\tif (fLogicalW != fWidth || fLogicalH != fHeight) {\n'
     '\t\t\txcover5_scaled_blit(fPixels, fRowBytes, fWidth, fHeight, fLogicalW, fLogicalH,\n'
     '\t\t\t    s, sx, sy, dx, dy, w, h);\n'
     '\t\t\ts->release();\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '#endif\n', 1),
])

# Boot 18/19: the lock screen stays up; SpringBoard loops forever on AKS
# 26 (assert hold) -> 27 (assert drop) -> 8 -> kIOReturnUnsupported, and the
# loop floods the console (thousands of lines, hiding everything else).
# Selector 8 (between get_lock_state 7 and unlock_device 9; 1 scalar in,
# nothing out) is answered like the other lock/unlock calls: success. Each
# selector is logged 20 times per process name, then only every 1000th call.
edit('iokit/Kernel/arm/SweetKeyStore.cpp', [
    ('\tkAKSGetLockState     = 7,\n',
     '\tkAKSGetLockState     = 7,\n'
     '\tkAKSSel8             = 8,       /* SpringBoard, 1 in, 0 out (XCover5 boot 18) */\n', 1),
    ('\tcase kAKSUnlockDevice:\n',
     '\tcase kAKSUnlockDevice:\n'
     '#if defined(XCOVER5)\n'
     '\tcase kAKSSel8:\n'
     '#endif\n', 1),
    ('\tchar name[32];\n\tproc_selfname(name, sizeof(name));\n'
     '\tIOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\\n", name, selector,\n',
     '\tchar name[32];\n\tproc_selfname(name, sizeof(name));\n'
     '#if defined(XCOVER5)\n'
     '\t{\n'
     '\t\tstatic uint32_t calls[128];\n'
     '\t\tuint32_t n = selector < 128 ? ++calls[selector] : 0;\n'
     '\t\tif (n > 20 && n % 1000 != 0) {\n'
     '\t\t\treturn ret;\n'
     '\t\t}\n'
     '\t}\n'
     '#endif\n'
     '\tIOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\\n", name, selector,\n', 1),
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
     '/*\n'
     ' * Galaxy XCover5 touch: Novatek NT36525 (TDDI) on SPI1 = USI_CMGP0 at\n'
     ' * 0x11d00000. The loader (shim/xcover5) sets up clocks, USI mode and pins,\n'
     ' * downloads the firmware Samsung ships in vendor/firmware (appended to the\n'
     ' * BOOT payload) and adds xc5touch=1 when the firmware reports ready. Here\n'
     ' * the event buffer (NT36672A map, 0x21c00) is polled every 10 ms: one byte\n'
     ' * status then 6 bytes per finger (id << 3 | state, x/y in 12 bits). The\n'
     ' * panel is 720x1480; the HID digitizer takes 1080x1920 for the whole\n'
     ' * presented display (iPhone X size scaled to 683x1480 at x 18, or the\n'
     ' * panel itself with sweetfb_native=1).\n'
     ' */\n'
     '#define XC5_SPI         0x11d00000u\n'
     '#define XC5_CMGP_PIN    0x11c30000u     /* gpm3 DAT at 0x64: CS */\n'
     '#define XC5_EVENT_BUF   0x21c00u\n'
     '#define XC5_FINGERS     10\n'
     'static vm_offset_t xc5ts_spi, xc5ts_pin;\n'
     'static int xc5ts_sleep, xc5ts_native;\n'
     'static bool xc5ts_down[XC5_FINGERS];\n'
     'static uint16_t xc5ts_x[XC5_FINGERS], xc5ts_y[XC5_FINGERS];\n'
     '/* Polls a finger has been missing; boot 15: releasing on the first miss\n'
     ' * turned every swipe into a tap (down, then up 10 ms later). */\n'
     'static uint8_t xc5ts_miss[XC5_FINGERS];\n'
     'static unsigned xc5ts_seen[XC5_FINGERS];\n'
     '#define XC5_RELEASE_MISSES 5\n'
     '\n'
     'static inline uint32_t\n'
     'xc5ts_rd(vm_offset_t base, uint32_t off)\n'
     '{\n'
     '\treturn *(volatile uint32_t *)(base + off);\n'
     '}\n'
     '\n'
     'static inline void\n'
     'xc5ts_wr(vm_offset_t base, uint32_t off, uint32_t v)\n'
     '{\n'
     '\t*(volatile uint32_t *)(base + off) = v;\n'
     '}\n'
     '\n'
     'static void\n'
     'xc5ts_cs(int level)\n'
     '{\n'
     '\tuint32_t d = xc5ts_rd(xc5ts_pin, 0x64);\n'
     '\txc5ts_wr(xc5ts_pin, 0x64, level ? (d | 1u) : (d & ~1u));\n'
     '\tdelay(2);\n'
     '}\n'
     '\n'
     '/* One byte full duplex, polled (FIFO levels: TX [14:6], RX [23:15]). */\n'
     'static int\n'
     'xc5ts_xfer(uint8_t tx)\n'
     '{\n'
     '\tfor (int i = 0; i < 100000; i++) {\n'
     '\t\tif (((xc5ts_rd(xc5ts_spi, 0x14) >> 6) & 0x1ffu) < 64) {\n'
     '\t\t\txc5ts_wr(xc5ts_spi, 0x18, tx);\n'
     '\t\t\tfor (int j = 0; j < 100000; j++) {\n'
     '\t\t\t\tif ((xc5ts_rd(xc5ts_spi, 0x14) >> 15) & 0x1ffu) {\n'
     '\t\t\t\t\treturn (int)(xc5ts_rd(xc5ts_spi, 0x1c) & 0xffu);\n'
     '\t\t\t\t}\n'
     '\t\t\t}\n'
     '\t\t\treturn -1;\n'
     '\t\t}\n'
     '\t}\n'
     '\treturn -1;\n'
     '}\n'
     '\n'
     '/* Page select FF a[23:15] a[14:7], then a[6:0], one dummy byte, data. */\n'
     'static bool\n'
     'xc5ts_read(uint32_t addr, uint8_t *buf, int n)\n'
     '{\n'
     '\tbool ok = true;\n'
     '\txc5ts_cs(0);\n'
     '\tok &= xc5ts_xfer(0xff) >= 0;\n'
     '\tok &= xc5ts_xfer((uint8_t)(addr >> 15)) >= 0;\n'
     '\tok &= xc5ts_xfer((uint8_t)(addr >> 7)) >= 0;\n'
     '\txc5ts_cs(1);\n'
     '\txc5ts_cs(0);\n'
     '\tok &= xc5ts_xfer((uint8_t)(addr & 0x7fu)) >= 0;\n'
     '\tok &= xc5ts_xfer(0) >= 0;\n'
     '\tfor (int i = 0; i < n; i++) {\n'
     '\t\tint v = xc5ts_xfer(0);\n'
     '\t\tok &= v >= 0;\n'
     '\t\tbuf[i] = (uint8_t)v;\n'
     '\t}\n'
     '\txc5ts_cs(1);\n'
     '\treturn ok;\n'
     '}\n'
     '\n'
     '/* Panel pixel -> HID digitizer 1080x1920 of the presented display. */\n'
     'static void\n'
     'xc5ts_map(unsigned px, unsigned py, int *x, int *y)\n'
     '{\n'
     '\tif (xc5ts_native) {\n'
     '\t\t*x = (int)(px * 1080u / 720u);\n'
     '\t} else {\n'
     '\t\tint lx = (int)px - 18;               /* 683 px wide, centered */\n'
     '\t\tlx = lx < 0 ? 0 : (lx > 682 ? 682 : lx);\n'
     '\t\t*x = lx * 1080 / 683;\n'
     '\t}\n'
     '\t*y = (int)(py * 1920u / 1480u);\n'
     '}\n'
     '\n'
     'static void\n'
     'xc5ts_thread(__unused void *arg, __unused wait_result_t wr)\n'
     '{\n'
     '\tuint8_t pt[66];\n'
     '\tunsigned errors = 0;\n'
     '\tfor (;;) {\n'
     '\t\tuint64_t deadline;\n'
     '\t\tbool now[XC5_FINGERS] = { false };\n'
     '\t\tclock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);\n'
     '\t\t(void)assert_wait_deadline(&xc5ts_sleep, THREAD_UNINT, deadline);\n'
     '\t\tthread_block(THREAD_CONTINUE_NULL);\n'
     '\t\tif (!xc5ts_read(XC5_EVENT_BUF, pt, 65)) {\n'
     '\t\t\tif (errors++ < 5) {\n'
     '\t\t\t\tprintf("XC5 TOUCH: SPI timeout\\n");\n'
     '\t\t\t}\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '\t\t/* Vendor buffers keep the address byte at [0]: their point[1 + 6i]\n'
     '\t\t * is our pt[6i] (boot 20: one byte off, every contact 1 poll). */\n'
     '\t\tif (pt[0] == 0xfd || pt[0] == 0xfe || pt[0] == 0x77) {\n'
     '\t\t\tcontinue;       /* watchdog / ESD marker: no data */\n'
     '\t\t}\n'
     '\t\tfor (int i = 0; i < XC5_FINGERS; i++) {\n'
     '\t\t\tconst uint8_t *q = &pt[6 * i];\n'
     '\t\t\tunsigned id = q[0] >> 3, st = q[0] & 7u;\n'
     '\t\t\tif (id == 0 || id > XC5_FINGERS || (st != 1 && st != 2)) {\n'
     '\t\t\t\tcontinue;\n'
     '\t\t\t}\n'
     '\t\t\tunsigned px = ((unsigned)q[1] << 4) | (q[3] >> 4);\n'
     '\t\t\tunsigned py = ((unsigned)q[2] << 4) | (q[3] & 0xfu);\n'
     '\t\t\tif (px >= 720 || py >= 1480) {\n'
     '\t\t\t\tcontinue;\n'
     '\t\t\t}\n'
     '\t\t\tint x, y, t = (int)id - 1;\n'
     '\t\t\txc5ts_map(px, py, &x, &y);\n'
     '\t\t\tnow[t] = true;\n'
     '\t\t\tif (!xc5ts_down[t]) {\n'
     '\t\t\t\tprintf("XC5 TOUCH: down id=%d st=%u hdr=%02x panel %u,%u\\n",\n'
     '\t\t\t\t    t, st, pt[0], px, py);\n'
     '\t\t\t\txc5ts_seen[t] = 0;\n'
     '\t\t\t}\n'
     '\t\t\txc5ts_down[t] = true;\n'
     '\t\t\txc5ts_miss[t] = 0;\n'
     '\t\t\txc5ts_seen[t]++;\n'
     '\t\t\txc5ts_x[t] = (uint16_t)x;\n'
     '\t\t\txc5ts_y[t] = (uint16_t)y;\n'
     '\t\t\tif (t < 5) {\n'
     '\t\t\t\tsweet_hid_touch(t, 1, x, y);\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tfor (int t = 0; t < XC5_FINGERS; t++) {\n'
     '\t\t\tif (xc5ts_down[t] && !now[t] && ++xc5ts_miss[t] >= XC5_RELEASE_MISSES) {\n'
     '\t\t\t\tprintf("XC5 TOUCH: up id=%d after %u polls\\n", t, xc5ts_seen[t]);\n'
     '\t\t\t\txc5ts_down[t] = false;\n'
     '\t\t\t\tif (t < 5) {\n'
     '\t\t\t\t\tsweet_hid_touch(t, 0, xc5ts_x[t], xc5ts_y[t]);\n'
     '\t\t\t\t}\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t}\n'
     '}\n'
     '\n'
     'static void\n'
     'xcover5_touch_start(void)\n'
     '{\n'
     '\tint ready = 0;\n'
     '\tthread_t thread;\n'
     '\t(void)PE_parse_boot_argn("xc5touch", &ready, sizeof(ready));\n'
     '\t(void)PE_parse_boot_argn("sweetfb_native", &xc5ts_native, sizeof(xc5ts_native));\n'
     '\tif (!ready) {\n'
     '\t\tprintf("XC5 TOUCH: off (loader did not start the firmware)\\n");\n'
     '\t\treturn;\n'
     '\t}\n'
     '\txc5ts_spi = ml_io_map(XC5_SPI, 0x1000);\n'
     '\txc5ts_pin = ml_io_map(XC5_CMGP_PIN, 0x1000);\n'
     '\tif (!xc5ts_spi || !xc5ts_pin) {\n'
     '\t\tprintf("XC5 TOUCH: MMIO mapping failed\\n");\n'
     '\t\treturn;\n'
     '\t}\n'
     '\tuint8_t id[8];\n'
     '\tbool ok = xc5ts_read(0x1f64e, id, 6);\n'
     '\tprintf("XC5 TOUCH: spi_st=%#x id %02x %02x %02x %02x %02x %02x%s\\n",\n'
     '\t    xc5ts_rd(xc5ts_spi, 0x14), id[0], id[1], id[2], id[3], id[4], id[5],\n'
     '\t    ok ? "" : " (timeout)");\n'
     '\tif (kernel_thread_start(xc5ts_thread, NULL, &thread) == KERN_SUCCESS) {\n'
     '\t\tthread_deallocate(thread);\n'
     '\t}\n'
     '}\n'
     '\n'
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
    ('#if defined(DREAMLTE)\n\tdreamlte_touch_start();   /* the A5 path below stays off */\n#endif\n',
     '#if defined(DREAMLTE)\n\tdreamlte_touch_start();   /* the A5 path below stays off */\n#endif\n'
     '#if defined(XCOVER5)\n\txcover5_touch_start();    /* the A5 path below stays off */\n#endif\n', 1),
])

edit('bsd/dev/a5xelte_emmc.c', [
    ('#include <pexpert/device_tree.h>\n',
     '#include <pexpert/device_tree.h>\n#include <pexpert/arm64/board_config.h>\n', 1),
    ('#define A5_LOG_SIZE   (4u << 20)\n',
     '#ifndef A5_EMMC_PHYS\n#define A5_EMMC_PHYS 0x13540000u\n#endif\n'
     '#ifndef A5_LOG_PARTITION\n#define A5_LOG_PARTITION "SYSTEM"\n#endif\n'
     '/* Sectors moved, for the XC5 heartbeat (busy disk or hang). */\n'
     'uint64_t xcover5_emmc_reads, xcover5_emmc_writes;\n'
     '#define A5_LOG_SIZE   (4u << 20)\n', 1),
    ('CMD_RESP_CRC | CMD_RESP | 17u;\n',
     'CMD_RESP_CRC | CMD_RESP | 17u;\n\txcover5_emmc_reads++;\n', 1),
    ('CMD_RESP_CRC | CMD_RESP | 24u;\n',
     'CMD_RESP_CRC | CMD_RESP | 24u;\n\txcover5_emmc_writes++;\n', 1),
    # Speed: boot 7 moved ~2 MB/s with one polled CMD17 per 512-byte sector.
    # Root I/O now goes up to 64 KiB per command (CMD18/CMD25, auto CMD12),
    # same PIO FIFO rules as the single-sector path. emmcmulti=0 reverts.
    ('static uint32_t\nemmc_le32(const uint8_t *p)\n',
     '#if defined(XCOVER5)\n'
     '#define EMMC_MULTI_MAX  128u            /* sectors per command: 64 KiB */\n'
     '#define CMD_SEND_STOP   (1u << 12)\n'
     '#define INT_ACD         (1u << 14)\n'
     '/* Boot 11: rint 0x424 = HTO, the FIFO was full and the controller paused\n'
     ' * the card clock until the CPU drained it. Not an error for PIO. */\n'
     '#define INT_HTO         (1u << 10)\n'
     '#define CMD_STOP_ABORT  (1u << 14)\n'
     'static uint64_t emmc_multi_buf[EMMC_MULTI_MAX * 512u / 8u];\n'
     'static int emmc_multi = -1;\n'
     '\n'
     '/* After a failed CMD18/CMD25 the card is still in its data state: without\n'
     ' * CMD12 the next read returns the old transfer\'s sectors (boot 11: HFS\n'
     ' * B-tree corruption). Stop it and empty the FIFO. */\n'
     'static void\n'
     'emmc_stop_transmission(void)\n'
     '{\n'
     '\tuint32_t cmd = CMD_START | CMD_RESP_CRC | CMD_RESP | CMD_STOP_ABORT | 12u;\n'
     '\tif ((emmc_rd(EMMC_CLKSEL) >> 16) & 7u) {\n'
     '\t\tcmd |= CMD_HOLD;\n'
     '\t}\n'
     '\tfor (unsigned i = 0; i < 1000000 && (emmc_rd(EMMC_CMD) & CMD_START); i++) {\n'
     '\t}\n'
     '\temmc_wr(EMMC_RINTSTS, 0xffffffffu);\n'
     '\temmc_wr(EMMC_CMDARG, 0);\n'
     '\temmc_wr(EMMC_CMD, cmd);\n'
     '\tfor (unsigned i = 0; i < 1000000; i++) {\n'
     '\t\tif (emmc_rd(EMMC_RINTSTS) & INT_CMD_DONE) {\n'
     '\t\t\tbreak;\n'
     '\t\t}\n'
     '\t}\n'
     '\tfor (unsigned i = 0; i < 4000000 && (emmc_rd(EMMC_STATUS) & (1u << 9)); i++) {\n'
     '\t}\n'
     '\temmc_wr(EMMC_CTRL, emmc_rd(EMMC_CTRL) | CTRL_FIFO_RESET);\n'
     '\tfor (unsigned i = 0; i < 1000000 && (emmc_rd(EMMC_CTRL) & CTRL_FIFO_RESET); i++) {\n'
     '\t}\n'
     '\temmc_wr(EMMC_RINTSTS, 0xffffffffu);\n'
     '}\n'
     '\n'
     'static int\n'
     'emmc_wait_idle(void)\n'
     '{\n'
     '\tfor (unsigned i = 0; i < 4000000; i++) {\n'
     '\t\tif (!(emmc_rd(EMMC_STATUS) & (1u << 9))) {\n'
     '\t\t\treturn 0;\n'
     '\t\t}\n'
     '\t}\n'
     '\treturn ETIMEDOUT;\n'
     '}\n'
     '\n'
     '/* nblk sectors at lba to/from emmc_multi_buf; root partition only. */\n'
     'static int\n'
     'emmc_xfer_blocks(uint32_t lba, uint32_t nblk, bool write)\n'
     '{\n'
     '\tuint32_t hcon = emmc_rd(EMMC_HCON);\n'
     '\tuint32_t width = (hcon >> 7) & 7u;\n'
     '\tuint32_t ver = emmc_rd(EMMC_VERID) & 0xffffu;\n'
     '\tuint32_t data_off = ver < 0x240au ? 0x100u : 0x200u;\n'
     '\tuint32_t ctrl = emmc_rd(EMMC_CTRL);\n'
     '\tuint32_t ints = emmc_rd(EMMC_INTMASK);\n'
     '\tuint32_t unit, words, done = 0;\n'
     '\tint error = 0;\n'
     '\n'
     '\tif (nblk == 0 || nblk > EMMC_MULTI_MAX || width > 2 || ver == 0 || ver == 0xffff ||\n'
     '\t    !(emmc_rd(EMMC_PWREN) & 1u) || !(emmc_rd(EMMC_CLKENA) & 1u)) {\n'
     '\t\treturn ENXIO;\n'
     '\t}\n'
     '\tif ((uint64_t)lba < emmc_first_lba ||\n'
     '\t    (uint64_t)lba + nblk > emmc_first_lba + emmc_nsectors) {\n'
     '\t\treturn EINVAL;\n'
     '\t}\n'
     '\tif (write && !emmc_writable) {\n'
     '\t\treturn EROFS;\n'
     '\t}\n'
     '\tunit = 2u << width;\n'
     '\twords = nblk * 512u / unit;\n'
     '\tif (emmc_wait_idle()) {\n'
     '\t\treturn EBUSY;\n'
     '\t}\n'
     '\temmc_wr(EMMC_INTMASK, 0);\n'
     '\temmc_wr(EMMC_CTRL, (ctrl & ~(CTRL_INT_ENABLE | CTRL_DMA_ENABLE |\n'
     '\t    CTRL_USE_IDMAC)) | CTRL_FIFO_RESET);\n'
     '\tfor (unsigned i = 0;; i++) {\n'
     '\t\tif (!(emmc_rd(EMMC_CTRL) & CTRL_FIFO_RESET)) {\n'
     '\t\t\tbreak;\n'
     '\t\t}\n'
     '\t\tif (i == 999999) {\n'
     '\t\t\terror = ETIMEDOUT;\n'
     '\t\t\tgoto out;\n'
     '\t\t}\n'
     '\t}\n'
     '\temmc_wr(EMMC_TMOUT, 0x573177ffu);\n'
     '\temmc_wr(EMMC_BLKSIZ, 512);\n'
     '\temmc_wr(EMMC_BYTCNT, nblk * 512u);\n'
     '\temmc_wr(EMMC_RINTSTS, 0xffffffffu);\n'
     '\temmc_wr(EMMC_CMDARG, lba);\n'
     '\tuint32_t command = CMD_START | CMD_WAIT_DATA | CMD_DATA | CMD_RESP_CRC | CMD_RESP;\n'
     '\tif (write) {\n'
     '\t\tcommand |= CMD_WRITE;\n'
     '\t}\n'
     '\tif (nblk > 1) {\n'
     '\t\tcommand |= CMD_SEND_STOP | (write ? 25u : 18u);\n'
     '\t} else {\n'
     '\t\tcommand |= write ? 24u : 17u;\n'
     '\t}\n'
     '\tif ((emmc_rd(EMMC_CLKSEL) >> 16) & 7u) {\n'
     '\t\tcommand |= CMD_HOLD;\n'
     '\t}\n'
     '\temmc_wr(EMMC_CMD, command);\n'
     '\tif (write) {\n'
     '\t\txcover5_emmc_writes += nblk;\n'
     '\t} else {\n'
     '\t\txcover5_emmc_reads += nblk;\n'
     '\t}\n'
     '\tuint64_t limit = 4000000ull + 400000ull * nblk;\n'
     '\tfor (uint64_t i = 0;; i++) {\n'
     '\t\tuint32_t pending = emmc_rd(EMMC_RINTSTS);\n'
     '\t\tif (pending & INT_HTO) {\n'
     '\t\t\temmc_wr(EMMC_RINTSTS, INT_HTO);\n'
     '\t\t}\n'
     '\t\tif (pending & INT_ERRORS & ~INT_HTO) {\n'
     '\t\t\tprintf("XC5 eMMC: CMD%u error lba=%u n=%u rint=%x status=%x words=%u/%u\\n",\n'
     '\t\t\t    command & 0x3fu, lba, nblk, pending, emmc_rd(EMMC_STATUS), done, words);\n'
     '\t\t\terror = EIO;\n'
     '\t\t\tgoto out;\n'
     '\t\t}\n'
     '\t\tuint32_t fifo = (emmc_rd(EMMC_STATUS) >> 17) & 0x1fffu;\n'
     '\t\tif (write) {\n'
     '\t\t\twhile (fifo < EMMC_FIFO_DEPTH && done < words) {\n'
     '\t\t\t\tif (unit == 8) {\n'
     '\t\t\t\t\temmc_wr(data_off, (uint32_t)emmc_multi_buf[done]);\n'
     '\t\t\t\t\temmc_wr(data_off + 4, (uint32_t)(emmc_multi_buf[done] >> 32));\n'
     '\t\t\t\t} else if (unit == 4) {\n'
     '\t\t\t\t\temmc_wr(data_off, ((uint32_t *)emmc_multi_buf)[done]);\n'
     '\t\t\t\t} else {\n'
     '\t\t\t\t\t*(volatile uint16_t *)(emmc_regs + data_off) =\n'
     '\t\t\t\t\t    ((uint16_t *)emmc_multi_buf)[done];\n'
     '\t\t\t\t}\n'
     '\t\t\t\tdone++;\n'
     '\t\t\t\tfifo++;\n'
     '\t\t\t}\n'
     '\t\t\tif (pending & INT_TXDR) {\n'
     '\t\t\t\temmc_wr(EMMC_RINTSTS, INT_TXDR);\n'
     '\t\t\t}\n'
     '\t\t} else {\n'
     '\t\t\twhile (fifo && done < words) {\n'
     '\t\t\t\tif (unit == 8) {\n'
     '\t\t\t\t\tuint32_t lo = emmc_rd(data_off);\n'
     '\t\t\t\t\tuint32_t hi = emmc_rd(data_off + 4);\n'
     '\t\t\t\t\temmc_multi_buf[done] = (uint64_t)lo | ((uint64_t)hi << 32);\n'
     '\t\t\t\t} else if (unit == 4) {\n'
     '\t\t\t\t\t((uint32_t *)emmc_multi_buf)[done] = emmc_rd(data_off);\n'
     '\t\t\t\t} else {\n'
     '\t\t\t\t\t((uint16_t *)emmc_multi_buf)[done] =\n'
     '\t\t\t\t\t    *(volatile uint16_t *)(emmc_regs + data_off);\n'
     '\t\t\t\t}\n'
     '\t\t\t\tdone++;\n'
     '\t\t\t\tfifo--;\n'
     '\t\t\t}\n'
     '\t\t\tif (pending & INT_RXDR) {\n'
     '\t\t\t\temmc_wr(EMMC_RINTSTS, INT_RXDR);\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tif (done == words && (pending & INT_DATA_OVER) && (pending & INT_CMD_DONE)) {\n'
     '\t\t\tbreak;\n'
     '\t\t}\n'
     '\t\tif (i == limit) {\n'
     '\t\t\tprintf("XC5 eMMC: CMD%u timeout lba=%u n=%u rint=%x words=%u/%u\\n",\n'
     '\t\t\t    command & 0x3fu, lba, nblk, pending, done, words);\n'
     '\t\t\terror = ETIMEDOUT;\n'
     '\t\t\tgoto out;\n'
     '\t\t}\n'
     '\t}\n'
     '\tif (nblk > 1) {\n'
     '\t\t/* The controller sends CMD12 itself; let it finish. */\n'
     '\t\tfor (unsigned i = 0; i < 1000000; i++) {\n'
     '\t\t\tif (emmc_rd(EMMC_RINTSTS) & INT_ACD) {\n'
     '\t\t\t\tbreak;\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t}\n'
     '\tif (write || nblk > 1) {\n'
     '\t\terror = emmc_wait_idle();\n'
     '\t}\n'
     'out:\n'
     '\tif (error) {\n'
     '\t\temmc_stop_transmission();\n'
     '\t}\n'
     '\temmc_wr(EMMC_RINTSTS, 0xffffffffu);\n'
     '\temmc_wr(EMMC_INTMASK, ints);\n'
     '\treturn error;\n'
     '}\n'
     '#endif\n'
     '\n'
     'static uint32_t\nemmc_le32(const uint8_t *p)\n', 1),
    ('\t\tuint32_t lba = (uint32_t)(emmc_first_lba + pos / 512u);\n',
     '\t\tuint32_t lba = (uint32_t)(emmc_first_lba + pos / 512u);\n'
     '#if defined(XCOVER5)\n'
     '\t\tif (emmc_multi < 0) {\n'
     '\t\t\temmc_multi = 1;\n'
     '\t\t\t(void)PE_parse_boot_argn("emmcmulti", &emmc_multi, sizeof(emmc_multi));\n'
     '\t\t}\n'
     '\t\tif (emmc_multi && skip == 0 && count - n >= 512u) {\n'
     '\t\t\tuint32_t nb = MIN((count - n) / 512u, EMMC_MULTI_MAX);\n'
     '\t\t\tif (buf_flags(bp) & B_READ) {\n'
     '\t\t\t\terror = emmc_xfer_blocks(lba, nb, false);\n'
     '\t\t\t\tif (!error) {\n'
     '\t\t\t\t\tmemcpy(vaddr + n, emmc_multi_buf, nb * 512u);\n'
     '\t\t\t\t}\n'
     '\t\t\t} else {\n'
     '\t\t\t\tmemcpy(emmc_multi_buf, vaddr + n, nb * 512u);\n'
     '\t\t\t\terror = emmc_xfer_blocks(lba, nb, true);\n'
     '\t\t\t}\n'
     '\t\t\tif (!error) {\n'
     '\t\t\t\tn += nb * 512u;\n'
     '\t\t\t\tbuf_setresid(bp, count - n);\n'
     '\t\t\t\tcontinue;\n'
     '\t\t\t}\n'
     '\t\t\t/* Retry this sector on the single-sector path below. */\n'
     '\t\t\tprintf("XC5 eMMC: multi-block %s failed (%d), sector path for lba=%u\\n",\n'
     '\t\t\t    (buf_flags(bp) & B_READ) ? "read" : "write", error, lba);\n'
     '\t\t\terror = 0;\n'
     '\t\t}\n'
     '#endif\n', 1),
    ('\tcase DKIOCGETMAXBLOCKCOUNTREAD:\n\t\t*(uint64_t *)data = 1;\n',
     '\tcase DKIOCGETMAXBLOCKCOUNTREAD:\n'
     '#if defined(XCOVER5)\n'
     '\t\t*(uint64_t *)data = emmc_multi ? EMMC_MULTI_MAX * 512u / emmc_secsize : 1;\n'
     '#else\n'
     '\t\t*(uint64_t *)data = 1;\n'
     '#endif\n', 1),
    ('\t\t*(uint64_t *)data = emmc_writable ? 1 : 0;\n',
     '#if defined(XCOVER5)\n'
     '\t\t*(uint64_t *)data = !emmc_writable ? 0 :\n'
     '\t\t    emmc_multi ? EMMC_MULTI_MAX * 512u / emmc_secsize : 1;\n'
     '#else\n'
     '\t\t*(uint64_t *)data = emmc_writable ? 1 : 0;\n'
     '#endif\n', 1),
    ('reg[0] != 0x13540000u', 'reg[0] != A5_EMMC_PHYS', 1),
    ('static const char system_name[] = "SYSTEM";', 'static const char system_name[] = A5_LOG_PARTITION;', 1),
    ('\tif (emmc_log_first_lba == emmc_first_lba && emmc_log_nsectors &&\n'
     '\t    strcmp(emmc_partition_name, "SYSTEM") == 0) {',
     '\tif (emmc_log_first_lba == emmc_first_lba && emmc_log_nsectors &&\n'
     '\t    strcmp(emmc_partition_name, A5_LOG_PARTITION) == 0) {', 1),
    # Boot 4: GPT read fine, but "USERDATA partition not found": the PIT says
    # USERDATA, the GPT names need not be upper case. Compare without case
    # and, if still missing, list the names that were there.
    ('static int\nemmc_find_root_partition(void)\n',
     '/* GPT names on the XCover5 need not be the PIT\'s upper case. */\n'
     'static bool\n'
     'emmc_name_char_eq(uint8_t got, char want)\n'
     '{\n'
     '\tuint8_t w = (uint8_t)want;\n'
     '\tif (got >= \'a\' && got <= \'z\') {\n'
     '\t\tgot -= 32;\n'
     '\t}\n'
     '\tif (w >= \'a\' && w <= \'z\') {\n'
     '\t\tw -= 32;\n'
     '\t}\n'
     '\treturn got == w;\n'
     '}\n'
     '\n'
     'static int\nemmc_find_root_partition(void)\n', 1),
    ('if (p[56 + k * 2] != (uint8_t)emmc_partition_name[k] ||',
     'if (!emmc_name_char_eq(p[56 + k * 2], emmc_partition_name[k]) ||', 1),
    ('if (p[56 + k * 2] != (uint8_t)system_name[k] ||',
     'if (!emmc_name_char_eq(p[56 + k * 2], system_name[k]) ||', 1),
    ('\t\tprintf("A5XELTE eMMC: %s partition not found\\n", emmc_partition_name);\n',
     '\t\tprintf("A5XELTE eMMC: %s partition not found; GPT names:", emmc_partition_name);\n'
     '\t\tfor (uint32_t s = 0; s < nentries / 4; s++) {\n'
     '\t\t\tif (emmc_read_sector((uint32_t)entry_lba + s, entry)) {\n'
     '\t\t\t\tbreak;\n'
     '\t\t\t}\n'
     '\t\t\tfor (uint32_t j = 0; j < 4; j++) {\n'
     '\t\t\t\tchar name[19];\n'
     '\t\t\t\tunsigned n = 0;\n'
     '\t\t\t\tfor (; n < 18 && entry[j * 128 + 56 + n * 2]; n++) {\n'
     '\t\t\t\t\tname[n] = (char)entry[j * 128 + 56 + n * 2];\n'
     '\t\t\t\t}\n'
     '\t\t\t\tname[n] = 0;\n'
     '\t\t\t\tif (n) {\n'
     '\t\t\t\t\tprintf(" %s", name);\n'
     '\t\t\t\t}\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tprintf("\\n");\n', 1),
    ('\tprintf("A5XELTE LOG: SYSTEM GPT first=%llu, %u sectors reserved\\n",\n',
     '\tprintf("A5XELTE LOG: " A5_LOG_PARTITION " GPT first=%llu, %u sectors reserved\\n",\n', 1),
])
