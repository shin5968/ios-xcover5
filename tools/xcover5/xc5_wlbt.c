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
#define SHMEM_SIZE      0xC00000UL      /* what the core is given (DBUS BAAW window 0) */
/*
 * wifi1 (video 191101): ml_io_map_wcomb of all 12 MiB panicked -- "kmem_alloc(..., 12582912, 0x100021) failed with 3
 * @vm_kern.c:178" (no room in the kernel map for one piece that size; ml_io_map panics rather than fail). Step 1 needs
 * the firmware (1.5 MiB), its run-time area (header +36: 0x233df0) and the panic record inside it, so only the first
 * 4 MiB are mapped and cleared; the rest of the window is the core's to use later (step 2 places mxconf there).
 */
#define SHMEM_MAP       0x400000UL

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

/*
 * Step 2: the configuration the firmware reads at ISSR1 (wifi2: without it the core wrote 0x22220004 into ISSR0 and
 * went quiet). Layout of Samsung's mxconf.h (not packed: natural alignment; every *_loc an offset in the shared memory
 * as the core's DRAM window sees it). Streams as mxman.c transports_init makes them, in its order, so the interrupt
 * bits come out the same: bit 0 of the AP->R4 register is reserved (forced panic). "to AP" streams are the firmware's
 * to write (their buffers start as 0xff), "from AP" ours.
 */
struct mx_cbuf {
	uint32_t buffer_loc, num_packets, packet_size, read_index_loc, write_index_loc;
};
struct mx_stream {
	struct mx_cbuf buf;
	uint8_t read_bit, write_bit, pad[2];
};
struct mx_trans {
	struct mx_stream to_ap, from_ap;
};
struct mx_conf {
	uint32_t magic;                         /* 0x79828486 */
	uint16_t major, minor;                  /* 0.5 */
	struct mx_trans mgmt, gdb_r4, gdb_m4;
	struct mx_stream mxlog;
	uint32_t soc_revision, flags;
	uint32_t fwconfig_offset, fwconfig_size;
	struct mx_trans gdb_m4_1;               /* unused: from_ap buffer 0 */
};
_Static_assert(sizeof(struct mx_conf) == 240, "mxconf layout");

static uint32_t heap;                           /* next free offset in the shared memory */

static uint32_t
halloc(uint32_t bytes)
{
	uint32_t at = (heap + 63) & ~63u;
	heap = at + bytes;
	return at;
}

static void
make_stream(struct mx_stream *s, uint32_t length, uint32_t packet, bool to_ap, uint8_t rbit, uint8_t wbit)
{
	s->buf.buffer_loc = halloc(length);
	s->buf.num_packets = length / packet;
	s->buf.packet_size = packet;
	s->buf.read_index_loc = halloc(4);
	s->buf.write_index_loc = halloc(4);
	memset(shm + s->buf.buffer_loc, to_ap ? 0xff : 0, length);
	*(volatile uint32_t *)(shm + s->buf.read_index_loc) = 0;
	*(volatile uint32_t *)(shm + s->buf.write_index_loc) = 0;
	s->read_bit = rbit;
	s->write_bit = wbit;
}

static struct mx_conf *conf;
static uint32_t conf_off;
static uint32_t hb_power, hb_cfg, hb_msgs, hb_first[4], hb_last[3], hb_sent, hb_cfm;
static bool started;                    /* MM_START_IND seen */
static uint64_t hb_smc;
static uint32_t hb_panic_off;

