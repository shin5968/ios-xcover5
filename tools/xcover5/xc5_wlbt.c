/*
 * XCover5 Wi-Fi, step 1: start the Exynos 850's own Wi-Fi/Bluetooth core (Samsung SCSC "WLBT", Maxwell S612)
 * with Samsung's firmware, and see whether it runs. Boot-arg xc5wifi=1.
 *
 * Nothing here talks Wi-Fi yet. It powers the core up, answers its CFG_REQ (memory windows, boot patch), puts the
 * firmware and the start mailboxes in place, lets it go, and then for 20 s reports what the core does: the PMU's
 * state of it, the AP<->WLBT mailbox, the interrupts it raises, and the firmware's panic record.
 *
 * The sequence and every number are from the phone's device tree (research/bootdtb_0.dts: scsc_wifibt@119C0000 and
 * its syscons) and from Samsung's SCSC driver for the Exynos 850 (S5E3830), read for these facts in the Galaxy A21s
 * kernel (GitHub jameskdev/android_kernel_samsung_a21s-common, drivers/misc/samsung/scsc: platform_mif_3830.c,
 * mif_reg_S5E3830.h, mxman.c, fwhdr.c; GPL-2.0). No line of it is copied. The two binary inputs are not in this
 * tree: the firmware (vendor/etc/wifi/mx140.bin of the stock image) and the PMU boot patch ("KA patch", an 8051
 * program in that driver), both put into the CACHE drop zone by the user's own build (tools/xcover5/dev/wlbtfw.sh):
 *   /private/var/xc5/wifi/mx140.bin, /private/var/xc5/wifi/ka.bin
 *
 *   PMU (0x11860000)      VGPIO_TX_MONITOR 0x1700 is in the i3c_apm_pmic block (0x11A50000)
 *     0x3100 WLBT_CONFIGURATION [0] LOCAL_PWR_CFG   0x3104 WLBT_STATUS [0]   0x3108 WLBT_STATES
 *     0x310C WLBT_OPTION [3] DATA   0x3110 WLBT_CTRL_NS [6] ACTIVE_CLR [5] ACTIVE_EN   0x3114 WLBT_CTRL_S [3] START
 *     0x3920 TOP_OUT [1] PWRRGTON_CP   0x005C WLBT_DEBUG
 *   cold start: CTRL_S.START=1, OPTION.DATA=1, 3 us, TOP_OUT.PWRRGTON_CP=1, wait VGPIO_TX_MONITOR[29], 1 ms,
 *     CONFIGURATION.LOCAL_PWR_CFG=1, wait STATUS[0], CTRL_NS.ACTIVE_CLR=0, CTRL_NS.ACTIVE_EN=1
 *   then the core raises CFG_REQ (SPI 0xE6): SMC 0x82000710 (1 << 32 | 0) sets its TZPC non-secure; WLBT_REMAP
 *     (0x14C50000) +0x400 = 0x80000000 >> 12; DBUS BAAW (0x14C20000) window 0: WLBT 0x80000000-0x80C00000 -> the
 *     shared memory; PBUS BAAW (0x14C30000) windows 0 (0xA0000000 -> CP2WLBT mailbox 0x11950000) and 1 (0xA0010000
 *     -> GNSS2WLBT 0x119A0000 and the next ones); BOOT_CFG (0x14C60000) +0 = 1, the patch at +0x1000, +0 = 0,
 *     +4 = 1 (CFG_ACK). Each BAAW window: start >> 12, end >> 12, target >> 12, 0x80000003.
 *   the firmware ("smxf" header): copied to the start of the shared memory (the core sees it at 0x80000000); the
 *     AP2WLBT mailbox (0x119C0000) ISSR0 = entry point (header +40), ISSR1 = the configuration's offset in the
 *     shared memory (none yet: 0), ISSR2 = 0xbcdeedcb, ISSR3 = start flags (0) -- written before the release.
 * The shared memory: 12 MiB at 0x880000000, in the high bank below the MFC carve-out, which neither the loader nor
 * XNU (from 0x8a2800000) uses.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode_internal.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define PMU_PA          0x11860000UL
#define APM_PMIC_PA     0x11A50000UL
#define DBUS_BAAW_PA    0x14C20000UL
#define PBUS_BAAW_PA    0x14C30000UL
#define REMAP_PA        0x14C50000UL
#define BOOT_CFG_PA     0x14C60000UL
#define MBOX_PA         0x119C0000UL    /* AP2WLBT */
#define GICD_PA         0x12a01000UL
#define SHMEM_PA        0x880000000UL
#define SHMEM_SIZE      0xC00000UL

