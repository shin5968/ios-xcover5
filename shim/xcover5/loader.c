/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy XCover5 (SM-G525F/N, Exynos 850 "s5e3830") XNU loader.
 *
 * Derived from shim/dreamlte/loader.c (Galaxy S8). S-Boot hands us the BOOT
 * ramdisk as /chosen/linux,initrd-{start,end}; it holds a tools/mkpayload.py
 * "SWEETXNU" v1 container (Mach-O segments, ADT, boot-args). We copy it into
 * the kernel-managed RAM window the ADT names (in the high bank, not over the
 * MFC firmware carve-out at 0x8a2400000), build boot_args and jump to
 * XNU's physical entry with the MMU off.
 *
 * Without a payload (any other ramdisk) the loader only prints what it
 * measured and stops: that is the first, harmless test of a new phone.
 *
 * Logs: there is no ramoops on the stock XCover5 kernel and no TWRP is
 * assumed. The loader keeps two "DBGC" text zones in free low RAM; the kernel
 * (patch 0125, A5_KLOG_PHYS) appends its console to the second one. Hold
 * VOLUME DOWN while the phone restarts (the forced-restart combination is
 * POWER + VOLUME DOWN, keep holding VOLUME DOWN) and the loader shows the
 * previous boot's log instead of booting.
 */
#include <stdint.h>
#include "../fdt.h"
#include "../font.h"

/* ---- board facts (stock DT of G525NKXSFEYL1: boot.img dtb + dtbo) ---- */
#define FB_BASE			0xfa000000UL	/* reserved-memory bootloader_fb (fb_handover) */
#define FB_W			720		/* panel nt36525_xcover5, video mode */
#define FB_H			1480
#define FB_STRIDE		(FB_W * 4)
#define ALIVE_PINCTRL		0x11850000UL	/* pinctrl@11850000: GPA0..GPA4, GPQ0 */
#define GPA0_DAT		0x004UL		/* VOLUP gpa0-7, XCover key gpa0-5 */
#define GPA1_DAT		0x024UL		/* VOLDOWN gpa1-0 */
#define LOG_A			0xb0000000UL	/* loader-only zone, holds the XNU marker */
#define LOG_B			0xb0080000UL	/* loader + kernel console (A5_KLOG_PHYS) */
#define LOG_SIZE		0x80000UL
#define PRZ_SIG			0x43474244U	/* "DBGC" */
#define XNU_MARKER_PHYS		0xb007f000UL	/* A5_ENTRY_MARKER_PHYS in 0125 */
#define LOW_RAM_BASE		0x80000000UL	/* initrd must be in the low bank */
#define LOW_RAM_END		0xbff80000UL	/* el3mon_reserve */
#define XNU_RAM_BASE		0x880000000UL	/* high bank */
#define XNU_RAM_END		0x900000000UL
#define MFC_RMEM		0x8a2400000UL	/* mfc_fw_rmem, 4 MiB: XNU RAM goes below or above */
#define MFC_RMEM_END		0x8a2800000UL
#define TIMER_HZ		26000000UL	/* DT arch timer clock-frequency */
#define PAGE_SIZE		4096UL
#define BOOT_LINE_LENGTH	608

/* ---- persistent log (persistent_ram text format, as on the S8) ---- */
struct prz {
	uint32_t sig, start, size;
	uint8_t data[];
};
static volatile struct prz *const prz = (volatile struct prz *)LOG_A;
static volatile struct prz *const prz2 = (volatile struct prz *)LOG_B;
#define PRZ_DATA_MAX	(LOG_SIZE - 12)
static uint32_t log_len, log2_len;

static void prz_put(volatile struct prz *z, uint32_t *len, char c)
{
	if (*len >= PRZ_DATA_MAX)
		return;
	z->data[(*len)++] = (uint8_t)c;
	z->start = *len;
	z->size = *len;
}

static void log_ch1(char c)
{
	prz_put(prz, &log_len, c);
}

static void log_ch(char c)
{
	prz_put(prz, &log_len, c);
	prz_put(prz2, &log2_len, c);
}