static void
make_conf(uint32_t runtime)
{
	heap = (runtime + 0xfff) & ~0xfffu;
	conf_off = halloc(sizeof(struct mx_conf));
	conf = (struct mx_conf *)(shm + conf_off);
	bzero(conf, sizeof(*conf));
	conf->magic = 0x79828486;
	conf->major = 0;
	conf->minor = 5;
	/* bits: AP->R4 bit 0 reserved; to-AP bits from 0 up, AP->R4 from 1, AP->M4 from 1 (0 reserved) */
	make_stream(&conf->mgmt.to_ap, 512, 128, true, 1, 0);           /* in:  read AP->R4 1, write to-AP 0 */
	make_stream(&conf->mgmt.from_ap, 512, 128, false, 1, 2);        /* out: read to-AP 1, write AP->R4 2 */
	make_stream(&conf->gdb_r4.to_ap, 2048, 4, true, 3, 2);
	make_stream(&conf->gdb_r4.from_ap, 2048, 4, false, 3, 0);
	make_stream(&conf->gdb_m4.to_ap, 2048, 4, true, 1, 4);
	make_stream(&conf->gdb_m4.from_ap, 2048, 4, false, 5, 0);
	make_stream(&conf->mxlog, 16384, 4, true, 4, 6);
	printf("XC5 WLBT: mxconf at %#x (240 bytes), mgmt to-AP %#x from-AP %#x, mxlog %#x, heap end %#x\n", conf_off,
	    conf->mgmt.to_ap.buf.buffer_loc, conf->mgmt.from_ap.buf.buffer_loc, conf->mxlog.buf.buffer_loc, heap);
}

/* Messages the firmware put in the management stream (mxmgmt_transport_format.h: channel byte + 127 of payload). */
static void
read_mgmt(void)
{
	struct mx_cbuf *b = &conf->mgmt.to_ap.buf;
	volatile uint32_t *ri = (volatile uint32_t *)(shm + b->read_index_loc);
	volatile uint32_t *wi = (volatile uint32_t *)(shm + b->write_index_loc);
	while (*ri != *wi) {
		uint8_t *p = shm + b->buffer_loc + (*ri % b->num_packets) * b->packet_size;
		if (hb_msgs == 0) {
			memcpy(hb_first, p, sizeof(hb_first));
		}
		memcpy(hb_last, p, sizeof(hb_last));
		hb_msgs++;
		if (p[0] == 0 && p[1] == 0) {
			started = true;
		}
		if (p[0] == 1 && p[2] == 1 && p[1] < 8) {
			hb_cfm |= 1u << p[1];          /* SM_MSG_START_CFM for that service */
		}
		printf("XC5 WLBT: mgmt message: channel %u, %02x %02x %02x %02x %02x %02x %02x %02x%s\n", p[0], p[1], p[2],
		    p[3], p[4], p[5], p[6], p[7], p[8], p[0] == 0 && p[1] == 0 ? " = MM_START_IND" : "");
		*ri = (*ri + 1) % b->num_packets;
		wr(mbox, 0x01c, 1u << conf->mgmt.to_ap.read_bit);      /* INTGR1: told the core it was read */
	}
}

/*
 * Step 3a: one message to the firmware on the management transport (mxmgmt_transport_send: the channel byte, then the
 * payload, in one 128-byte packet of the from-AP ring; then the ring's write bit to the R4 through INTGR1).
 */
static void
send_mgmt(uint8_t channel, const void *payload, uint32_t len)
{
	struct mx_cbuf *b = &conf->mgmt.from_ap.buf;
	volatile uint32_t *ri = (volatile uint32_t *)(shm + b->read_index_loc);
	volatile uint32_t *wi = (volatile uint32_t *)(shm + b->write_index_loc);
	uint32_t next = (*wi + 1) % b->num_packets;
	if (next == *ri) {
		printf("XC5 WLBT: management ring to the firmware is full\n");
		return;
	}
	uint8_t *p = shm + b->buffer_loc + *wi * b->packet_size;
	bzero(p, b->packet_size);
	p[0] = channel;
	memcpy(p + 1, payload, MIN(len, b->packet_size - 1));
	__asm__ volatile ("dsb sy" ::: "memory");
	*wi = next;
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(mbox, 0x01c, 1u << conf->mgmt.from_ap.write_bit);   /* INTGR1 */
	hb_sent++;
}

