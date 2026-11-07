/*
 * XCover5: the S2MPU12 PMIC through the Exynos 850's ACPM (the always-on power processor owns the PMIC's
 * SPEEDY bus; the AP asks it in a shared-SRAM queue). First user: the power key, which is the PMIC's own
 * (dtbo s2mpu12mfd@1f, s2mpu12-keys "pmic-keys: KEY_POWER"), not a GPIO like the volume and XCover keys.
 *
 * Numbers from the phone's device tree (research/bootdtb_0.dts, dtbo o_0.dts):
 *   acpm_ipc    mailbox 0x11900000 (0x1000), SRAM 0x02039000 (0x30000), initdata-base 0x7000
 *   s2mpu12mfd  acpm-ipc-channel 2, i2c-speedy-address, reg 0x1f, interrupt SPI 0x38 (not used: polled)
 * The protocol, after two ports that read Samsung's GPL drivers for it and wrote their own:
 *   y2sios (Exynos 990, MIT: kernel/xnu/files/bsd/dev/redwood_exynos_acpm.c) -- the channel table at
 *     initdata+8, the AP's count at initdata+24, 72-byte descriptors (rx = our requests at +16/+20/+24,
 *     tx = its answers at +40/+44/+48, length +52, element size +56, type +12), the doorbell INTGR0 bit
 *     16 + channel, answers pending in INTSR1, cleared in INTCR1, matched by a sequence in word 0 [21:16];
 *   Scion (Exynos 9810, docs in drivers/soc/exynos/hakios.exynos_acpm.c) -- the PMIC channel's words:
 *     word 0 = block << 8 | register (block: 0 top, 1 PMIC, 2 RTC), word 1 = function (0 read, 1 write,
 *     2 update) | value << 8 | mask << 16; the answer's word 1: value [15:8], status [31:24] (0 good).
 * No line of either is copied. The PMIC is only read, except one write: the RTC's RUDR latch (rtc_sync).
 *
 * Boot-args:
 *   xc5rtc=0    do not set the calendar from the PMIC's RTC (default: set it at start-up, rtc_sync below)
 *   xc5pmic=1  find the channel, dump the PMIC block's registers 0x00-0x2f and the RTC block's 0x00-0x1f once,
 *               then every 100 ms read the PMIC block's 0x00-0x2f and print each register that changes (press
 *               the power key: the one that follows it is the key's register) -- for 10 minutes
 *   xc5pmic=2   the power key: read register xc5pwrreg (PMIC block) every 30 ms; bit xc5pwrbit set (or clear
 *               with xc5pwrlow=1) is "pressed", handed to the keys driver as its Power (bsd/dev/a5xelte_keys.c)
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define ACPM_MBOX_PA    0x11900000UL
#define ACPM_SRAM_PA    0x02039000UL
#define ACPM_SRAM_SIZE  0x30000u
#define ACPM_INITDATA   0x7000u

#define INTGR0          0x08
#define INTCR1          0x20
#define INTSR1          0x28

#define CH_PMIC         2
#define TYPE_BUFFER     2
#define SEQ_SHIFT       16
#define SEQ_MASK        (0x3fu << SEQ_SHIFT)

#define BLK_PMIC        1
#define BLK_RTC         2

/* the keys driver ORs this into its Power (bsd/dev/a5xelte_keys.c, XCOVER5) */
volatile int xc5_pwrkey_down;
uint64_t xcover5_pmic_state[4];         /* [0] mode, [1] reads, [2] failed, [3] power presses */

static volatile uint8_t *mbox, *sram;
static struct {
	uint32_t id, len, elem, type, seq;
	volatile uint8_t *req_front, *req_rear, *req_base;     /* ours -> APM */
	volatile uint8_t *ans_front, *ans_rear, *ans_base;     /* APM -> ours */
} ch;

static inline uint32_t
rd(volatile uint8_t *at)
{
	return *(volatile uint32_t *)at;
}

