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
 * No line of either is copied. THIS FILE ONLY READS the PMIC.
 *
 * Boot-args:
 *   xc5pmic=1   find the channel, dump the PMIC block's registers 0x00-0x2f and the RTC block's 0x00-0x1f once,
 *               then every 100 ms read the PMIC block's 0x00-0x2f and print each register that changes (press
 *               the power key: the one that follows it is the key's register) -- for 10 minutes
 *   xc5pmic=2   the power key: read register xc5pwrreg (PMIC block) every 30 ms; bit xc5pwrbit set (or clear
 *               with xc5pwrlow=1) is "pressed", handed to the keys driver as its Power (bsd/dev/a5xelte_keys.c)
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
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

static void
pmic_thread(__unused void *arg, __unused wait_result_t wr_unused)
{
	int mode = (int)xcover5_pmic_state[0];
	mbox = (volatile uint8_t *)ml_io_map(ACPM_MBOX_PA, 0x1000);
	sram = (volatile uint8_t *)ml_io_map(ACPM_SRAM_PA, ACPM_SRAM_SIZE);
	if (find_channel() != 0) {
		printf("XC5 PMIC: no usable ACPM channel %u: stopping\n", CH_PMIC);
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
	int mode = 0;
	if (!PE_parse_boot_argn("xc5pmic", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	xcover5_pmic_state[0] = (uint64_t)mode;
	thread_t thread;
	if (kernel_thread_start(pmic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