/*
 * The service manager's start request (servman_messages.h: struct sm_msg_packet { u8 service_id; u8 msg; s32
 * optional_data; } packed; SM_MSG_START_REQ 0, START_CFM 1) on channel 1 (MMTRANS_CHAN_ID_SERVICE_MANAGEMENT).
 * Service 0 is the NULL service, which Samsung's own test client starts with no configuration (client_test.c):
 * the answer, SM_MSG_START_CFM, proves the way to the firmware's service manager before WLAN (1) needs its HIP4.
 */
static void
start_service(uint8_t id, uint32_t ref)
{
	uint8_t m[6] = { id, 0, (uint8_t)ref, (uint8_t)(ref >> 8), (uint8_t)(ref >> 16), (uint8_t)(ref >> 24) };
	send_mgmt(1, m, sizeof(m));
	printf("XC5 WLBT: SM_MSG_START_REQ for service %u (configuration %#x) sent\n", id, ref);
}

/*
 * Step 3b: the WLAN service's HIP4 (hip4.h / hip4_init, cm_if.c slsi_sm_wlan_service_open): 2.5 MiB of the shared
 * memory, 4 KiB aligned -- here right after mxconf, at 0x240000 (see HIP_OFF) --
 * laid out as CONFIG 8 KiB (the hip4_hip_control), MIB 32 KiB (the stock wlan.hcf files back to back), TX data 1 MiB,
 * TX control 64 KiB, RX 1 MiB. The control (packed parts, Samsung's alignments): init {magic 0xcaaa0400,
 * conf_hip4_ver (the firmware's), refs to config A (v4) and B (v5)} at 0, config v5 (120 bytes) at 32, config v4
 * (227) at 160, scoreboard u32[256] at 448, six queues of {u32[256], 3 index bytes} each 1088 bytes from 1472.
 * Every location is an offset in the shared memory. Then SM_MSG_START_REQ for service 1 with the control's offset.
 */
/*
 * wifi6 (video 203333): a second 4 MiB ml_io_map_wcomb panicked like wifi1 ("kmem_alloc(..., 4194304, ...) failed
 * with 3"). So the HIP sits in the 4 MiB already mapped, after mxconf: control, MIB and TX (0x240000-0x35a000) are
 * ours to reach; RX (0x35a000-0x45a000) is the firmware's to write and runs past the map, read later through a small
 * map of its own when there is something in it.
 */
#define HIP_OFF         0x240000u       /* in the shared memory */
#define HIP_MIB         0x2000u
#define HIP_MIB_SZ      0x8000u
#define HIP_TX          (HIP_MIB + HIP_MIB_SZ)
#define HIP_TX_SZ       (0x100000u + 0x10000u)
#define HIP_RX          (HIP_TX + HIP_TX_SZ)
#define HIP_RX_SZ       0x100000u
#define HIP_V5          32u
#define HIP_V4          160u
#define HIP_SCBRD       448u
#define HIP_Q           1472u
#define HIP_QSZ         1088u
#define HIP_TH_BIT      7u              /* to-AP: the next free one after the transports' 0-6 */
#define HIP_FH_BIT      5u              /* AP->R4: the next free one after the transports' */

struct hip_cfg_head {                   /* the part both versions share, 64 bytes */
	uint32_t magic;
	uint16_t ver, len;
	uint32_t compat_flag;           /* firmware's */
	uint16_t sap_mlme, sap_ma, sap_debug, sap_test;
	uint32_t fw_build, fw_patch;
	uint8_t headroom, tailroom, align, host_cache_line;
	uint32_t host_buf_loc, host_buf_sz, fw_buf_loc, fw_buf_sz, mib_loc, mib_sz, log_loc, log_sz;
} __attribute__((packed));
_Static_assert(sizeof(struct hip_cfg_head) == 64, "hip config head");
struct hip_cfg_v4 {
	struct hip_cfg_head h;
	uint8_t fh_int, r1[3], th_int[6], r2[2];
	uint32_t scbrd_loc;
	uint16_t q_num, q_len, q_idx_sz;
	uint8_t r3[2];
	uint32_t q_loc[6];
	uint8_t r4[99 + 16];
} __attribute__((packed));
_Static_assert(sizeof(struct hip_cfg_v4) == 227, "hip config v4");
struct hip_cfg_v5 {
	struct hip_cfg_head h;
	uint8_t fh_int, th_int, r1[2];
	uint32_t scbrd_loc;
	uint16_t q_num, q_len, q_idx_sz;
	uint8_t r2[2];
	uint32_t q_loc[6];
	uint8_t r3[16];
} __attribute__((packed));
_Static_assert(sizeof(struct hip_cfg_v5) == 120, "hip config v5");
struct hip_init {
	uint32_t magic, conf_ver, a_ref, b_ref;
};