#define WLBT_CONFIGURATION 0x3100
#define WLBT_STATUS     0x3104
#define WLBT_STATES     0x3108
#define WLBT_OPTION     0x310C
#define WLBT_CTRL_NS    0x3110
#define WLBT_CTRL_S     0x3114
#define TOP_OUT         0x3920
#define WLBT_DEBUG      0x005C
#define VGPIO_TX_MONITOR 0x1700

#define INTSR0          0x014
#define INTMSR0         0x018
#define INTSR1          0x028
#define INTMSR1         0x02c
#define ISSR(n)         (0x080 + 4 * (n))

#define SPI_MBOX        (0x36 + 32)
#define SPI_ALIVE       (0x26 + 32)
#define SPI_CFG_REQ     (0xE6 + 32)
#define SPI_WDOG        (0xE7 + 32)

#define BAAW_CTRL       0x80000003u

uint64_t xcover5_wlbt_state[4];          /* [0] step reached, [1] CFG_REQ seen, [2] entry, [3] fw bytes */

static volatile uint8_t *pmu, *apm, *dbaaw, *pbaaw, *remap, *bootcfg, *mbox, *gicd;
static uint8_t *shm;

static inline uint32_t
rd(volatile uint8_t *b, uint32_t off)
{
	return *(volatile uint32_t *)(b + off);
}

static inline void
wr(volatile uint8_t *b, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(b + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline void
upd(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t val)
{
	wr(b, off, (rd(b, off) & ~mask) | (val & mask));
}

static bool
spi_pending(uint32_t intid)
{
	return (rd(gicd, 0x200 + (intid / 32) * 4) >> (intid % 32)) & 1;
}

static uint64_t
smc4(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory",
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	return x0;
}

/* A file of the drop zone into dst (at most max bytes): its length, or -1. */
static int64_t
read_file(const char *path, uint8_t *dst, size_t max)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
		return -1;
	}
	uint8_t *buf = kalloc_data(65536, Z_WAITOK);
	int64_t done = 0;
	while (buf != NULL && (size_t)done < max) {
		int resid = 0, n = (int)MIN((size_t)65536, max - (size_t)done);
		if (vn_rdwr(UIO_READ, vp, (caddr_t)buf, n, (off_t)done, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx)) != 0) {
			done = -1;
			break;
		}
		int got = n - resid;
		memcpy(dst + done, buf, (size_t)got);
		done += got;
		if (got < n) {
			break;
		}
	}
	if (buf != NULL) {
		kfree_data(buf, 65536);
	}
	vnode_close(vp, FREAD, ctx);
	return done;
}

static void
report(const char *when)
{
	printf("XC5 WLBT %s: STATUS %#x STATES %#x CONF %#x OPT %#x CTRL_NS %#x CTRL_S %#x DEBUG %#x | mbox INTSR0 %#x "
	    "INTMSR0 %#x INTSR1 %#x INTMSR1 %#x ISSR0-3 %#x %#x %#x %#x | spi mbox %d alive %d cfg %d wdog %d\n", when,
	    rd(pmu, WLBT_STATUS), rd(pmu, WLBT_STATES), rd(pmu, WLBT_CONFIGURATION), rd(pmu, WLBT_OPTION),
	    rd(pmu, WLBT_CTRL_NS), rd(pmu, WLBT_CTRL_S), rd(pmu, WLBT_DEBUG), rd(mbox, INTSR0), rd(mbox, INTMSR0),
	    rd(mbox, INTSR1), rd(mbox, INTMSR1), rd(mbox, ISSR(0)), rd(mbox, ISSR(1)), rd(mbox, ISSR(2)),
	    rd(mbox, ISSR(3)), spi_pending(SPI_MBOX), spi_pending(SPI_ALIVE), spi_pending(SPI_CFG_REQ),
	    spi_pending(SPI_WDOG));
}