static inline void
wr(volatile uint8_t *at, uint32_t v)
{
	*(volatile uint32_t *)at = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
in_sram(uint32_t off)
{
	return off < ACPM_SRAM_SIZE;
}

static int
find_channel(void)
{
	uint32_t table = rd(sram + ACPM_INITDATA + 8), count = rd(sram + ACPM_INITDATA + 24);
	printf("XC5 PMIC: ACPM initdata: channel table %#x, %u channels for the AP\n", table, count);
	if (!in_sram(table) || count <= CH_PMIC || count > 32) {
		return -1;
	}
	volatile uint8_t *d = sram + table + CH_PMIC * 72;
	uint32_t rq_rear = rd(d + 16), rq_front = rd(d + 20), rq_base = rd(d + 24);
	uint32_t an_rear = rd(d + 40), an_front = rd(d + 44), an_base = rd(d + 48);
	ch.id = rd(d) & 0xff;
	ch.type = rd(d + 12);
	ch.len = rd(d + 52);
	ch.elem = rd(d + 56);
	printf("XC5 PMIC: channel %u: id %u type %u queue %u x %u, req %#x/%#x/%#x ans %#x/%#x/%#x, polled %u\n",
	    CH_PMIC, ch.id, ch.type, ch.len, ch.elem, rq_rear, rq_front, rq_base, an_rear, an_front, an_base, rd(d + 68));
	if (ch.elem < 16 || ch.elem > 64 || ch.len > 64 || !in_sram(rq_rear) || !in_sram(rq_front) ||
	    !in_sram(rq_base) || !in_sram(an_rear) || !in_sram(an_front) || !in_sram(an_base)) {
		return -1;
	}
	ch.req_rear = sram + rq_rear;
	ch.req_front = sram + rq_front;
	ch.req_base = sram + rq_base;
	ch.ans_rear = sram + an_rear;
	ch.ans_front = sram + an_front;
	ch.ans_base = sram + an_base;
	return 0;
}

/* One request, its answer in cmd. 0, or -1. */
static int
ipc(uint32_t cmd[4])
{
	bool buffer = ch.type == TYPE_BUFFER || ch.len < 2;
	uint32_t front = buffer ? 0 : rd(ch.req_front), next = buffer ? 0 : (front + 1 == ch.len ? 0 : front + 1);
	if (!buffer) {
		/* stale answers first: a full answer queue stops the APM */
		if (rd(ch.ans_rear) != rd(ch.ans_front)) {
			wr(ch.ans_rear, rd(ch.ans_front));
			wr(mbox + INTCR1, 1u << ch.id);
		}
		if (next == rd(ch.req_rear)) {
			return -1;
		}
	}
	uint32_t seq = ch.seq = ch.seq >= 63 ? 1 : ch.seq + 1;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | (seq << SEQ_SHIFT);
	for (uint32_t i = 0; i < ch.elem / 4; i++) {
		wr(ch.req_base + ch.elem * front + i * 4, i < 4 ? cmd[i] : 0);
	}
	if (!buffer) {
		wr(ch.req_front, next);
	}
	wr(mbox + INTGR0, (1u << ch.id) << 16);
	uint64_t deadline;
	clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (rd(mbox + INTSR1) & (1u << ch.id)) {
			if (buffer) {
				for (int w = 0; w < 4; w++) {
					cmd[w] = rd(ch.ans_base + w * 4);
				}
				wr(mbox + INTCR1, 1u << ch.id);
				return 0;
			}
			uint32_t rear = rd(ch.ans_rear), last = rd(ch.ans_front);
			for (uint32_t i = rear; i != last; i = i + 1 == ch.len ? 0 : i + 1) {
				volatile uint8_t *e = ch.ans_base + ch.elem * i;
				if (((rd(e) & SEQ_MASK) >> SEQ_SHIFT) == seq) {
					for (int w = 0; w < 4; w++) {
						cmd[w] = rd(e + w * 4);
					}
					wr(ch.ans_rear, last);          /* one request at a time: the rest is stale */
					wr(mbox + INTCR1, 1u << ch.id);
					return 0;
				}
			}
		}
		delay(20);
	}
	return -1;
}

/* A PMIC register: its value, or -1. */
static int
pmic_read(uint32_t block, uint32_t reg)
{
	uint32_t cmd[4] = { block << 8 | reg, 0, 0, 0 };
	xcover5_pmic_state[1]++;
	if (ipc(cmd) != 0 || (cmd[1] >> 24) != 0) {
		xcover5_pmic_state[2]++;
		return -1;
	}
	return (int)((cmd[1] >> 8) & 0xff);
}