static uint8_t *hip;                    /* kernel address of HIP_OFF */
static uint32_t hip_mib;
static int wlan_mode = 1;               /* xc5wlan: 1 start WLAN, 0 the NULL service (step 3a) */

static void
head_fill(struct hip_cfg_head *h, uint16_t ver, uint16_t len)
{
	h->magic = 0xcaba0401;
	h->ver = ver;
	h->len = len;
	h->host_cache_line = 64;
	h->host_buf_loc = HIP_OFF + HIP_TX;
	h->host_buf_sz = HIP_TX_SZ;
	h->fw_buf_loc = HIP_OFF + HIP_RX;
	h->fw_buf_sz = HIP_RX_SZ;
	h->mib_loc = hip_mib ? HIP_OFF + HIP_MIB : 0;
	h->mib_sz = hip_mib;
}

/*
 * What the Samsung driver gives the firmware of an .hcf file is not the file (mgt.c slsi_mib_open_file and
 * slsi_mib_get_platform): an 8-byte header (byte 7 = 1, the format) is dropped, then, when the payload starts with the
 * keys 0xFFFE (version, length 1, value 1) and 0xFFFF (platform name, a vldata octet string), those are dropped too.
 * wifi8 (video 210843): with the whole files the firmware logged "Received SM_MSG_START_REQ for service: 1" and then
 * nothing more, and no START_CFM. In place at p; returns the length left (0 if the header is not there).
 */
static uint32_t
hcf_payload(uint8_t *p, uint32_t len)
{
	if (len < 8 || p[7] != 1) {
		return 0;
	}
	uint32_t pos = 8;
	if (len >= pos + 12 && p[pos] == 0xFE && p[pos + 1] == 0xFF && p[pos + 2] == 1 && p[pos + 3] == 0 &&
	    p[pos + 4] == 1 && p[pos + 5] == 0 && p[pos + 6] == 0xFF && p[pos + 7] == 0xFF) {
		uint32_t q = pos + 8;
		uint32_t nlen = p[q] | ((uint32_t)p[q + 1] << 16);   /* as the driver reads it */
		q += 2;
		uint8_t vh = p[q++];
		uint32_t vl = vh & 0x17;
		if ((vh & 0xA0) != 0 && vl <= 2 && nlen >= 2 + vl && q + nlen <= len) {
			nlen -= 1 + vl;
			q += vl;
			nlen += nlen & 1;
			pos = q + nlen;
		}
	}
	memmove(p, p + pos, len - pos);
	return len - pos;
}