/* ---- screen console ---- */
static volatile uint32_t *const fb = (volatile uint32_t *)FB_BASE;
#define SCALE	2
#define CELL_W	(8 * SCALE)
#define CELL_H	(16 * SCALE + 4)
#define TOP	96
#define COLS	((FB_W - 32) / CELL_W)
#define ROWS	((FB_H - TOP - 40) / CELL_H)
static int row, col;
static int screen_on = 1;
static uint32_t fg = 0xffffffffU;

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h && y < FB_H; y++)
		for (int x = x0; x < x0 + w && x < FB_W; x++)
			fb[y * (FB_STRIDE / 4) + x] = c;
}

static void glyph(int c0l, int r0w, char c)
{
	if (c < 0x20 || c > 0x7e)
		c = '?';
	const unsigned char *g = font8x16[c - 0x20];
	int x0 = 16 + c0l * CELL_W, y0 = TOP + r0w * CELL_H;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			     (g[gy] & (0x80 >> gx)) ? fg : 0xff000000U);
}

static void scr_ch(char c)
{
	if (c == '\n' || col >= COLS) {
		col = 0;
		row = (row + 1) % ROWS;
		fill(0, TOP + row * CELL_H, FB_W, CELL_H, 0xff000000U);
		if (c == '\n')
			return;
	}
	glyph(col++, row, c);
}

static void out(const char *s)
{
	while (*s) {
		log_ch(*s);
		if (screen_on)
			scr_ch(*s);
		s++;
	}
}

static void hex(uint64_t v)
{
	static const char digits[] = "0123456789abcdef";
	char b[19] = "0x";
	int n = 2, started = 0;
	for (int i = 60; i >= 0; i -= 4) {
		int d = (v >> i) & 0xf;
		if (d || started || i == 0) {
			b[n++] = digits[d];
			started = 1;
		}
	}
	b[n] = 0;
	out(b);
}

static void kv(const char *k, uint64_t v)
{
	out(k);
	out("=");
	hex(v);
	out(" ");
}

/* ---- keys (ALIVE GPIOs, active low; S-Boot owns mux and pulls) ---- */
#define KEY_VOLUP	1
#define KEY_VOLDOWN	2
#define KEY_XCOVER	4

static unsigned keys_down(void)
{
	uint32_t a0 = *(volatile uint32_t *)(ALIVE_PINCTRL + GPA0_DAT);
	uint32_t a1 = *(volatile uint32_t *)(ALIVE_PINCTRL + GPA1_DAT);
	unsigned k = 0;
	if (!(a0 & (1u << 7)))
		k |= KEY_VOLUP;
	if (!(a1 & (1u << 0)))
		k |= KEY_VOLDOWN;
	if (!(a0 & (1u << 5)))
		k |= KEY_XCOVER;
	return k;
}

/* CNTFRQ_EL0 may be 0 (S8); count with the DT's 26 MHz. */
static void delay_ms(uint64_t ms)
{
	uint64_t t0, t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	do
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	while (t - t0 < ms * (TIMER_HZ / 1000));
}

/* ---- previous-boot log viewer ---- */
static int prz_valid(volatile struct prz *z)
{
	return z->sig == PRZ_SIG && z->size <= PRZ_DATA_MAX && z->start <= PRZ_DATA_MAX;
}

/* Text of a zone in order: oldest byte first (the kernel wraps it). */
static uint8_t prz_at(volatile struct prz *z, uint32_t i)
{
	uint32_t first = z->size < PRZ_DATA_MAX ? 0 : z->start;
	return z->data[(first + i) % PRZ_DATA_MAX];
}

/* Index of the first byte of the line `back` lines above the end. */
static uint32_t line_start(volatile struct prz *z, uint32_t back)
{
	uint32_t i = z->size;
	while (i > 0) {
		if (prz_at(z, i - 1) == '\n') {
			if (back == 0)
				return i;
			back--;
		}
		i--;
	}
	return 0;
}

static void glyphs(int c0l, int r0w, const char *s)
{
	for (; *s && c0l < COLS; s++)
		glyph(c0l++, r0w, *s);
}

static char *udec(char *p, uint32_t v)
{
	char t[10];
	int n = 0;
	do
		t[n++] = (char)('0' + v % 10);
	while ((v /= 10) && n < 10);
	while (n)
		*p++ = t[--n];
	*p = 0;
	return p;
}