/* Write one PMIC register (function 1). 0, or -1. Used only for the RTC's RUDR latch below. */
static int
pmic_write(uint32_t block, uint32_t reg, uint32_t value)
{
	uint32_t cmd[4] = { block << 8 | reg, 1u | (value & 0xff) << 8, 0, 0 };
	xcover5_pmic_state[1]++;
	if (ipc(cmd) != 0 || (cmd[1] >> 24) != 0) {
		xcover5_pmic_state[2]++;
		return -1;
	}
	return 0;
}

static void
dump(const char *what, uint32_t block, uint32_t n)
{
	char line[200];
	size_t len = 0;
	for (uint32_t r = 0; r < n; r++) {
		int v = pmic_read(block, r);
		if (r % 16 == 0) {
			len = (size_t)snprintf(line, sizeof(line), "XC5 PMIC: %s %02x:", what, r);
		}
		len += (size_t)snprintf(line + len, sizeof(line) - len, v < 0 ? " --" : " %02x", v & 0xff);
		if (r % 16 == 15 || r + 1 == n) {
			printf("%s\n", line);
		}
	}
}

static int acpm_ready = -1;            /* -1 not tried, 0 no, 1 yes */

static bool
acpm_open(void)
{
	if (acpm_ready < 0) {
		mbox = (volatile uint8_t *)ml_io_map(ACPM_MBOX_PA, 0x1000);
		sram = (volatile uint8_t *)ml_io_map(ACPM_SRAM_PA, ACPM_SRAM_SIZE);
		acpm_ready = find_channel() == 0;
		if (!acpm_ready) {
			printf("XC5 PMIC: no usable ACPM channel %u\n", CH_PMIC);
		}
	}
	return acpm_ready == 1;
}

/*
 * The date: the S2MPU12's RTC block (2), read at start-up, sets the kernel's calendar (it started at 1970: the lock
 * screen said 31 December). Layout as Scion documents the S2MPS18's (drivers/power/hakios.s2mps18_rtc.c, from
 * Samsung's rtc-s2mp.h; the S2MPU12's is checked against the first boot's dump): 0x00 CTRL (bit 0 BCD, bit 1 24 h),
 * 0x02 UPDATE, 0x03 CAPSEL, 0x04 MSEC, 0x05 SEC, 0x06 MIN, 0x07 HOUR [4:0] (bit 6 PM), 0x08 WEEK, 0x09 DAY,
 * 0x0A MONTH (1-12), 0x0B YEAR (since 2000), binary. What is read is what was last latched; S-Boot latches on every
 * boot, early, so the time since then (the generic timer counts from S-Boot) is added. Nothing is written (no RUDR).
 * Android kept this clock on network time, in UTC. Boot-arg xc5rtc=0 leaves the calendar alone.
 */
extern void clock_set_calendar_microtime(clock_sec_t secs, clock_usec_t microsecs);

static uint64_t
days_secs(uint32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi, uint32_t s)
{
	static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	uint64_t days = 0;
	for (uint32_t year = 1970; year < y; year++) {
		days += ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
	}
	days += before[mo - 1] + (d - 1);
	if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
		days++;
	}
	return days * 86400 + (uint64_t)h * 3600 + (uint64_t)mi * 60 + s;
}

static uint32_t
bcd(uint32_t v, bool is_bcd)
{
	return is_bcd ? (v >> 4) * 10 + (v & 0xf) : v;
}

/*
 * Boots rtc1 and rtc2 (videos 165706, 180915): launchd still stamps 1969-12-31, and nothing of rtc_sync could be
 * read -- at that point of the boot the screen shows the heartbeat block, not the log. So the outcome is kept here
 * and said again every 20 s for three minutes by rtc_thread, which also sets the calendar again from the same
 * reading (advanced by the uptime since) whenever it finds the calendar still before 2020.
 */
static char rtc_msg[200] = "XC5 RTC: never ran";
static uint64_t rtc_secs, rtc_up_ns;           /* the date read, and the uptime when it was; 0: none */