static bool
make_hip(void)
{
	if (heap > HIP_OFF) {
		printf("XC5 WLBT: mxconf heap %#x runs into the HIP at %#x: WLAN not started\n", heap, HIP_OFF);
		return false;
	}
	hip = shm + HIP_OFF;
	bzero(hip, SHMEM_MAP - HIP_OFF);
	/*
	 * the MIB: the stock files the Samsung driver loads (wlan.hcf, wlan_sw.hcf as mx140_*), whole, back to back.
	 * wifi7 (video 204718): mib=0 on the boot that installed them -- they came with the drop after this ran; so wait.
	 */
	int64_t a = -1;
	for (int s = 0; s < 90 && a <= 0; s++) {
		a = read_file("/private/var/xc5/wifi/wlan.hcf", hip + HIP_MIB, HIP_MIB_SZ);
		if (a <= 0) {
			delay(1000000);
		}
	}
	uint32_t pa = a > 0 ? hcf_payload(hip + HIP_MIB, (uint32_t)a) : 0;
	int64_t b = pa > 0 ? read_file("/private/var/xc5/wifi/wlan_sw.hcf", hip + HIP_MIB + pa, HIP_MIB_SZ - pa) : -1;
	uint32_t pb = b > 0 ? hcf_payload(hip + HIP_MIB + pa, (uint32_t)b) : 0;
	hip_mib = pa + pb;
	bzero(hip + HIP_MIB + hip_mib, HIP_MIB_SZ - hip_mib);
	printf("XC5 WLBT: MIB payloads %u of %lld + %u of %lld bytes\n", pa, a, pb, b);

	struct hip_cfg_v5 *v5 = (struct hip_cfg_v5 *)(hip + HIP_V5);
	struct hip_cfg_v4 *v4 = (struct hip_cfg_v4 *)(hip + HIP_V4);
	head_fill(&v5->h, 5, sizeof(*v5));
	head_fill(&v4->h, 4, sizeof(*v4));
	v5->fh_int = v4->fh_int = HIP_FH_BIT;
	v5->th_int = HIP_TH_BIT;
	for (int i = 0; i < 6; i++) {
		v4->th_int[i] = HIP_TH_BIT;
		v4->q_loc[i] = v5->q_loc[i] = HIP_OFF + HIP_Q + (uint32_t)i * HIP_QSZ;
	}
	v4->scbrd_loc = v5->scbrd_loc = HIP_OFF + HIP_SCBRD;
	v4->q_num = v5->q_num = 6;
	v4->q_len = v5->q_len = 256;
	v4->q_idx_sz = v5->q_idx_sz = 1;
	struct hip_init *in = (struct hip_init *)hip;
	in->a_ref = HIP_OFF + HIP_V4;
	in->b_ref = HIP_OFF + HIP_V5;
	__asm__ volatile ("dsb sy" ::: "memory");
	in->magic = 0xcaaa0400;
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(mbox, 0x010, rd(mbox, 0x010) & ~(1u << (16 + HIP_TH_BIT)));        /* INTMR0: the HIP's to-AP bit */
	printf("XC5 WLBT: HIP4 control at %#x (MIB %u bytes: %lld + %lld), TX %#x RX %#x, bits to-AP %u AP->R4 %u\n",
	    HIP_OFF, hip_mib, a, b, HIP_OFF + HIP_TX, HIP_OFF + HIP_RX, HIP_TH_BIT, HIP_FH_BIT);
	return true;
}

/*
 * The firmware's log (mxlog.c, mxlog_transport.c; facts only): the mxlog stream is a ring of 4-byte packets. A record is
 * a header word (sync in [31:16]: 0x9669 phase 5, 0xA55A phase 4; level [15:8]; length [7:0], in words for phase 5,
 * in characters for phase 4) and its payload. Phase 5: timestamp, the offset of the format string in log-strings.bin
 * (the firmware's debug file of the same build: vendor/etc/wifi/mx140/debug/common/log-strings.bin, put in the drop as
 * wifi/log-strings.bin), then 32-bit arguments; a %s argument is itself an offset in that file.
 * wifi7: after START_REQ for WLAN the firmware filled its HIP4 fields (version 4) but sent no START_CFM, and its log
 * stopped growing (write index 194). The decoded lines go to the console and the last ones into the heartbeat.
 */
#define LOGN 5
static char *ls;                        /* log-strings.bin */
static uint32_t ls_sz, log_recs, log_bad;
static char loglines[LOGN][112];

static bool
is_flag(char c)
{
	return c == '-' || c == '+' || c == ' ' || c == '#' || c == '.' || (c >= '0' && c <= '9');
}