static void view_page(volatile struct prz *z, uint32_t back, uint32_t lines)
{
	fill(0, 0, FB_W, FB_H, 0xff000000U);
	/* Position: last line on screen / total lines (total = newest). */
	char pos[48], *p = pos;
	const char *k = "PREV BOOT LOG  line ";
	while (*k)
		*p++ = *k++;
	p = udec(p, lines > back ? lines - back : 0);
	*p++ = '/';
	udec(p, lines);
	fg = back ? 0xffff8040U : 0xff40ff40U;
	glyphs(0, -2, pos);
	uint32_t i = line_start(z, back + ROWS - 2);
	int r = 0, c = 0;
	fg = 0xffffffffU;
	for (; i < z->size && r < ROWS - 1; i++) {
		char ch = (char)prz_at(z, i);
		if (ch == '\r')		/* XNU's console ends lines with \r\n */
			continue;
		if (ch == '\n' || c >= COLS) {
			r++;
			c = 0;
			if (ch == '\n')
				continue;
			if (r >= ROWS - 1)
				break;
		}
		glyph(c++, r, ch);
	}
	fg = 0xffffff00U;
	glyphs(0, ROWS - 1, back ? "VOL- older  VOL+ newer  XCOVER: boot"
				  : "NEWEST  VOL- older  XCOVER: boot");
}

/* Raw ALIVE data (active low) above the page: VOL+ was not seen on GPA0-7
 * (boot 10), this shows which bit a key really moves. */
static void draw_keys(void)
{
	char pos[32], *p = pos;
	const char *k = "GPA0=";
	while (*k)
		*p++ = *k++;
	p = udec(p, *(volatile uint32_t *)(ALIVE_PINCTRL + GPA0_DAT) & 0xff);
	k = " GPA1=";
	while (*k)
		*p++ = *k++;
	p = udec(p, *(volatile uint32_t *)(ALIVE_PINCTRL + GPA1_DAT) & 0xff);
	*p++ = ' ';
	*p = 0;
	fg = 0xff808080U;
	glyphs(0, -1, pos);
}

static void log_viewer(void)
{
	volatile struct prz *z = prz_valid(prz2) ? prz2 : prz_valid(prz) ? prz : 0;
	fill(0, 0, FB_W, FB_H, 0xff000000U);
	if (!z) {
		fg = 0xffff4040U;
		glyph(0, 0, '-');
		const char *m = "no log in RAM (cold boot?)";
		for (int j = 0; m[j]; j++)
			glyph(j + 2, 0, m[j]);
		delay_ms(3000);
		return;
	}
	uint32_t back = 0, lines = 0;
	for (uint32_t i = 0; i < z->size; i++)
		lines += prz_at(z, i) == '\n';
	view_page(z, back, lines);
	draw_keys();
	/* Boot 10: the XCover key's GPA0-5 reads 0 when released, so waiting for
	 * "no key down" never ended. Work on changes instead: a key acts when its
	 * pin leaves the level it had at rest (1.5 s after the viewer opened, so
	 * the restart combination is let go), whichever polarity that is.
	 * VOL- pages back and wraps to the newest page; VOL+ pages forward. */
	delay_ms(1500);
	unsigned rest = keys_down(), prev = rest;
	for (uint32_t idle = 0;; idle++) {
		unsigned now = keys_down();
		unsigned hit = (now ^ rest) & ~(prev ^ rest);	/* just left rest */
		prev = now;
		if (hit & KEY_XCOVER)
			break;
		if (hit & KEY_VOLDOWN)
			back = back + ROWS < lines + 1 ? back + ROWS / 2 : 0;
		else if ((hit & KEY_VOLUP) && back)
			back = back > ROWS / 2 ? back - ROWS / 2 : 0;
		else {
			delay_ms(20);
			if (idle % 10 == 0)
				draw_keys();
			continue;
		}
		view_page(z, back, lines);
		draw_keys();
		delay_ms(150);
	}
	delay_ms(500);
}

/* ---- payload (tools/mkpayload.py v1) ---- */
struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};
struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};
struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};
_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

uint64_t dl_boot_args;	/* read by start.S */

/* MMU off: everything is Device memory, keep accesses naturally aligned. */
static void copy(uintptr_t dst, uintptr_t src, uint64_t n)
{
	if (!((dst | src) & 7)) {
		for (; n >= 8; n -= 8, dst += 8, src += 8)
			*(volatile uint64_t *)dst = *(volatile uint64_t *)src;
	}
	for (; n; n--, dst++, src++)
		*(volatile uint8_t *)dst = *(volatile uint8_t *)src;
}