static uint64_t
uptime_ns(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static void
rtc_thread(__unused void *arg, __unused wait_result_t wr_unused)
{
	for (int i = 0; i < 9; i++) {
		delay(20 * 1000 * 1000);
		clock_sec_t now;
		clock_usec_t us;
		clock_get_calendar_microtime(&now, &us);
		printf("%s; calendar now %lu (uptime %llu s)\n", rtc_msg, (unsigned long)now, uptime_ns() / NSEC_PER_SEC);
		if (rtc_secs != 0 && now < 1577836800ul) {     /* before 2020: set again */
			uint64_t t = rtc_secs + (uptime_ns() - rtc_up_ns) / NSEC_PER_SEC;
			clock_set_calendar_microtime((clock_sec_t)t, 0);
			clock_get_calendar_microtime(&now, &us);
			printf("XC5 RTC: calendar was before 2020: set again to %llu, reads back %lu\n", t, (unsigned long)now);
		}
	}
}

static void
rtc_sync(void)
{
	int r[12];
	uint32_t latch = 1;
	/*
	 * Samsung's own S2MPU12 driver (Galaxy A21s, Exynos 850 too: drivers/rtc/rtc-s2mpu12.c, read for these facts)
	 * latches before every read: UPDATE (0x02) written with its own value | RUDR (bit 0), 1 ms, then SEC..YEAR.
	 * Without it the registers hold whatever was latched last -- S-Boot's read, or an Android boot days ago.
	 * The same one write here (xc5rtclatch=0: read only). The layout is that driver's (no CONFIG_RTC_DRV_S2MP):
	 * MSEC 0x04, SEC 0x05 .. YEAR 0x0B, hour & 0x1f with PM bit 6, year + 2000; the RTC block is I2C address 2.
	 */
	(void)PE_parse_boot_argn("xc5rtclatch", &latch, sizeof(latch));
	if (latch) {
		int u = pmic_read(BLK_RTC, 0x02);
		int e = u < 0 ? -1 : pmic_write(BLK_RTC, 0x02, (uint32_t)u | 1u);
		delay(2000);
		printf("XC5 RTC: latched (UPDATE %#x | RUDR): %d\n", u, e);
	}
	printf("XC5 RTC2: reading RTC block 2 registers 00-0b\n");
	for (uint32_t i = 0; i < 12; i++) {
		r[i] = pmic_read(BLK_RTC, i);
		printf("XC5 RTC2: raw[%02x]=%d\n", i, r[i]);
		if (r[i] < 0) {
			snprintf(rtc_msg, sizeof(rtc_msg), "XC5 RTC: register %#x not readable (%llu reads, %llu failed)", i,
			    xcover5_pmic_state[1], xcover5_pmic_state[2]);
			printf("%s: calendar left alone\n", rtc_msg);
			return;
		}
	}
	printf("XC5 RTC: CTRL %02x UPDATE %02x CAPSEL %02x MSEC %02x SEC %02x MIN %02x HOUR %02x WEEK %02x DAY %02x "
	    "MONTH %02x YEAR %02x\n", r[0], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
	bool b = r[0] & 1;
	uint32_t sec = bcd(r[5] & 0x7f, b), min = bcd(r[6] & 0x7f, b), hour = bcd(r[7] & 0x1f, b);
	uint32_t day = bcd(r[9] & 0x3f, b), mon = bcd(r[10] & 0x1f, b), year = 2000 + bcd(r[11] & 0x7f, b);
	if (!(r[0] & 2) && (r[7] & 0x40) && hour < 12) {
		hour += 12;                     /* 12-hour mode, PM */
	}
	if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12 || year < 2024 || year > 2099) {
		snprintf(rtc_msg, sizeof(rtc_msg), "XC5 RTC: %u-%02u-%02u %02u:%02u:%02u is not a date to trust (CTRL %02x "
		    "raw %02x %02x %02x %02x %02x %02x)", year, mon, day, hour, min, sec, r[0], r[5], r[6], r[7], r[9], r[10],
		    r[11]);
		printf("%s: calendar left alone\n", rtc_msg);
		return;
	}
	uint64_t t = days_secs(year, mon, day, hour, min, sec), up_ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &up_ns);
	uint64_t up = up_ns / NSEC_PER_SEC, add = latch ? 0 : ((up >= 2 && up < 600) ? up - 1 : 0);
	clock_sec_t before, after; clock_usec_t usec;
	clock_get_calendar_microtime(&before, &usec);
	clock_set_calendar_microtime((clock_sec_t)(t + add), 0);
	clock_get_calendar_microtime(&after, &usec);
	printf("XC5 RTC2: before=%llu requested=%llu readback=%llu usec=%u\n",
	    (unsigned long long)before, t + add, (unsigned long long)after, (unsigned)usec);
	rtc_secs = t + add;
	rtc_up_ns = uptime_ns();
	snprintf(rtc_msg, sizeof(rtc_msg), "XC5 RTC: the PMIC's clock said %u-%02u-%02u %02u:%02u:%02u UTC at uptime %llu s; "
	    "set %llu, read back %lu", year, mon, day, hour, min, sec, up, t + add, (unsigned long)after);
	printf("%s\n", rtc_msg);
}