static void
fw_format(char *out, size_t outsz, const char *fmt, const uint32_t *args, uint32_t nargs)
{
	size_t o = 0;
	uint32_t ai = 0;
	while (*fmt != 0 && o + 1 < outsz) {
		if (*fmt != '%') {
			char c = *fmt++;
			out[o++] = (c == '\n' || c == '\r') ? ' ' : c;
			continue;
		}
		if (fmt[1] == '%') {
			out[o++] = '%';
			fmt += 2;
			continue;
		}
		/* flags and width are dropped (a format from the firmware's file is not given to snprintf as a format) */
		fmt++;
		while (is_flag(*fmt) || *fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
			fmt++;
		}
		char c = *fmt;
		if (c == 0) {
			break;
		}
		fmt++;
		uint32_t a = ai < nargs ? args[ai] : 0;
		ai++;
		int n;
		if (c == 's') {
			n = snprintf(out + o, outsz - o, "%s", a < ls_sz ? ls + a : "<?>");
		} else if (c == 'd' || c == 'i') {
			n = snprintf(out + o, outsz - o, "%d", (int)a);
		} else if (c == 'c') {
			n = snprintf(out + o, outsz - o, "%c", (int)(a & 0x7f));
		} else if (c == 'u') {
			n = snprintf(out + o, outsz - o, "%u", a);
		} else {
			n = snprintf(out + o, outsz - o, "%x", a);
		}
		if (n < 0) {
			break;
		}
		o += MIN((size_t)n, outsz - o - 1);
	}
	out[o] = 0;
}

static void
read_log(void)
{
	static uint32_t words[256];
	static char line[240];
	struct mx_cbuf *b = &conf->mxlog.buf;
	volatile uint32_t *ri = (volatile uint32_t *)(shm + b->read_index_loc);
	volatile uint32_t *wi = (volatile uint32_t *)(shm + b->write_index_loc);
	volatile uint32_t *ring = (volatile uint32_t *)(shm + b->buffer_loc);
	uint32_t n = b->num_packets;
	for (int guard = 0; guard < 256; guard++) {
		uint32_t r = *ri % n, w = *wi % n, avail = (w + n - r) % n;
		if (avail == 0) {
			break;
		}
		uint32_t hdr = ring[r], bytes;
		if ((hdr >> 16) == 0x9669) {
			bytes = (hdr & 0xff) * 4;
		} else if ((hdr >> 16) == 0xA55A) {
			bytes = hdr & 0xff;
		} else {
			*ri = (r + 1) % n;
			log_bad++;
			continue;
		}
		uint32_t pk = (bytes + 3) / 4;
		if (avail < 1 + pk) {
			break;
		}
		for (uint32_t i = 0; i < pk; i++) {
			words[i] = ring[(r + 1 + i) % n];
		}
		*ri = (r + 1 + pk) % n;
		log_recs++;
		if ((hdr >> 16) == 0xA55A) {
			size_t m = MIN((size_t)bytes, sizeof(line) - 1);
			memcpy(line, words, m);
			line[m] = 0;
			snprintf(loglines[log_recs % LOGN], sizeof(loglines[0]), "%s", line);
		} else if (pk >= 2) {
			const char *fmt = (ls != NULL && words[1] < ls_sz) ? ls + words[1] : NULL;
			if (fmt != NULL) {
				fw_format(line, sizeof(line), fmt, words + 2, pk - 2);
			} else {
				snprintf(line, sizeof(line), "fmt@%#x %x %x %x", words[1], pk > 2 ? words[2] : 0,
				    pk > 3 ? words[3] : 0, pk > 4 ? words[4] : 0);
			}
			snprintf(loglines[log_recs % LOGN], sizeof(loglines[0]), "%x %s", words[0], line);
		}
		printf("XC5 FW %u: %s\n", log_recs, loglines[log_recs % LOGN]);
	}
}