static void zero(uintptr_t dst, uint64_t n)
{
	for (; n && (dst & 7); n--, dst++)
		*(volatile uint8_t *)dst = 0;
	for (; n >= 8; n -= 8, dst += 8)
		*(volatile uint64_t *)dst = 0;
	for (; n; n--, dst++)
		*(volatile uint8_t *)dst = 0;
}

static void clean(uintptr_t start, uintptr_t end)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uintptr_t p = start & ~(line - 1); p < end; p += line)
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int magic_ok(const volatile char *m)
{
	const char *want = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static uint64_t fail(const char *why)
{
	fg = 0xffff4040U;
	out("FAIL: ");
	out(why);
	out("\nPOWER+VOL DOWN 7s restarts. Download mode:\n"
	    "VOL UP+VOL DOWN while plugging USB in.\n");
	clean(LOG_A, LOG_A + LOG_SIZE);
	clean(LOG_B, LOG_B + LOG_SIZE);
	return 0;
}

/* Leave an 8-byte hole at XNU_MARKER_PHYS inside zone A's text. XNU's
 * start_first_cpu stores "XNUELTE1" there (0125 A5_ENTRY_MARKER_PHYS). */
static void reserve_marker(void)
{
	uint32_t at = (uint32_t)(XNU_MARKER_PHYS - (LOG_A + 12));
	const char *tag = "\nXNU entry marker: [";
	uint32_t tag_len = 0;
	while (tag[tag_len])
		tag_len++;
	while (log_len < at - tag_len)
		log_ch1(log_len % 128 == 127 ? '\n' : ' ');
	for (uint32_t i = 0; i < tag_len; i++)
		log_ch1(tag[i]);
	for (int i = 0; i < 8; i++)
		log_ch1('-');
	log_ch1(']');
	log_ch1('\n');
}

static void probe_cpu(uintptr_t dtb, uint64_t el, uint64_t sctlr)
{
	uint64_t mpidr, mmfr0, isar0, pfr0, cntfrq, cnt, midr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
	__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
	__asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
	__asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(cnt));
	uint64_t pc = (uint64_t)(uintptr_t)&probe_cpu;
	kv("EL", el);
	kv("SCTLR", sctlr);
	out("\n");
	kv("DTB", dtb);
	kv("loader", pc);
	out("\n");
	kv("MIDR", midr);
	kv("MPIDR", mpidr);
	out("\n");
	kv("MMFR0", mmfr0);
	kv("ISAR0", isar0);
	out("\n");
	kv("PFR0", pfr0);
	kv("CNTFRQ", cntfrq);
	out("\n");
	kv("CNTPCT", cnt);
	out("\n");
}

/* ---- touch: Novatek NT36525 on SPI1 (USI_CMGP0), probe only ----
 * Stock dtbo: novatek,NVT-ts-spi on spi_1 = spi@11d00000, CS gpm3-0 (GPIO),
 * bus gpm0/1/2-0 function 2, IRQ gpa0-0. Clocks, USI mode and pins follow
 * mainline exynos850 (clk-exynos850.c CMU_CMGP, exynos-usi.c, pinctrl
 * banks, spi-s3c64xx.c with clk_div 4). NT36xxx SPI framing (Linux RFC
 * nt36xxx_spi.c): page select FF a[23:15] a[14:7], then a[6:0] (read) or
 * a[6:0]|0x80 (write) followed by data. Reads only: no reset, no download. */
#define CMU_TOP			0x120e0000UL
#define CMU_CMGP		0x11c00000UL
#define SYSREG_CMGP_USI0	0x11c22000UL	/* SW_CONF of USI_CMGP0 */
#define USI_CMGP0		0x11d000c0UL	/* USI_CON +4, USI_OPTION +8 */
#define PINCTRL_CMGP		0x11c30000UL	/* gpm0..gpm7 at 0x20 steps */
#define SPI1			0x11d00000UL
#define GATE_ON			(1u << 21)

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void touch_cs(int level)
{
	uint32_t dat = rd32(PINCTRL_CMGP + 0x64);
	wr32(PINCTRL_CMGP + 0x64, level ? (dat | 1) : (dat & ~1u));
	delay_ms(1);
}