static void
pmic_thread(__unused void *arg, __unused wait_result_t wr_unused)
{
	int mode = (int)xcover5_pmic_state[0];
	if (!acpm_open()) {
		return;
	}
	int id = pmic_read(0, 0);
	printf("XC5 PMIC: top block register 0 (chip id?) = %#x\n", id);
	if (mode == 1) {
		uint8_t last[0x30];
		dump("pmic", BLK_PMIC, 0x30);
		dump("rtc", BLK_RTC, 0x20);
		for (uint32_t r = 0; r < sizeof(last); r++) {
			int v = pmic_read(BLK_PMIC, r);
			last[r] = (uint8_t)(v < 0 ? 0 : v);
		}
		printf("XC5 PMIC: watching the PMIC block 0x00-0x2f every 100 ms: press the power key\n");
		for (int t = 0; t < 6000; t++) {
			for (uint32_t r = 0; r < sizeof(last); r++) {
				int v = pmic_read(BLK_PMIC, r);
				if (v >= 0 && (uint8_t)v != last[r]) {
					printf("XC5 PMIC: pmic %02x: %02x -> %02x\n", r, last[r], v);
					last[r] = (uint8_t)v;
				}
			}
			delay(100000);
		}
		printf("XC5 PMIC: watch over (%llu reads, %llu failed)\n", xcover5_pmic_state[1], xcover5_pmic_state[2]);
		return;
	}
	uint32_t reg = 0, bit = 0, low = 0;
	if (!PE_parse_boot_argn("xc5pwrreg", &reg, sizeof(reg)) || !PE_parse_boot_argn("xc5pwrbit", &bit, sizeof(bit))) {
		printf("XC5 PMIC: xc5pmic=2 needs xc5pwrreg= and xc5pwrbit= (find them with xc5pmic=1)\n");
		return;
	}
	(void)PE_parse_boot_argn("xc5pwrlow", &low, sizeof(low));
	printf("XC5 PMIC: power key = PMIC register %#x bit %u (%s when pressed), every 30 ms\n", reg, bit,
	    low ? "clear" : "set");
	int was = 0;
	for (;;) {
		int v = pmic_read(BLK_PMIC, reg);
		if (v >= 0) {
			int down = ((v >> bit) & 1) != (int)low;
			if (down != was) {
				was = down;
				xc5_pwrkey_down = down;
				if (down) {
					xcover5_pmic_state[3]++;
				}
				printf("XC5 PMIC: power key %s\n", down ? "down" : "up");
			}
		}
		delay(30000);
	}
}

void xc5_pmic_start(void);
void
xc5_pmic_start(void)
{
	int mode = 0, rtc = 1;
	(void)PE_parse_boot_argn("xc5rtc", &rtc, sizeof(rtc));
	printf("XC5 RTC2: entered xc5rtc=%d\n", rtc);
	int connected = rtc ? acpm_open() : 0;
	printf("XC5 RTC2: ACPM=%d enabled=%d\n", connected, rtc);
	if (rtc && connected) {
		rtc_sync();             /* before launchd: everything after starts at the right date */
	} else if (rtc) {
		snprintf(rtc_msg, sizeof(rtc_msg), "XC5 RTC: ACPM not usable at start-up");
	}
	if (rtc) {
		thread_t rt;
		if (kernel_thread_start(rtc_thread, NULL, &rt) == KERN_SUCCESS) {
			thread_deallocate(rt);
		}
	}
	if (!PE_parse_boot_argn("xc5pmic", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	xcover5_pmic_state[0] = (uint64_t)mode;
	thread_t thread;
	if (kernel_thread_start(pmic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