static void
load_ls(void)
{
	if (ls_sz != 0) {
		return;
	}
	if (ls == NULL) {
		ls = kalloc_data(1u << 20, Z_WAITOK);
		if (ls == NULL) {
			return;
		}
	}
	int64_t n = read_file("/private/var/xc5/wifi/log-strings.bin", (uint8_t *)ls, (1u << 20) - 1);
	if (n > 0) {
		ls[n] = 0;
		ls_sz = (uint32_t)n;
		printf("XC5 WLBT: log-strings.bin %u bytes: %.60s\n", ls_sz, ls);
	}
}

/* the log is read once its strings are there, or after 60 s without them (raw offsets then) */
static void
poll_log(int tenths)
{
	if (ls_sz == 0 && tenths % 20 == 0) {
		load_ls();
	}
	if (ls_sz != 0 || tenths >= 600) {
		read_log();
	}
}

static void
wlbt_thread(__unused void *arg, __unused wait_result_t w)
{
	/* the firmware comes with the drop, which is written once launchd remounts / read-write */
	int64_t fwlen = -1;
	shm = (uint8_t *)ml_io_map_wcomb(SHMEM_PA, SHMEM_MAP);
	for (int s = 0; s < 240 && fwlen < 0; s++) {
		fwlen = read_file("/private/var/xc5/wifi/mx140.bin", shm, SHMEM_MAP);
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
	hb_panic_off = (panic_off != 0 && panic_off + 64 < SHMEM_MAP) ? panic_off : 0;
	xcover5_wlbt_state[2] = entry;
	printf("XC5 WLBT: firmware %lld bytes, header v%u.%u, api %u.%u, entry %#x, runtime %#x, panic record at %#x, "
	    "build %.40s\n", fwlen, *(uint16_t *)(shm + 14), *(uint16_t *)(shm + 12), *(uint16_t *)(shm + 22),
	    *(uint16_t *)(shm + 20), entry, runtime, panic_off, (const char *)(shm + 48));
	(void)hdr_len;
	bzero(shm + fwlen, SHMEM_MAP - (size_t)fwlen);

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

	/* the configuration (step 2), then the start mailboxes, before the core runs (mxman.c mbox_init) */
	make_conf(runtime);
	wr(mbox, 0x010, rd(mbox, 0x010) & ~(0x7fu << 16));     /* INTMR0: the to-AP bits 0-6 unmasked */
	wr(mbox, ISSR(0), entry);
	wr(mbox, ISSR(1), conf_off);
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
	hb_power = v & 1;
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
	hb_cfg = cfg;
	report(cfg ? "CFG_REQ" : "no CFG_REQ in 1 s");
	uint64_t smc = smc4(0x82000710, 1ull << 32, 0, 0);
	printf("XC5 WLBT: SMC CONN_IF set TZPC: %#llx\n", smc);
	hb_smc = smc;
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
	bool asked = false;
	for (int t = 0; t < 200; t++) {
		read_mgmt();
		if (started && !asked) {
			asked = true;
			if (wlan_mode != 0 && make_hip()) {
				start_service(1, HIP_OFF);      /* WLAN, with its HIP4 control */
			} else {
				start_service(0, 0);            /* the NULL service */
			}
		}
		poll_log(t);

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
	/* then the log and the management ring every 0.5 s for good; the outcome said again every 20 s for 3 minutes */
	for (int i = 0, tick = 0;; tick++) {
		read_mgmt();
		poll_log(200 + tick * 5);
		if (tick % 40 != 0 || i >= 9) {
			delay(500000);
			continue;
		}
		i++;
		uint32_t *p = (uint32_t *)(shm + ((panic_off != 0 && panic_off + 64 < SHMEM_MAP) ? panic_off : 0));
		printf("XC5 WLBT summary: fw %lld B entry %#x | power %d CFG_REQ %d SMC %#llx | STATUS %#x STATES %#x | ISSR0-3 "
		    "%#x %#x %#x %#x | INTSR0 %#x INTMSR0 %#x INTSR1 %#x | mgmt to-AP r%u w%u, mxlog w%u | panic %08x %08x %08x\n",
		    fwlen, entry, (int)(v & 1), (int)cfg, smc, rd(pmu, WLBT_STATUS), rd(pmu, WLBT_STATES), rd(mbox, ISSR(0)),
		    rd(mbox, ISSR(1)), rd(mbox, ISSR(2)), rd(mbox, ISSR(3)), rd(mbox, INTSR0), rd(mbox, INTMSR0),
		    rd(mbox, INTSR1), *(volatile uint32_t *)(shm + conf->mgmt.to_ap.buf.read_index_loc),
		    *(volatile uint32_t *)(shm + conf->mgmt.to_ap.buf.write_index_loc),
		    *(volatile uint32_t *)(shm + conf->mxlog.buf.write_index_loc), p[0], p[1], p[2]);
		delay(500000);
	}
}

/*
 * wifi3 (video 194332): the core raised to-AP bits 0 (management) and 6 (log) -- INTSR0 0x410000 -- and ISSR0 became
 * 0x2222801b, but no other line of this file could be found on the screen: the console scrolls too fast. So one line in
 * the heartbeat block (SweetPlatformExpert), which is drawn where it can always be read.
 */
void xc5_wlbt_heartbeat(void);
void
xc5_wlbt_heartbeat(void)
{
	if (mbox == NULL || conf == NULL) {
		return;
	}
	read_mgmt();
	if (hip != NULL) {
		struct hip_init *in = (struct hip_init *)hip;
		struct hip_cfg_v5 *v5 = (struct hip_cfg_v5 *)(hip + HIP_V5);
		struct hip_cfg_v4 *v4 = (struct hip_cfg_v4 *)(hip + HIP_V4);
		struct hip_cfg_head *h = in->conf_ver == 4 ? &v4->h : &v5->h;
		printf("XC5 hip mib=%u cfm=%#x ver=%u mlme=%#x build=%#x scb=%08x q0 %u/%u q3 %u/%u ls=%u log %u bad %u\n",
		    hip_mib, hb_cfm, in->conf_ver, h->sap_mlme, h->fw_build, *(volatile uint32_t *)(hip + HIP_SCBRD),
		    hip[HIP_Q + 1024], hip[HIP_Q + 1025], hip[HIP_Q + 3 * HIP_QSZ + 1024], hip[HIP_Q + 3 * HIP_QSZ + 1025], ls_sz,
		    log_recs, log_bad);
	}
	/* the firmware's last log lines, oldest first */
	for (uint32_t k = log_recs >= LOGN ? log_recs - LOGN + 1 : 1; k != 0 && k <= log_recs; k++) {
		printf("XC5 fw%u %s\n", k, loglines[k % LOGN]);
	}
	printf("XC5 wlbt pwr=%u cfg=%u smc=%#llx st=%#x issr0=%#x sr0=%#x mgmt r%u w%u msgs=%u first=%08x last=%08x %08x "
	    "sent=%u out r%u w%u log w%u\n", hb_power, hb_cfg, hb_smc, rd(pmu, WLBT_STATES), rd(mbox, ISSR(0)),
	    rd(mbox, INTSR0), *(volatile uint32_t *)(shm + conf->mgmt.to_ap.buf.read_index_loc),
	    *(volatile uint32_t *)(shm + conf->mgmt.to_ap.buf.write_index_loc), hb_msgs, hb_first[0], hb_last[0],
	    hb_last[1], hb_sent, *(volatile uint32_t *)(shm + conf->mgmt.from_ap.buf.read_index_loc),
	    *(volatile uint32_t *)(shm + conf->mgmt.from_ap.buf.write_index_loc),
	    *(volatile uint32_t *)(shm + conf->mxlog.buf.write_index_loc));
}

void xc5_wlbt_start(void);
void
xc5_wlbt_start(void)
{
	int on = 0;
	if (!PE_parse_boot_argn("xc5wifi", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("xc5wlan", &wlan_mode, sizeof(wlan_mode));
	thread_t thread;
	if (kernel_thread_start(wlbt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