/* One byte full duplex; returns the received byte or -1 on timeout. */
static int spi_xfer(uint8_t tx)
{
	for (int i = 0; i < 100000; i++) {
		if (((rd32(SPI1 + 0x14) >> 6) & 0x1ff) < 64) {
			wr32(SPI1 + 0x18, tx);
			for (int j = 0; j < 100000; j++)
				if ((rd32(SPI1 + 0x14) >> 15) & 0x1ff)
					return (int)(rd32(SPI1 + 0x1c) & 0xff);
			return -1;
		}
	}
	return -1;
}

static void touch_page(uint32_t addr)
{
	touch_cs(0);
	spi_xfer(0xff);
	spi_xfer((uint8_t)(addr >> 15));
	spi_xfer((uint8_t)(addr >> 7));
	touch_cs(1);
}

/* Read n bytes at addr; prints the address byte's echo plus n+1 bytes so
 * a dummy byte, if the chip sends one, is visible. */
static void touch_read(const char *what, uint32_t addr, int n)
{
	int v[20];
	touch_page(addr);
	touch_cs(0);
	v[0] = spi_xfer((uint8_t)(addr & 0x7f));
	for (int i = 1; i <= n + 1 && i < 20; i++)
		v[i] = spi_xfer(0);
	touch_cs(1);
	out(what);
	out(":");
	for (int i = 0; i <= n + 1 && i < 20; i++) {
		static const char d[] = "0123456789abcdef";
		char b[4] = { ' ', '-', '-', 0 };
		if (v[i] >= 0) {
			b[1] = d[(v[i] >> 4) & 0xf];
			b[2] = d[v[i] & 0xf];
		}
		out(b);
	}
	out("\n");
}

static void touch_probe(void)
{
	fg = 0xff40ffffU;
	out("\nTOUCH SPI PROBE (NT36525)\n");
	fg = 0xffffffffU;
	kv("cmgp_bus", rd32(CMU_TOP + 0x2000));
	kv("usi0_mux", rd32(CMU_CMGP + 0x1004));
	out("\n");
	kv("usi0_div", rd32(CMU_CMGP + 0x1804));
	kv("ipclk", rd32(CMU_CMGP + 0x2044));
	out("\n");
	/* Clocks on: CMGP bus, GPIO, sysreg, USI0 ipclk/pclk. USI0 stays on the
	 * CMGP bus as S-Boot left it (mux 1): probe 1 switched it to the RCO and
	 * the FIFO never moved. Divide by 32 (the SPI block divides by 4 more). */
	wr32(CMU_TOP + 0x2000, rd32(CMU_TOP + 0x2000) | GATE_ON);
	wr32(CMU_CMGP + 0x2018, rd32(CMU_CMGP + 0x2018) | GATE_ON);
	wr32(CMU_CMGP + 0x2040, rd32(CMU_CMGP + 0x2040) | GATE_ON);
	wr32(CMU_CMGP + 0x1004, rd32(CMU_CMGP + 0x1004) | 1u);
	wr32(CMU_CMGP + 0x1804, (rd32(CMU_CMGP + 0x1804) & ~0x1fu) | 0x1f);
	wr32(CMU_CMGP + 0x2044, rd32(CMU_CMGP + 0x2044) | GATE_ON);
	wr32(CMU_CMGP + 0x2048, rd32(CMU_CMGP + 0x2048) | GATE_ON);
	delay_ms(1);
	kv("sw_conf", rd32(SYSREG_CMGP_USI0));
	kv("usi_con", rd32(USI_CMGP0 + 4));
	out("\n");
	wr32(SYSREG_CMGP_USI0, (rd32(SYSREG_CMGP_USI0) & ~7u) | 2);	/* SPI */
	wr32(USI_CMGP0 + 4, rd32(USI_CMGP0 + 4) & ~1u);		/* out of reset */
	wr32(USI_CMGP0 + 8, (rd32(USI_CMGP0 + 8) & ~4u) | 2);	/* CLKREQ_ON */
	/* Pins: gpm0/1/2-0 function 2 (SPI), gpm3-0 output high (CS). */
	for (uintptr_t b = 0; b < 0x60; b += 0x20)
		wr32(PINCTRL_CMGP + b, (rd32(PINCTRL_CMGP + b) & ~0xfu) | 2);
	wr32(PINCTRL_CMGP + 0x64, rd32(PINCTRL_CMGP + 0x64) | 1);
	wr32(PINCTRL_CMGP + 0x60, (rd32(PINCTRL_CMGP + 0x60) & ~0xfu) | 1);
	/* SPI: reset, master, mode 0, 8-bit, PIO, controller CS inactive. */
	wr32(SPI1 + 0x00, 1u << 5);
	delay_ms(1);
	wr32(SPI1 + 0x00, 0);
	wr32(SPI1 + 0x08, 0);
	wr32(SPI1 + 0x0c, 1);
	wr32(SPI1 + 0x10, 0);
	wr32(SPI1 + 0x24, 0x1f);
	wr32(SPI1 + 0x24, 0);
	wr32(SPI1 + 0x00, 3);					/* TX + RX on */
	kv("spi_st", rd32(SPI1 + 0x14));
	kv("irq gpa0-0", rd32(ALIVE_PINCTRL + 0x004) & 1);
	out("\n");
	/* Controller alone first: self loopback must echo what it sends. Try
	 * with the controller's nCS inactive, then active (unmuxed pin). */
	for (int cs = 1; cs >= 0; cs--) {
		wr32(SPI1 + 0x0c, (uint32_t)cs);
		wr32(SPI1 + 0x08, 1u << 3);
		int a = spi_xfer(0xa5), b = spi_xfer(0x5a);
		wr32(SPI1 + 0x08, 0);
		kv(cs ? "loop nCS1 a5" : "loop nCS0 a5", (uint64_t)(int64_t)a);
		kv("5a", (uint64_t)(int64_t)b);
		kv("st", rd32(SPI1 + 0x14));
		out("\n");
		wr32(SPI1 + 0x00, 1u << 5);	/* flush FIFOs */
		delay_ms(1);
		wr32(SPI1 + 0x00, 3);
	}
	/* Real transfers run with the controller's nCS active as well. */
	wr32(SPI1 + 0x0c, 0);
	touch_read("trim 1f64e", 0x1f64e, 7);	/* chip ID: .. 25 65 03 = NT36525 */
	touch_read("boot 1f141", 0x1f141, 2);	/* BOOT_RDY */
	touch_read("evt 11a00", 0x11a00, 16);	/* event buffer */
	touch_read("evt 11a78", 0x11a78, 4);	/* fw version/state area */
	kv("spi_st", rd32(SPI1 + 0x14));
	out("\n");
}