static void
wlbt_thread(__unused void *arg, __unused wait_result_t w)
{
	/* the firmware comes with the drop, which is written once launchd remounts / read-write */
	int64_t fwlen = -1;
	shm = (uint8_t *)ml_io_map_wcomb(SHMEM_PA, SHMEM_SIZE);
	for (int s = 0; s < 240 && fwlen < 0; s++) {
		fwlen = read_file("/private/var/xc5/wifi/mx140.bin", shm, SHMEM_SIZE);
		if (fwlen < 0) {
			delay(1000000);
		}
	}
	if (fwlen < 64 || memcmp(shm + 8, "smxf", 4) != 0) {
		printf("XC5 WLBT: no firmware (/private/var/xc5/wifi/mx140.bin: %lld bytes): stopping\n", fwlen);
		return;
	}
	xcover5_wlbt_state[3] = (uint64_t)fwlen;
	uint32_t entry = *(uint32_t *)(shm + 40), hdr_len = *(uint32_t *)(shm + 16);
	uint32_t runtime = *(uint32_t *)(shm + 36), panic_off = *(uint32_t *)(shm + 176);
	xcover5_wlbt_state[2] = entry;
	printf("XC5 WLBT: firmware %lld bytes, header v%u.%u, api %u.%u, entry %#x, runtime %#x, panic record at %#x, "
	    "build %.40s\n", fwlen, *(uint16_t *)(shm + 14), *(uint16_t *)(shm + 12), *(uint16_t *)(shm + 22),
	    *(uint16_t *)(shm + 20), entry, runtime, panic_off, (const char *)(shm + 48));
	(void)hdr_len;
	bzero(shm + fwlen, SHMEM_SIZE - (size_t)fwlen);

	uint8_t ka[1024];
	int64_t kalen = read_file("/private/var/xc5/wifi/ka.bin", ka, sizeof(ka));

	pmu = (volatile uint8_t *)ml_io_map(PMU_PA, 0x10000);
	apm = (volatile uint8_t *)ml_io_map(APM_PMIC_PA, 0x2000);
	dbaaw = (volatile uint8_t *)ml_io_map(DBUS_BAAW_PA, 0x1000);
	pbaaw = (volatile uint8_t *)ml_io_map(PBUS_BAAW_PA, 0x1000);
	remap = (volatile uint8_t *)ml_io_map(REMAP_PA, 0x1000);
	bootcfg = (volatile uint8_t *)ml_io_map(BOOT_CFG_PA, 0x2000);
	mbox = (volatile uint8_t *)ml_io_map(MBOX_PA, 0x1000);
	gicd = (volatile uint8_t *)ml_io_map(GICD_PA, 0x1000);
	report("before");
	if (rd(pmu, WLBT_STATUS) & 1) {
		printf("XC5 WLBT: the core is already powered (left on by the boot loader?): not touched\n");
		return;
	}

	/* the start mailboxes, before the core runs (mxman.c mbox_init) */
	wr(mbox, ISSR(0), entry);
	wr(mbox, ISSR(1), 0);
	wr(mbox, ISSR(2), 0xbcdeedcb);
	wr(mbox, ISSR(3), 0);
	xcover5_wlbt_state[0] = 1;

	/* power up (platform_mif_pmu_reset_release, first boot) */
	upd(pmu, WLBT_CTRL_S, 1u << 3, 1u << 3);
	upd(pmu, WLBT_OPTION, 1u << 3, 1u << 3);
	delay(3);
	upd(pmu, TOP_OUT, 1u << 1, 1u << 1);
	uint32_t v = 0;
	for (int i = 0; i < 500 && !(v & (1u << 29)); i++) {
		v = rd(apm, VGPIO_TX_MONITOR);
		if (!(v & (1u << 29))) {
			delay(1000);
		}
	}
	printf("XC5 WLBT: VGPIO_TX_MONITOR %#x\n", v);
	delay(1000);
	upd(pmu, WLBT_CONFIGURATION, 1u, 1u);
	v = 0;
	for (int i = 0; i < 500 && !(v & 1); i++) {
		v = rd(pmu, WLBT_STATUS);
		if (!(v & 1)) {
			delay(1000);
		}
	}
	printf("XC5 WLBT: power %s (STATUS %#x)\n", (v & 1) ? "on" : "NOT on", v);
	upd(pmu, WLBT_CTRL_NS, 1u << 6, 0);
	upd(pmu, WLBT_CTRL_NS, 1u << 5, 1u << 5);
	xcover5_wlbt_state[0] = 2;

	/* CFG_REQ (platform_cfg_req_isr): watched for, not taken as an interrupt */
	bool cfg = false;
	for (int i = 0; i < 1000 && !cfg; i++) {
		cfg = spi_pending(SPI_CFG_REQ);
		if (!cfg) {
			delay(1000);
		}
	}
	xcover5_wlbt_state[1] = cfg;
	report(cfg ? "CFG_REQ" : "no CFG_REQ in 1 s");
	uint64_t smc = smc4(0x82000710, 1ull << 32, 0, 0);
	printf("XC5 WLBT: SMC CONN_IF set TZPC: %#llx\n", smc);
	wr(remap, 0x400, 0x80000000u >> 12);
	printf("XC5 WLBT: CHIP_VERSION_ID %#x\n", rd(remap, 0x410));
	wr(dbaaw, 0x0, 0x80000000u >> 12);
	wr(dbaaw, 0x4, 0x80C00000u >> 12);
	wr(dbaaw, 0x8, (uint32_t)(SHMEM_PA >> 12));
	wr(dbaaw, 0xC, BAAW_CTRL);
	wr(pbaaw, 0x0, 0xA0000000u >> 12);
	wr(pbaaw, 0x4, 0xA000FFFFu >> 12);
	wr(pbaaw, 0x8, 0x11950000u >> 12);
	wr(pbaaw, 0xC, BAAW_CTRL);
	wr(pbaaw, 0x10, 0xA0010000u >> 12);
	wr(pbaaw, 0x14, 0xA00CFFFFu >> 12);
	wr(pbaaw, 0x18, 0x119A0000u >> 12);
	wr(pbaaw, 0x1C, BAAW_CTRL);
	wr(bootcfg, 0x0, 1);
	if (kalen > 0) {
		for (int64_t i = 0; i + 4 <= kalen; i += 4) {
			wr(bootcfg, 0x1000 + (uint32_t)i, *(uint32_t *)(ka + i));
		}
	}
	wr(bootcfg, 0x0, 0);
	wr(bootcfg, 0x4, 1);            /* CFG_ACK */
	xcover5_wlbt_state[0] = 3;
	printf("XC5 WLBT: configured (KA patch %lld bytes), CFG_ACK given: the core runs from %#x\n", kalen, entry);

	/* 20 s of what it does */
	uint32_t last_states = 0xffffffff, last_sr0 = 0xffffffff, last_sr1 = 0xffffffff;
	for (int t = 0; t < 200; t++) {
		uint32_t st = rd(pmu, WLBT_STATES), sr0 = rd(mbox, INTSR0), sr1 = rd(mbox, INTSR1);
		if (st != last_states || sr0 != last_sr0 || sr1 != last_sr1 || t % 50 == 0) {
			char when[32];
			snprintf(when, sizeof(when), "t+%d ms", t * 100);
			report(when);
			last_states = st;
			last_sr0 = sr0;
			last_sr1 = sr1;
		}
		delay(100000);
	}
	if (panic_off != 0 && panic_off + 64 < SHMEM_SIZE) {
		uint32_t *p = (uint32_t *)(shm + panic_off);
		printf("XC5 WLBT: panic record @%#x: %08x %08x %08x %08x %08x %08x %08x %08x\n", panic_off, p[0], p[1], p[2],
		    p[3], p[4], p[5], p[6], p[7]);
	}
	printf("XC5 WLBT: step 1 over\n");
}

void xc5_wlbt_start(void);
void
xc5_wlbt_start(void)
{
	int on = 0;
	if (!PE_parse_boot_argn("xc5wifi", &on, sizeof(on)) || on == 0) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start(wlbt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
