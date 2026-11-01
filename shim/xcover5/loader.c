/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy XCover5 (SM-G525F/N, Exynos 850 "s5e3830") XNU loader.
 *
 * Derived from shim/dreamlte/loader.c (Galaxy S8). S-Boot hands us the BOOT
 * ramdisk as /chosen/linux,initrd-{start,end}; it holds a tools/mkpayload.py
 * "SWEETXNU" v1 container (Mach-O segments, ADT, boot-args). We copy it into
 * the kernel-managed RAM window the ADT names (512 MiB at 0x880000000, below
 * the MFC firmware carve-out at 0x8a2400000), build boot_args and jump to
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
#define XNU_RAM_END		0x8a2400000UL	/* mfc_fw_rmem */
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
	while (keys_down())	/* wait for the restart combination to be released */
		delay_ms(20);
	/* VOL- pages back (it is the key that works: it opened the viewer) and
	 * wraps to the newest page after the oldest; VOL+ pages forward. */
	for (uint32_t idle = 0;; idle++) {
		unsigned k = keys_down();
		if (k & KEY_XCOVER)
			break;
		if (k & KEY_VOLDOWN)
			back = back + ROWS < lines + 1 ? back + ROWS / 2 : 0;
		else if ((k & KEY_VOLUP) && back)
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
	while (keys_down())
		delay_ms(20);
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
	    p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
		return fail("payload RAM outside 0x880000000-0x8a2400000");
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