uint64_t dl_main(uintptr_t dtb, uint64_t el, uint64_t sctlr)
{
	/* Before the log zones are reset: show the previous boot's log? */
	if (keys_down() & KEY_VOLDOWN)
		log_viewer();
	prz->sig = PRZ_SIG;
	prz->start = prz->size = 0;
	prz2->sig = PRZ_SIG;
	prz2->start = prz2->size = 0;
	fill(0, 0, FB_W, FB_H, 0xff000000U);
	fill(0, 0, FB_W, 8, 0xff00a0ffU);
	fg = 0xffffff00U;
	out("XC5LOADER BEGIN (ios-xcover5 XNU loader)\n");
	fg = 0xffffffffU;
	probe_cpu(dtb, el, sctlr);
	if (el != 1)
		return fail("not entered at EL1");

	uint64_t rd_start, rd_end;
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return fail("no initrd in /chosen");
	kv("initrd", rd_start);
	kv("end", rd_end);
	out("\n");
	if (rd_start < LOW_RAM_BASE || rd_end > LOW_RAM_END || rd_end <= rd_start)
		return fail("initrd outside low RAM");

	const volatile struct payload *p = (const volatile struct payload *)rd_start;
	/* initrd-end is exclusive (probe 1: 4096-byte ramdisk at 0x84080000,
	 * end 0x84081000); the stock DT's 0x847fffff is only a placeholder. */
	uint64_t length = rd_end - rd_start;
	if (length < sizeof(struct payload) || !magic_ok(p->magic) || p->version != 1) {
		touch_probe();
		fg = 0xff40ff40U;
		out("\nPROBE OK: no SWEETXNU payload in the ramdisk.\n"
		    "Send a photo of this screen.\n"
		    "Restore: flash the stock boot.img with Odin.\n");
		clean(LOG_A, LOG_A + LOG_SIZE);
		clean(LOG_B, LOG_B + LOG_SIZE);
		return 0;
	}
	uint64_t nsegs = p->nsegs;
	kv("segs", nsegs);
	kv("virt", p->virt_base);
	kv("entry", p->entry_va);
	out("\n");
	kv("phys", p->phys_base);
	kv("mem", p->mem_size);
	kv("adt", p->adt_size);
	out("\n");
	if (!nsegs || nsegs > 32 ||
	    __builtin_offsetof(struct payload, segs) + nsegs * sizeof(struct payload_seg) > length ||
	    p->adt_off > length || p->adt_size > length - p->adt_off)
		return fail("bad payload header");
	if (p->phys_base < XNU_RAM_BASE || p->phys_base >= XNU_RAM_END || !p->mem_size ||
	    p->mem_size > XNU_RAM_END - p->phys_base ||
	    (p->phys_base < MFC_RMEM_END && p->phys_base + p->mem_size > MFC_RMEM) ||
	    p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
		return fail("payload RAM outside the high bank or over mfc_fw_rmem");
	uintptr_t mem_end = p->phys_base + p->mem_size;
	uintptr_t kernel_end = p->phys_base + (p->kernel_end - p->virt_base);
	if (kernel_end >= mem_end)
		return fail("kernel larger than RAM window");

	/* iBoot hands XNU zeroed RAM; S-Boot leaves the previous OS's data
	 * (S8: XNU hung in vm_mem_bootstrap depending on the reboot method). */
	out("zeroing XNU RAM...\n");
	zero(p->phys_base, p->mem_size);
	out("zeroed\n");

	for (uint64_t i = 0; i < nsegs; i++) {
		const volatile struct payload_seg *s = &p->segs[i];
		if (s->vmaddr < p->virt_base || s->file_size > s->vmsize ||
		    s->file_off > length || s->file_size > length - s->file_off ||
		    s->vmsize > p->mem_size || s->vmaddr - p->virt_base > p->mem_size - s->vmsize)
			return fail("bad segment");
		uintptr_t dst = p->phys_base + (s->vmaddr - p->virt_base);
		copy(dst, rd_start + s->file_off, s->file_size);
		zero(dst + s->file_size, s->vmsize - s->file_size);
		clean(dst, dst + s->vmsize);
		out("seg ");
		hex(i);
		out(" -> ");
		hex(dst);
		out(" ");
		hex(s->vmsize);
		out("\n");
	}

	uintptr_t adt_pa = kernel_end;
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (ba_pa + PAGE_SIZE > mem_end)
		return fail("no room for ADT/boot_args");
	copy(adt_pa, rd_start + p->adt_off, p->adt_size);
	zero(ba_pa, PAGE_SIZE);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = ba_pa + PAGE_SIZE;
	ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	clean(adt_pa, ba_pa + PAGE_SIZE);

	uintptr_t entry_pa = p->phys_base + (p->entry_va - p->virt_base);
	if (entry_pa >= kernel_end)
		return fail("entry outside kernel");
	dl_boot_args = ba_pa;
	kv("adt_pa", adt_pa);
	kv("boot_args", ba_pa);
	out("\n");
	kv("entry_pa", entry_pa);
	out("\ncmdline: ");
	for (int i = 0; i < BOOT_LINE_LENGTH && p->cmdline[i]; i++) {
		char c[2] = { p->cmdline[i], 0 };
		out(c);
	}
	out("\n");
	fg = 0xff40ff40U;
	out("\nJUMP TO XNU\n");
	reserve_marker();
	clean(LOG_A, LOG_A + LOG_SIZE);
	clean(LOG_B, LOG_B + LOG_SIZE);
	return entry_pa;
}

void dl_exception(uint64_t slot)
{
	uint64_t esr, elr, far;
	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));
	screen_on = 1;
	fg = 0xffff4040U;
	out("\nLOADER EXCEPTION ");
	kv("slot", slot);
	kv("ESR", esr);
	kv("ELR", elr);
	kv("FAR", far);
	out("\n");
	clean(LOG_A, LOG_A + LOG_SIZE);
	clean(LOG_B, LOG_B + LOG_SIZE);
	for (;;)
		__asm__ volatile("wfe");
}

/* gcc may emit calls for struct copies; keep the image self-contained. */
void *memset(void *d, int c, unsigned long n)
{
	unsigned char *q = d;
	while (n--)
		*q++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *q = d;
	const unsigned char *r = s;
	while (n--)
		*q++ = *r++;
	return d;
}
