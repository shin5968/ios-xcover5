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
#include <kern/locks.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <sys/sysctl.h>
#include <sys/kern_event.h>
#include <net/if_media.h>
#include <sys/redwood_sta.h>
#include <sys/redwood_net.h>

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
 * ours to reach; RX (0x35a000-0x3da000, 512 KiB since step 3c) too.
 */
#define HIP_OFF         0x240000u       /* in the shared memory */
#define HIP_MIB         0x2000u
#define HIP_MIB_SZ      0x8000u
#define HIP_TX          (HIP_MIB + HIP_MIB_SZ)
#define HIP_TX_SZ       (0x100000u + 0x10000u)
#define HIP_RX          (HIP_TX + HIP_TX_SZ)
#define HIP_RX_SZ       0x80000u        /* the 512 KiB the driver also uses: 0x35a000-0x3da000, inside the map */
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

/*
 * Step 3c: the first FAPI signal over the HIP4 (hip4.c hip4_q_add_signal / hip4_skb_to_mbulk / hip4_mbulk_to_skb,
 * mbulk_def.h, utils.h fapi_alloc_f, fapi.h, mib.c; facts only).
 *   Queues: 0 FH ctrl, 1 FH data, 2 FH RFB (the firmware gives our buffers back), 3 TH ctrl, 4 TH data, 5 TH RFB (we
 *   give its buffers back). Each one's write and read index is a byte in the scoreboard: q_idx_layout, the side that
 *   the firmware owns at +64. An entry is the shared-memory offset of an mbulk; after it is put in, the index moves
 *   and the AP->R4 bit (HIP_FH_BIT) is raised.
 *   mbulk: 20-byte header {u32 next; u8 flag (1 signal, 4 owned by the other side), class (5 from host ctrl), pool
 *   (1 ctrl), refcnt; u16 dat_bufsz, sig_bufsz, len, head; u32 chain}, then the signal buffer (4 bytes for the
 *   firmware's own header, then the FAPI signal), then the data at +20+head (after the headroom the firmware asked for).
 *   MLME-GET.request 0x2001: {u16 id, receiver 0, sender 0xC001; u32 fw_reference; u16 vif 0; 12 spare} and as data
 *   the MIB keys asked for ({u16 psid, u16 0} each): what Samsung's driver asks first (slsi_mib_initial_get) --
 *   unifiChipVersion 0x07E6, unifiHardwarePlatform 0x0941, unifiFirmwareBuildId 0x07E5. (The MAC address is not the
 *   firmware's: Samsung's driver reads it from a file.) The answer, MLME-GET.confirm 0x2101, comes on queue 3.
 */
static const uint8_t q_layout[6][2] = {         /* [queue][write, read] */
	{ 0, 64 }, { 1, 65 }, { 66, 2 }, { 67, 3 }, { 68, 4 }, { 5, 69 }
};
static uint32_t mlme_sent, mlme_got, fh_rfb, tx_slot;
static uint16_t mlme_ids[8];
static bool mlme_asked;
/* what came back: MLME-GET values, ADD_VIF / ADD_SCAN results, scan results */
static uint32_t mib_ht = 0xff, mib_vht = 0xff, mib_rates[3];
static uint8_t conn_data[48];
static uint32_t conn_len;
/* wifi14: the AP answered the association with status 18. What the firmware asked with (MLME-PROCEDURE-STARTED.ind
 * carries our association request), and a second try with rate elements given (MLME-ADD-INFO-ELEMENTS) */
static uint8_t req_ids[12], req_rates[12], n_req_ids, n_req_rates;
static uint32_t req_len, conn_try, info_res = 0xffff;
static int rate_retry;
static uint32_t mib_chip, mib_plat, mib_build, vif_res = 0xffff, scan_res = 0xffff, scan_inds, scan_done;
#define APN 24
static struct {
	char ssid[33];
	uint8_t ssid_len;
	uint8_t raw_ssid[32];
	uint8_t bssid[6];
	int16_t rssi;
	uint16_t mhz;
	bool open;
	uint8_t rates[16];                      /* Supported Rates then Extended Supported Rates */
	uint8_t nrates;
	uint8_t caps;                           /* 1 HT, 2 VHT, 4 HE (extension 35) present */
	uint16_t capability, beacon_interval;   /* (step 4e: what wifid is given of a network, with its elements) */
	uint16_t elements_length;
	uint8_t elements[REDWOOD_STA_ELEMENTS_MOST];
} aps[APN];
static uint32_t ap_count;
static const uint8_t vif_mac[6] = { 0x02, 0x58, 0x43, 0x35, 0x00, 0x01 };   /* locally administered: "XC5" */

static inline volatile uint8_t *
scb(int q, int rw)
{
	return (volatile uint8_t *)(hip + HIP_SCBRD + q_layout[q][rw]);
}

static inline volatile uint32_t *
qarr(int q)
{
	return (volatile uint32_t *)(hip + HIP_Q + (uint32_t)q * HIP_QSZ);
}

static bool
q_put(int q, uint32_t ref)
{
	uint8_t w = *scb(q, 0), r = *scb(q, 1);
	if ((uint8_t)(w + 1) == r) {
		return false;
	}
	qarr(q)[w] = ref;
	__asm__ volatile ("dsb sy" ::: "memory");
	*scb(q, 0) = (uint8_t)(w + 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(mbox, 0x01c, 1u << HIP_FH_BIT);      /* INTGR1 */
	return true;
}

static struct hip_cfg_head *
fw_cfg(void)
{
	struct hip_init *in = (struct hip_init *)hip;
	return in->conf_ver == 4 ? &((struct hip_cfg_v4 *)(hip + HIP_V4))->h : &((struct hip_cfg_v5 *)(hip + HIP_V5))->h;
}

/*
 * One FAPI signal (sig: the signal from its id on, siglen bytes) with data, as hip4_skb_to_mbulk makes it, in the next
 * of 32 slots of 2 KiB of the TX control area, into the FH control queue.
 */
static void
send_sig(const uint8_t *sigp, uint32_t siglen, const uint8_t *data, uint32_t dlen)
{
	struct hip_cfg_head *h = fw_cfg();
	uint32_t moff = HIP_TX + 0x100000 + (tx_slot++ % 32) * 2048;
	uint8_t *m = hip + moff;
	uint16_t sigbuf = (uint16_t)(((20 + siglen + 4 + 3) & ~3u) - 20);
	uint16_t head = (uint16_t)(sigbuf + (dlen ? h->headroom : 0));
	bzero(m, 2048);
	m[4] = 1 | 4;                           /* signal, owned by the firmware now */
	m[5] = 5;                               /* MBULK_CLASS_FROM_HOST_CTL */
	m[6] = 1;                               /* MBULK_POOL_ID_CTRL */
	m[7] = dlen ? 1 : 0;
	*(uint16_t *)(m + 8) = (uint16_t)(dlen ? dlen + h->headroom + h->tailroom : 0);
	*(uint16_t *)(m + 10) = sigbuf;
	*(uint16_t *)(m + 12) = (uint16_t)dlen;
	*(uint16_t *)(m + 14) = head;
	memcpy(m + 20 + 4, sigp, siglen);
	*(uint16_t *)(m + 20 + 4 + 4) = (uint16_t)(0xC001 + mlme_sent % 0x100);        /* sender process id */
	if (dlen) {
		memcpy(m + 20 + head, data, dlen);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (q_put(0, HIP_OFF + moff)) {
		mlme_sent++;
		printf("XC5 WLBT: signal %#x (%u + %u bytes) sent, mbulk %#x\n", *(const uint16_t *)sigp, siglen, dlen,
		    HIP_OFF + moff);
	}
}

static void
send_mlme_get(void)
{
	/* + unifiHtActivated 0x07FE, unifiVhtActivated 0x07FD, unifiSupportedDataRates 0x07F9 (wifi13: association refused) */
	static const uint8_t keys[] = { 0xE6, 0x07, 0, 0, 0x41, 0x09, 0, 0, 0xE5, 0x07, 0, 0, 0xFE, 0x07, 0, 0, 0xFD, 0x07, 0, 0,
		                        0xF9, 0x07, 0, 0 };
	uint8_t s[24] = { 0x01, 0x20 };         /* MLME_GET_REQ, vif 0 */
	send_sig(s, sizeof(s), keys, sizeof(keys));
}

/*
 * Step 3d: a station interface and a scan, as the driver does them (mlme.c slsi_mlme_add_vif, slsi_mlme_add_scan,
 * slsi_set_scan_params, slsi_mlme_append_channel_list; cfg80211_ops.c slsi_scan; facts only).
 *   MLME-ADD-VIF.request 0x2007 {header; vif 1 (SLSI_NET_INDEX_WLAN); interface_address; type 2 (station);
 *   device_address; 12 spare} -> MLME-ADD-VIF.confirm 0x2107 {header; vif; result_code (0 success)}.
 *   MLME-ADD-SCAN.request 0x2005 {header; vif 0; scan_id 1 << 8 (vif 1, SLSI_SCAN_HW_ID 0); scan_type 2 (full);
 *   device_address; report_mode 4 (real time); 12 spare}, data: an SSID element of length 0 (any network), the scan
 *   timing element (dd 11 00 16 32 01 01, zeros), the channel list element (dd len 00 16 32 01 02, then per channel
 *   {u16 2 x MHz, u8 policy 0}) for 2.4 GHz channels 1-13 -> MLME-ADD-SCAN.confirm 0x2105, then per network
 *   MLME-SCAN.indication 0x2300 {header; vif; channel_frequency (2 x MHz); rssi; ...} with the received beacon or
 *   probe response as data, and MLME-SCAN-DONE.indication 0x2301.
 */
static void
send_add_vif(void)
{
	uint8_t s[38] = { 0x07, 0x20 };
	s[10] = 1;                              /* vif */
	memcpy(s + 12, vif_mac, 6);
	s[18] = 2;                              /* FAPI_VIFTYPE_STATION */
	/*
	 * device_address: all ones for a station (cfg80211_ops.c slsi_connect; the real one only for P2P). wifi16: with
	 * our own address there the firmware's association request had OFDM rates only (no 1/2/5.5/11, as P2P does) and
	 * the hotspot, whose basic rates are those, refused it with status 18.
	 */
	memset(s + 20, 0xff, 6);
	send_sig(s, sizeof(s), NULL, 0);
}

static void
send_add_scan(void)
{
	uint8_t s[36] = { 0x05, 0x20 };
	s[12] = 0x00;
	s[13] = 0x01;                           /* scan_id 0x100 */
	s[14] = 2;                              /* FAPI_SCANTYPE_FULL_SCAN */
	memcpy(s + 16, vif_mac, 6);
	s[22] = 4;                              /* FAPI_REPORTMODE_REAL_TIME */
	uint8_t d[2 + 19 + 7 + 13 * 3];
	uint32_t n = 0;
	d[n++] = 0;                             /* SSID element, wildcard */
	d[n++] = 0;
	static const uint8_t timing[19] = { 0xdd, 0x11, 0x00, 0x16, 0x32, 0x01, 0x01 };
	memcpy(d + n, timing, sizeof(timing));
	n += sizeof(timing);
	static const uint8_t chl[7] = { 0xdd, 0x05 + 13 * 3, 0x00, 0x16, 0x32, 0x01, 0x02 };
	memcpy(d + n, chl, sizeof(chl));
	n += sizeof(chl);
	for (int ch = 1; ch <= 13; ch++) {
		uint16_t fw = (uint16_t)(2 * (2407 + 5 * ch));
		d[n++] = (uint8_t)fw;
		d[n++] = (uint8_t)(fw >> 8);
		d[n++] = 0;
	}
	send_sig(s, sizeof(s), d, n);
}

/* The data of an mbulk and of the ones chained to it, up to max bytes */
static uint32_t
gather(uint32_t ref, uint8_t *out, uint32_t max)
{
	uint32_t n = 0;
	for (int seg = 0; seg < 8 && ref >= HIP_OFF && ref + 20 < SHMEM_MAP && n < max; seg++) {
		uint8_t *m = shm + ref;
		uint16_t len = *(uint16_t *)(m + 12), head = *(uint16_t *)(m + 14);
		if (ref + 20u + head + len > SHMEM_MAP) {
			break;
		}
		uint32_t k = MIN((uint32_t)len, max - n);
		memcpy(out + n, m + 20 + head, k);
		n += k;
		if (!(m[4] & 0x80)) {           /* MBULK_F_CHAIN */
			break;
		}
		ref = *(uint32_t *)(m + 16);
	}
	return n;
}

/* MIB values of a GET confirm: {u16 psid, u16 length, value}..., a value 0x80|n then n bytes big-endian */
static void
mib_values(const uint8_t *d, uint32_t n)
{
	for (uint32_t p = 0; p + 4 <= n;) {
		uint16_t psid = (uint16_t)(d[p] | d[p + 1] << 8), l = (uint16_t)(d[p + 2] | d[p + 3] << 8);
		const uint8_t *v = d + p + 4;
		uint32_t val = 0;
		if (p + 4 + l <= n && l > 0) {
			if (v[0] & 0x80) {
				for (uint32_t i = 0; i < (uint32_t)(v[0] & 0x1f) && i + 1 < l; i++) {
					val = val << 8 | v[1 + i];
				}
			} else {
				val = v[0] & 0x7f;
			}
		}
		if (psid == 0x07E6) {
			mib_chip = val;
		} else if (psid == 0x0941) {
			mib_plat = val;
		} else if (psid == 0x07E5) {
			mib_build = val;
		} else if (psid == 0x07FE) {
			mib_ht = val;
		} else if (psid == 0x07FD) {
			mib_vht = val;
		} else if (psid == 0x07F9 && p + 4 + l <= n) {
			bzero(mib_rates, sizeof(mib_rates));
			memcpy(mib_rates, v, MIN((size_t)l, sizeof(mib_rates)));
		}
		p += 4 + l + (l & 1);
	}
}

static void
scan_result(const uint8_t *sig, const uint8_t *f, uint32_t n)
{
	scan_inds++;
	if (n < 36 + 2) {
		return;
	}
	char ssid[33] = "";
	uint8_t raw[32];
	uint32_t rawl = 0;
	uint8_t rates[16], nr = 0, caps = 0;
	for (uint32_t p = 36; p + 2 <= n && p + 2u + f[p + 1] <= n; p += 2u + f[p + 1]) {
		if (f[p] == 1 || f[p] == 50) {
			for (uint32_t i = 0; i < f[p + 1] && nr < sizeof(rates); i++) {
				rates[nr++] = f[p + 2 + i];
			}
		} else if (f[p] == 45) {
			caps |= 1;
		} else if (f[p] == 191) {
			caps |= 2;
		} else if (f[p] == 255 && f[p + 1] >= 1 && f[p + 2] == 35) {
			caps |= 4;
		}
	}
	for (uint32_t p = 36; p + 2 <= n; p += 2u + f[p + 1]) {         /* elements after the 24 + 12 fixed bytes */
		if (f[p] == 0 && p + 2u + f[p + 1] <= n) {
			uint32_t l = MIN((uint32_t)f[p + 1], 32u);
			for (uint32_t i = 0; i < l; i++) {
				ssid[i] = (f[p + 2 + i] >= 0x20 && f[p + 2 + i] < 0x7f) ? (char)f[p + 2 + i] : '?';
			}
			ssid[l] = 0;
			memcpy(raw, f + p + 2, l);
			rawl = l;
			break;
		}
	}
	const uint8_t *bssid = f + 16;
	uint32_t k;
	for (k = 0; k < ap_count && k < APN; k++) {
		if (memcmp(aps[k].bssid, bssid, 6) == 0) {
			break;
		}
	}
	if (k == ap_count) {
		if (ap_count >= APN) {
			return;
		}
		ap_count++;
	}
	snprintf(aps[k].ssid, sizeof(aps[k].ssid), "%s", ssid[0] ? ssid : "(hidden)");
	memcpy(aps[k].raw_ssid, raw, rawl);
	aps[k].ssid_len = (uint8_t)rawl;
	aps[k].open = !(f[34] & 0x10);          /* capability: privacy bit */
	aps[k].capability = (uint16_t)(f[34] | f[35] << 8);
	aps[k].beacon_interval = (uint16_t)(f[32] | f[33] << 8);
	aps[k].elements_length = (uint16_t)MIN(n - 36, (uint32_t)REDWOOD_STA_ELEMENTS_MOST);
	memcpy(aps[k].elements, f + 36, aps[k].elements_length);
	memcpy(aps[k].rates, rates, nr);
	aps[k].nrates = nr;
	aps[k].caps = caps;
	memcpy(aps[k].bssid, bssid, 6);
	aps[k].rssi = (int16_t)(sig[14] | sig[15] << 8);
	aps[k].mhz = (uint16_t)((sig[12] | sig[13] << 8) / 2);
	printf("XC5 WLBT: scan: %s %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm\n", aps[k].ssid, bssid[0], bssid[1],
	    bssid[2], bssid[3], bssid[4], bssid[5], aps[k].mhz, aps[k].rssi);
}

/*
 * Step 4: join an open network and see the data path work (mlme.c slsi_mlme_connect, slsi_mlme_connect_resp;
 * rx.c slsi_rx_connect_ind; tx.c; netif.h; fapi.h; facts only).
 *   The network: the one named by the boot-arg xc5ssid, else the strongest open one the scan found (capability
 *   without the privacy bit).
 *   MLME-CONNECT.request 0x200b {header; vif 1; bssid; authentication_type 0 (open); channel_frequency 2 x MHz;
 *   12 spare}, data: the SSID element -> MLME-CONNECT.confirm 0x210b {vif; result_code}; the firmware
 *   authenticates and associates and says MLME-CONNECT.indication 0x2303 {vif; result_code}, answered by
 *   MLME-CONNECT.response 0x2200 {vif; 12 spare}. (The 802.1X port is the host's own state: nothing to send.)
 *   MA-UNITDATA.request 0x1000 {header; vif 1; host_tag; priority 0; peer_index 1 (the AP: queue set 0 + 1);
 *   data_unit_descriptor 1 (802.3 frame); bulk_data_descriptor 0; 12 spare} with an Ethernet frame as data, in an
 *   mbulk of the data pool (class 4, pool 0) on the FH data queue -> MA-UNITDATA.confirm 0x1100 {vif;
 *   transmission_status}; received frames come as MA-UNITDATA.indication 0x1300 {vif; data_unit_descriptor; ...;
 *   proprieraty_information_length (bytes before the frame)}.
 *   The proof that frames go both ways: DHCP from the kernel -- DISCOVER, the OFFER's address, REQUEST, ACK.
 */
static int ap_sel = -1;
static uint32_t conn_cfm = 0xffff, conn_ind = 0xffff, disc_reason = 0xffff, data_cfm = 0xffff, data_tx, data_rx;
static uint32_t dhcp_state, dhcp_xid = 0x58433501, offer_ip, offer_srv, ack_ip, ack_gw, ack_dns, ack_mask;
static uint64_t dhcp_next, rescan_at, tx_freed_at, tx_busy;
static uint32_t tx_full, tx_force, tx_back, disc_count;
static char want_ssid[33];
/* Step 4e (xc5wifiui=1): iOS's own Wi-Fi side asks for scans and joins (y2sios's redwood_sta/redwood_apple80211) */
static int wifi_ui;
static bool ui_ready, ui_lost, want_scan, scan_inflight;
static uint64_t scan_done_ns;
static uint32_t irq_cleared;
static int want_join = -1;
static bool want_leave;
static uint8_t leave_sig[32];                /* (signals go out from the Wi-Fi thread's poll: see sta_scan) */
static void send_connect_to(int best);

static void
send_connect(void)
{
	int best = -1;
	for (uint32_t k = 0; k < ap_count && k < APN; k++) {
		bool named = want_ssid[0] != 0 && aps[k].ssid_len == strlen(want_ssid) &&
		    memcmp(aps[k].raw_ssid, want_ssid, aps[k].ssid_len) == 0;
		if (want_ssid[0] != 0 ? named : (aps[k].open && aps[k].ssid_len > 0)) {
			if (best < 0 || aps[k].rssi > aps[best].rssi) {
				best = (int)k;
			}
		}
	}
	if (best < 0) {
		printf("XC5 WLBT: no network to join (%s)\n", want_ssid[0] ? want_ssid : "no open one found");
		return;
	}
	send_connect_to(best);
}

static void
send_connect_to(int best)
{
	ap_sel = best;
	uint8_t s[34] = { 0x0b, 0x20 };
	s[10] = 1;
	memcpy(s + 12, aps[best].bssid, 6);
	s[18] = 0;                              /* open system */
	uint16_t fw = (uint16_t)(2 * aps[best].mhz);
	s[20] = (uint8_t)fw;
	s[21] = (uint8_t)(fw >> 8);
	uint8_t d[34];
	d[0] = 0;
	d[1] = aps[best].ssid_len;
	memcpy(d + 2, aps[best].raw_ssid, aps[best].ssid_len);
	send_sig(s, sizeof(s), d, 2u + aps[best].ssid_len);
	printf("XC5 WLBT: joining %s (%u MHz, %s)\n", aps[best].ssid, aps[best].mhz, aps[best].open ? "open" : "secured");
}

static uint16_t
ip_sum(const uint8_t *p, uint32_t n)
{
	uint32_t s = 0;
	for (uint32_t i = 0; i + 1 < n; i += 2) {
		s += (uint32_t)(p[i] << 8 | p[i + 1]);
	}
	while (s >> 16) {
		s = (s & 0xffff) + (s >> 16);
	}
	return (uint16_t)~s;
}

/* An Ethernet frame through the data path: MA-UNITDATA.request on the FH data queue, from the TX data area */
static void
send_frame(const uint8_t *fr, uint32_t len)
{
	struct hip_cfg_head *h = fw_cfg();
	/*
	 * A slot is the firmware's from the moment it is queued until it comes back on the FH RFB queue (hip_poll_locked).
	 * Before (wifi20: 2117 frames out), the 64 slots were simply taken in turn, so a slot could be written again while
	 * the firmware still read it. With every slot out for half a second, they are taken back as a last resort.
	 */
	static uint32_t slot;
	uint32_t n;
	for (n = 0; n < 64 && (tx_busy & (1ull << ((slot + n) % 64))) != 0; n++) {
	}
	if (n == 64) {
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		if (ns - tx_freed_at < 500000000ull) {
			tx_full++;
			return;
		}
		tx_busy = 0;
		tx_force++;
		n = 0;
	}
	slot = (slot + n) % 64;
	tx_busy |= 1ull << slot;
	uint32_t moff = HIP_TX + (slot++) * 2048;
	slot %= 64;
	uint8_t *m = hip + moff;
	uint8_t s[34] = { 0x00, 0x10 };
	s[10] = 1;                              /* vif */
	s[12] = (uint8_t)(0x10 + data_tx);      /* host_tag */
	s[13] = 0x10;
	s[16] = 1;                              /* peer_index */
	s[18] = 1;                              /* 802.3 frame */
	uint16_t sigbuf = (uint16_t)(((20 + sizeof(s) + 4 + 3) & ~3u) - 20);
	uint16_t head = (uint16_t)(sigbuf + h->headroom);
	bzero(m, 2048);
	m[4] = 1 | 4;
	m[5] = 4;                               /* MBULK_CLASS_FROM_HOST_DAT */
	m[6] = 0;                               /* MBULK_POOL_ID_DATA */
	m[7] = 1;
	*(uint16_t *)(m + 8) = (uint16_t)(len + h->headroom + h->tailroom);
	*(uint16_t *)(m + 10) = sigbuf;
	*(uint16_t *)(m + 12) = (uint16_t)len;
	*(uint16_t *)(m + 14) = head;
	memcpy(m + 20 + 4, s, sizeof(s));
	memcpy(m + 20 + head, fr, len);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (q_put(1, HIP_OFF + moff)) {
		data_tx++;
	}
}

/* DHCP over UDP over IPv4 in an Ethernet broadcast: type 1 DISCOVER, 3 REQUEST */
static void
send_dhcp(uint8_t type)
{
	uint8_t fr[342];
	bzero(fr, sizeof(fr));
	memset(fr, 0xff, 6);
	memcpy(fr + 6, vif_mac, 6);
	fr[12] = 0x08;
	uint8_t *ip = fr + 14, *udp = ip + 20, *b = udp + 8;
	uint32_t blen = 236 + 4, o = 0;
	uint8_t *opt = b + 240;
	opt[o++] = 53; opt[o++] = 1; opt[o++] = type;
	if (type == 3) {
		opt[o++] = 50; opt[o++] = 4; memcpy(opt + o, &offer_ip, 4); o += 4;
		opt[o++] = 54; opt[o++] = 4; memcpy(opt + o, &offer_srv, 4); o += 4;
	}
	opt[o++] = 55; opt[o++] = 3; opt[o++] = 1; opt[o++] = 3; opt[o++] = 6;
	opt[o++] = 255;
	blen += o;
	if (blen < 300) {
		blen = 300;
	}
	b[0] = 1; b[1] = 1; b[2] = 6;
	b[4] = (uint8_t)(dhcp_xid >> 24); b[5] = (uint8_t)(dhcp_xid >> 16); b[6] = (uint8_t)(dhcp_xid >> 8);
	b[7] = (uint8_t)dhcp_xid;
	b[10] = 0x80;                           /* broadcast replies */
	memcpy(b + 28, vif_mac, 6);
	b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;
	uint32_t ulen = 8 + blen, iplen = 20 + ulen;
	udp[1] = 68; udp[3] = 67;
	udp[4] = (uint8_t)(ulen >> 8); udp[5] = (uint8_t)ulen;
	ip[0] = 0x45; ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)iplen; ip[8] = 64; ip[9] = 17;
	memset(ip + 16, 0xff, 4);
	uint16_t cs = ip_sum(ip, 20);
	ip[10] = (uint8_t)(cs >> 8); ip[11] = (uint8_t)cs;
	send_frame(fr, 14 + iplen);
	printf("XC5 WLBT: DHCP %s sent\n", type == 1 ? "DISCOVER" : "REQUEST");
}

static void deliver(const uint8_t *fr, uint32_t n);

static void
on_frame(const uint8_t *fr, uint32_t n)
{
	data_rx++;
	deliver(fr, n);
	if (n < 14 + 20 + 8 + 240 || fr[12] != 0x08 || fr[13] != 0 || fr[14 + 9] != 17) {
		return;
	}
	const uint8_t *ip = fr + 14;
	const uint8_t *udp = ip + (ip[0] & 15) * 4, *b = udp + 8;
	if (udp[1] != 67 || udp[3] != 68 || b + 240 > fr + n || b[0] != 2 ||
	    (uint32_t)(b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]) != dhcp_xid) {
		return;
	}
	uint8_t type = 0;
	uint32_t srv = 0, gw = 0, dns = 0, mask = 0, yi;
	memcpy(&yi, b + 16, 4);
	for (const uint8_t *p = b + 240; p + 2 <= fr + n && p[0] != 255;) {
		if (p[0] == 0) {
			p++;
			continue;
		}
		if (p + 2 + p[1] > fr + n) {
			break;
		}
		if (p[0] == 53) {
			type = p[2];
		} else if (p[0] == 54 && p[1] >= 4) {
			memcpy(&srv, p + 2, 4);
		} else if (p[0] == 3 && p[1] >= 4) {
			memcpy(&gw, p + 2, 4);
		} else if (p[0] == 6 && p[1] >= 4) {
			memcpy(&dns, p + 2, 4);
		} else if (p[0] == 1 && p[1] >= 4) {
			memcpy(&mask, p + 2, 4);
		}
		p += 2 + p[1];
	}
	if (type == 2 && dhcp_state == 1) {     /* OFFER */
		offer_ip = yi;
		offer_srv = srv;
		dhcp_state = 2;
		send_dhcp(3);
	} else if (type == 5) {                 /* ACK */
		ack_ip = yi;
		ack_gw = gw;
		ack_dns = dns;
		ack_mask = mask;
		dhcp_state = 3;
	}
	const uint8_t *a = (const uint8_t *)&yi;
	printf("XC5 WLBT: DHCP %u from the network: %u.%u.%u.%u\n", type, a[0], a[1], a[2], a[3]);
}

static void
dhcp_tick(void)
{
	if (wifi_ui) {
		return;                         /* configd does DHCP on the interface it now knows */
	}
	if (conn_ind != 0 || dhcp_state >= 3) {
		return;
	}
	uint64_t now = mach_absolute_time(), ns;
	absolutetime_to_nanoseconds(now, &ns);
	if (ns < dhcp_next) {
		return;
	}
	dhcp_next = ns + 4000000000ull;
	if (dhcp_state <= 1) {
		dhcp_state = 1;
		dhcp_xid++;
		send_dhcp(1);
	} else if (dhcp_state == 2) {
		send_dhcp(3);
	}
}
static void
on_signal(uint32_t ref)
{
	static uint8_t data[2048];
	uint8_t *m = shm + ref;
	uint8_t *sig = m + 20 + 4;
	uint16_t id = *(uint16_t *)sig;
	mlme_ids[mlme_got % 8] = id;
	mlme_got++;
	uint32_t n = gather(ref, data, sizeof(data));
	switch (id) {
	case 0x2101:                            /* MLME_GET_CFM */
		mib_values(data, n);
		if (vif_res == 0xffff) {
			send_add_vif();
		}
		break;
	case 0x2107:                            /* MLME_ADD_VIF_CFM */
		vif_res = *(uint16_t *)(sig + 12);
		if (vif_res == 0 && wifi_ui) {
			ui_ready = true;                /* the Wi-Fi thread makes en0 and hands the station to wifid's side */
			scan_inflight = true;
			send_add_scan();                /* (and a first scan at once, as the working runs did: kept for wifid) */
		} else if (vif_res == 0) {
			send_add_scan();
		}
		break;
	case 0x2105:                            /* MLME_ADD_SCAN_CFM */
		scan_res = *(uint16_t *)(sig + 12);
		break;
	case 0x2300:                            /* MLME_SCAN_IND */
		scan_result(sig, data, n);
		break;
	case 0x2301:                            /* MLME_SCAN_DONE_IND */
		scan_done++;
		scan_inflight = false;
		absolutetime_to_nanoseconds(mach_absolute_time(), &scan_done_ns);
		if (!wifi_ui && ap_sel < 0 && conn_cfm == 0xffff) {
			send_connect();
			if (ap_sel < 0) {
				uint64_t ns;
				absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
				rescan_at = ns + 8000000000ull;         /* nothing to join yet: scan again */
			}
		}
		break;
	case 0x210b:                            /* MLME_CONNECT_CFM */
		conn_cfm = *(uint16_t *)(sig + 12);
		break;
	case 0x2303: {                          /* MLME_CONNECT_IND */
		conn_ind = *(uint16_t *)(sig + 12);
		conn_len = n;
		memcpy(conn_data, data, MIN((size_t)n, sizeof(conn_data)));
		if (conn_ind == 0x8212 && conn_try == 0 && ap_sel >= 0 && rate_retry) {    /* xc5wretry=1 (wifi16: no help) */
			conn_try = 1;
			/* MLME-ADD-INFO-ELEMENTS.request 0x2004 {vif 1; purpose 0x0010 association request}: the AP's rates */
			uint8_t s[26] = { 0x04, 0x20 };
			s[10] = 1;
			s[12] = 0x10;
			uint8_t d[24], k = 0, nr = aps[ap_sel].nrates;
			d[k++] = 1;
			d[k++] = (uint8_t)MIN(nr, 8);
			for (uint8_t i = 0; i < MIN(nr, 8); i++) {
				d[k++] = aps[ap_sel].rates[i];
			}
			if (nr > 8) {
				d[k++] = 50;
				d[k++] = (uint8_t)(nr - 8);
				for (uint8_t i = 8; i < nr; i++) {
					d[k++] = aps[ap_sel].rates[i];
				}
			}
			send_sig(s, sizeof(s), d, k);
		}
		uint8_t s[24] = { 0x00, 0x22 };
		s[10] = 1;
		if (conn_ind == 0) {
			send_sig(s, sizeof(s), NULL, 0);        /* MLME_CONNECT_RES */
		}
		break;
	}
	case 0x2309: {                          /* MLME_DISCONNECTED_IND: join again (a scan first, as at the start) */
		disc_reason = *(uint16_t *)(sig + 18);
		disc_count++;
		conn_ind = 0xffff;
		conn_cfm = 0xffff;
		if (wifi_ui) {
			ui_lost = true;                 /* told to wifid by the Wi-Fi thread; iOS decides whether to join again */
			break;
		}
		ap_sel = -1;
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		rescan_at = ns + 2000000000ull;
		break;
	}
	case 0x2104:                            /* MLME_ADD_INFO_ELEMENTS_CFM */
		info_res = *(uint16_t *)(sig + 12);
		if (conn_try == 1) {
			conn_try = 2;
			conn_ind = 0xffff;
			send_connect();
		}
		break;
	case 0x230a:                            /* MLME_PROCEDURE_STARTED_IND: our association request */
		req_len = n;
		n_req_ids = n_req_rates = 0;
		for (uint32_t p = 28; p + 2 <= n && p + 2u + data[p + 1] <= n; p += 2u + data[p + 1]) {
			if (n_req_ids < sizeof(req_ids)) {
				req_ids[n_req_ids++] = data[p];
			}
			if (data[p] == 1 || data[p] == 50) {
				for (uint32_t i = 0; i < data[p + 1] && n_req_rates < sizeof(req_rates); i++) {
					req_rates[n_req_rates++] = data[p + 2 + i];
				}
			}
		}
		break;
	case 0x1100:                            /* MA_UNITDATA_CFM */
		data_cfm = *(uint16_t *)(sig + 12);
		break;
	case 0x1300: {                          /* MA_UNITDATA_IND */
		uint16_t pil = *(uint16_t *)(sig + 20);
		if (pil < n) {
			on_frame(data + pil, n - pil);
		}
		break;
	}
	default:
		break;
	}
	if (id != 0x1300 && id != 0x8301) {
		printf("XC5 WLBT: signal %#x from the firmware, %u data bytes\n", id, n);
	}
}

/* Signals from the firmware (queues 3 and 4) and our buffers it gave back (queue 2); the caller holds xc5w_mtx */
static void
hip_poll_locked(void)
{
	if (hip == NULL || (hb_cfm & 2) == 0 || ((struct hip_init *)hip)->conf_ver == 0) {
		return;
	}
	if (!mlme_asked) {
		mlme_asked = true;
		send_mlme_get();
	}
	while (*scb(2, 1) != *scb(2, 0)) {
		uint8_t r = *scb(2, 1);
		uint32_t ref = qarr(2)[r];
		if (ref >= HIP_OFF + HIP_TX && ref < HIP_OFF + HIP_TX + 64 * 2048) {
			tx_busy &= ~(1ull << ((ref - HIP_OFF - HIP_TX) / 2048));
			absolutetime_to_nanoseconds(mach_absolute_time(), &tx_freed_at);
			tx_back++;
		}
		*scb(2, 1) = (uint8_t)(r + 1);
		fh_rfb++;
	}
	dhcp_tick();
	/*
	 * wifi28: the first scan (right after ADD_VIF) ended -- 2 networks heard, SCAN_DONE -- but wifid's scans that came
	 * after it emptied the list (ap_count = 0) and sent ADD_SCAN with the same scan id again and again, and none of
	 * those was confirmed: "scancfm ffff ind 2 done 1 aps 0 sent 10". Now what was heard is kept (by BSSID), a scan
	 * already running is not asked for again, and before another ADD_SCAN the last one is deleted (MLME-DEL-SCAN,
	 * 0x2006 {vif 0; scan_id}, as the driver does before it scans again in mgt.c / cfg80211_ops.c).
	 */
	if (want_scan) {
		want_scan = false;
		if (!scan_inflight) {
			uint8_t d[24] = { 0x06, 0x20 };
			d[12] = 0x00;
			d[13] = 0x01;                   /* scan id 0x100 */
			send_sig(d, sizeof(d), NULL, 0);
			scan_inflight = true;
			send_add_scan();
		}
	}
	if (want_join >= 0) {
		send_connect_to(want_join);
		want_join = -1;
	}
	if (want_leave) {
		want_leave = false;
		send_sig(leave_sig, sizeof(leave_sig), NULL, 0);
	}
	if (rescan_at != 0) {
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		if (ns >= rescan_at) {
			rescan_at = 0;
			send_add_scan();
		}
	}
	for (int q = 3; q <= 4; q++) {
		for (int guard = 0; guard < 64 && *scb(q, 1) != *scb(q, 0); guard++) {
			uint8_t r = *scb(q, 1);
			uint32_t ref = qarr(q)[r];
			if (ref >= HIP_OFF && ref + 64 < SHMEM_MAP) {
				on_signal(ref);
			} else {
				printf("XC5 WLBT: TH entry %#x outside the map\n", ref);
			}
			*scb(q, 1) = (uint8_t)(r + 1);
			__asm__ volatile ("dsb sy" ::: "memory");
			q_put(5, ref);          /* back to the firmware */
		}
	}
}
/*
 * Step 4c: the network interface (en<n>, normally en0) the iOS network stack sees. A legacy BSD Ethernet interface as
 * bsd/net/if_fake.c makes one in its BSD mode: ifnet_allocate_extended (IFNET_INIT_LEGACY, no Skywalk nexus), the
 * ether_* demux/framer, ifnet_attach, the link-layer address. Output: the framed Ethernet packet goes out as an
 * MA-UNITDATA.request. Input: every frame received becomes an mbuf whose packet header points at the Ethernet header
 * and whose data starts after it (as feth does it), handed to ifnet_input outside our lock. The address, mask and
 * default route come from the DHCP ACK, set with a kernel socket (SIOCAIFADDR) and a routing socket (RTM_ADD). Then
 * the gateway is pinged (an ICMP datagram socket) every 2 s, the proof that the stack itself talks through the radio.
 */
static LCK_GRP_DECLARE(xc5w_grp, "xc5wlbt");
static LCK_MTX_DECLARE(xc5w_mtx, &xc5w_grp);
static ifnet_t xc5_ifp;
static mbuf_t rx_head, rx_tail;
static uint32_t rx_n, if_out, if_in, if_drop, ping_sent, ping_got;
static int if_err = -1, addr_err = -1, route_err = -1, ping_err = -1;
static char if_nm[IFNAMSIZ] = "-";
static socket_t ping_so;
static uint64_t ping_next;
/*
 * Step 4d: what configd needs to make en0 the primary interface with a DNS server (gpu/eagl/src/xc5_net.c, inserted
 * into configd with xc5net=1, reads it and answers in kern.xc5netstat: bit 0 written to the dynamic store, bit 1 en0
 * primary, bit 2 a global DNS configuration, bit 3 a name resolved; 0x100 + SCError when writing failed).
 */
static char xc5wifi_info[128] = "down";
SYSCTL_STRING(_kern, OID_AUTO, xc5wifi, CTLFLAG_RD | CTLFLAG_LOCKED, xc5wifi_info, sizeof(xc5wifi_info), "");
static int xc5net_stat;
SYSCTL_INT(_kern, OID_AUTO, xc5netstat, CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &xc5net_stat, 0, "");

static void
deliver(const uint8_t *fr, uint32_t n)
{
	if (xc5_ifp == NULL || n < ETHER_HDR_LEN || n > 2048) {
		return;
	}
	mbuf_t m = NULL;
	unsigned int chunks = 1;
	if (mbuf_allocpacket(MBUF_DONTWAIT, n, &chunks, &m) != 0 || m == NULL) {
		if_drop++;
		return;
	}
	if (mbuf_copyback(m, 0, n, fr, MBUF_DONTWAIT) != 0) {
		mbuf_freem(m);
		if_drop++;
		return;
	}
	(void)mbuf_pkthdr_setrcvif(m, xc5_ifp);
	void *h = mbuf_data(m);
	mbuf_pkthdr_setheader(m, h);
	mbuf_pkthdr_adjustlen(m, -ETHER_HDR_LEN);
	(void)mbuf_setdata(m, (char *)h + ETHER_HDR_LEN, mbuf_len(m) - ETHER_HDR_LEN);
	if (rx_tail != NULL) {
		mbuf_setnextpkt(rx_tail, m);
	} else {
		rx_head = m;
	}
	rx_tail = m;
	rx_n++;
}

static void
hip_poll(void)
{
	lck_mtx_lock(&xc5w_mtx);
	/*
	 * wifi29: after an idle while the firmware logged "[DEEPSLEEP] infrastructure" and then answered nothing we sent
	 * (CONNECT: no confirm). We had never cleared its interrupts to us: Samsung's driver clears each one it handles
	 * (platform_mif_irq_bit_clear: INTCR0 = bit << 16). Cleared here, before the queues are read.
	 */
	uint32_t pend = rd(mbox, INTSR0) & 0xffff0000u;
	if (pend != 0) {
		wr(mbox, 0x00c, pend);          /* INTCR0 */
		irq_cleared++;
	}
	hip_poll_locked();
	mbuf_t head = rx_head;
	uint32_t n = rx_n;
	rx_head = rx_tail = NULL;
	rx_n = 0;
	ifnet_t ifp = xc5_ifp;
	lck_mtx_unlock(&xc5w_mtx);
	if (head != NULL) {
		struct ifnet_stat_increment_param st = {};
		st.packets_in = n;
		if_in += n;
		ifnet_input(ifp, head, &st);
	}
}

static errno_t
xc5_if_output(ifnet_t ifp, mbuf_t m)
{
	static uint8_t buf[1600];
	size_t len = mbuf_pkthdr_len(m);
	lck_mtx_lock(&xc5w_mtx);
	if (conn_ind == 0 && len >= ETHER_HDR_LEN && len <= sizeof(buf) && mbuf_copydata(m, 0, len, buf) == 0) {
		send_frame(buf, (uint32_t)len);
		if_out++;
		(void)ifnet_stat_increment_out(ifp, 1, (uint32_t)len, 0);
	} else {
		if_drop++;
		(void)ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	lck_mtx_unlock(&xc5w_mtx);
	mbuf_freem(m);
	return 0;
}

static bool link_up;

/*
 * Step 4e: as y2sios's redwood_net.c answers for its en0 -- the media (802.11, and whether the link is there, which is
 * what configd asks), the MTU, and every request it does not know handed to bsd/dev/redwood_apple80211.c: wifid's
 * Apple80211 ioctls (SIOCGA80211, SIOCSA80211) come to the interface that way.
 */
static errno_t
xc5_if_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
	const int media[] = { (wifi_ui ? IFM_IEEE80211 : IFM_ETHER) | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *mr = data;
	user_addr_t list;
	errno_t error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		if (wifi_ui) {
			ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		}
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		list = cmd == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)mr)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)mr)->ifmu_ulist);
		mr->ifm_active = mr->ifm_current = media[0];
		mr->ifm_mask = 0;
		mr->ifm_status = IFM_AVALID | ((wifi_ui ? link_up : conn_ind == 0) ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && mr->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		mr->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = ETHERMTU;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	default:
		error = wifi_ui ? redwood_apple80211_ioctl(cmd, data) : EOPNOTSUPP;
		break;
	}
	return error;
}

static void
sin_set(struct sockaddr_in *s, uint32_t a)
{
	bzero(s, sizeof(*s));
	s->sin_len = sizeof(*s);
	s->sin_family = AF_INET;
	s->sin_addr.s_addr = a;
}

/* In the Wi-Fi thread, without our lock (attaching and the address make the stack send) */
static void
net_up(void)
{
	struct ifnet_init_eparams p;
	ifnet_t ifp = NULL;
	uint32_t unit;
	for (unit = 0; unit < 8; unit++) {
		ifnet_t other = NULL;
		snprintf(if_nm, sizeof(if_nm), "en%u", unit);
		if (ifnet_find_by_name(if_nm, &other) != 0) {
			break;
		}
		ifnet_release(other);
	}
	bzero(&p, sizeof(p));
	p.ver = IFNET_INIT_CURRENT_VERSION;
	p.len = sizeof(p);
	p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
	p.uniqueid = "xc5wlan";
	p.uniqueid_len = 7;
	p.name = "en";
	p.unit = unit;
	p.family = IFNET_FAMILY_ETHERNET;
	p.subfamily = wifi_ui ? IFNET_SUBFAMILY_WIFI : IFNET_SUBFAMILY_ANY;     /* (how the stack and configd tell a radio) */
	p.type = IFT_ETHER;
	p.output = xc5_if_output;
	p.demux = ether_demux;
	p.add_proto = ether_add_proto;
	p.del_proto = ether_del_proto;
	p.check_multi = ether_check_multi;
	p.framer_extended = ether_frameout_extended;
	p.ioctl = xc5_if_ioctl;
	p.broadcast_addr = etherbroadcastaddr;
	p.broadcast_len = ETHER_ADDR_LEN;
	if_err = ifnet_allocate_extended(&p, &ifp);
	if (if_err != 0) {
		return;
	}
	(void)ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(ifp, 72000000);
	ifnet_set_mtu(ifp, ETHERMTU);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(ifp, sizeof(struct ether_header));
	ifnet_set_offload(ifp, 0);
	if_err = ifnet_attach(ifp, NULL);
	if (if_err != 0) {
		ifnet_release(ifp);
		return;
	}
	ifnet_set_lladdr(ifp, vif_mac, ETHER_ADDR_LEN);
	if (wifi_ui) {
		/* up is configd's to say; the link comes with a join (link_state) */
		lck_mtx_lock(&xc5w_mtx);
		xc5_ifp = ifp;
		lck_mtx_unlock(&xc5w_mtx);
		printf("XC5 WLBT: %s made for iOS's Wi-Fi side\n", if_nm);
		return;
	}
	ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	lck_mtx_lock(&xc5w_mtx);
	xc5_ifp = ifp;
	lck_mtx_unlock(&xc5w_mtx);

	uint32_t mask = ack_mask != 0 ? ack_mask : htonl(0xffffff00);
	socket_t so = NULL;
	addr_err = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	if (addr_err == 0) {
		struct in_aliasreq ifra;
		bzero(&ifra, sizeof(ifra));
		strlcpy(ifra.ifra_name, if_nm, sizeof(ifra.ifra_name));
		sin_set(&ifra.ifra_addr, ack_ip);
		sin_set(&ifra.ifra_mask, mask);
		sin_set(&ifra.ifra_broadaddr, ack_ip | ~mask);
		addr_err = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	if (ack_gw != 0) {
		struct {
			struct rt_msghdr h;
			struct sockaddr_in dst, gw, mask;
		} msg;
		bzero(&msg, sizeof(msg));
		msg.h.rtm_msglen = sizeof(msg);
		msg.h.rtm_version = RTM_VERSION;
		msg.h.rtm_type = RTM_ADD;
		msg.h.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
		msg.h.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
		msg.h.rtm_seq = 1;
		sin_set(&msg.dst, 0);
		sin_set(&msg.gw, ack_gw);
		sin_set(&msg.mask, 0);
		socket_t rs = NULL;
		route_err = sock_socket(PF_ROUTE, SOCK_RAW, 0, NULL, NULL, &rs);
		if (route_err == 0) {
			struct iovec iov = { &msg, sizeof(msg) };
			struct msghdr mh;
			size_t sent = 0;
			bzero(&mh, sizeof(mh));
			mh.msg_iov = &iov;
			mh.msg_iovlen = 1;
			route_err = sock_send(rs, &mh, 0, &sent);
			sock_close(rs);
		}
	}
	ping_err = sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_ICMP, NULL, NULL, &ping_so);
	const uint8_t *a = (const uint8_t *)&ack_ip, *mk = (const uint8_t *)&mask, *g = (const uint8_t *)&ack_gw;
	const uint8_t *d = (const uint8_t *)(ack_dns != 0 ? &ack_dns : &ack_gw);
	snprintf(xc5wifi_info, sizeof(xc5wifi_info), "up %s %u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u", if_nm, a[0], a[1],
	    a[2], a[3], mk[0], mk[1], mk[2], mk[3], g[0], g[1], g[2], g[3], d[0], d[1], d[2], d[3]);
	printf("XC5 WLBT: %s up (%d), address %d, route %d, ping socket %d\n", if_nm, if_err, addr_err, route_err, ping_err);
}

static void
ping_tick(void)
{
	if (ping_so == NULL || ack_gw == 0) {
		return;
	}
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	if (ns >= ping_next) {
		ping_next = ns + 2000000000ull;
		uint8_t e[24];
		bzero(e, sizeof(e));
		e[0] = 8;                       /* echo request */
		e[4] = 0x58;
		e[5] = 0x35;
		e[7] = (uint8_t)ping_sent;
		uint16_t cs = ip_sum(e, sizeof(e));
		e[2] = (uint8_t)(cs >> 8);
		e[3] = (uint8_t)cs;
		struct sockaddr_in to;
		sin_set(&to, ack_gw);
		struct iovec iov = { e, sizeof(e) };
		struct msghdr mh;
		size_t sent = 0;
		bzero(&mh, sizeof(mh));
		mh.msg_name = &to;
		mh.msg_namelen = sizeof(to);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		ping_err = sock_send(ping_so, &mh, 0, &sent);
		if (ping_err == 0) {
			ping_sent++;
		}
	}
	for (int k = 0; k < 8; k++) {
		uint8_t r[128];
		struct iovec iov = { r, sizeof(r) };
		struct msghdr mh;
		size_t got = 0;
		bzero(&mh, sizeof(mh));
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		if (sock_receive(ping_so, &mh, MSG_DONTWAIT, &got) != 0 || got == 0) {
			break;
		}
		const uint8_t *icmp = (got >= 20 && (r[0] >> 4) == 4) ? r + (r[0] & 15) * 4 : r;
		if (icmp < r + got && icmp[0] == 0) {
			ping_got++;
		}
	}
}
/*
 * Step 4e: the station side of y2sios's station boundary (sys/redwood_sta.h): what iOS's Wi-Fi side (wifid, Settings,
 * through bsd/dev/redwood_apple80211.c and redwood_sta.c's thread) asks of the radio, done with this file's signals.
 * Each call may take as long as the thing takes; scan() returns when the scan is over, join() when frames can pass or
 * it is known why not. Open networks only so far: a WPA2 join is refused (Settings: "Unable to join").
 */
extern void RedwoodWLANStart(const char *interface, const uint8_t *address);   /* iokit/Kernel/arm/RedwoodWLAN.cpp */
extern void MiniIONetworkStackStart(void);                                     /* iokit/Kernel/arm/MiniIONetworkStack.cpp */

static volatile int sta_phase = REDWOOD_STA_IDLE;
static uint32_t ui_scans, ui_joins, ui_leaves;

static void
link_event(uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(xc5_ifp);
	about->if_unit = ifnet_unit(xc5_ifp);
	strlcpy(about->if_name, ifnet_name(xc5_ifp), IFNAMSIZ);
	ifnet_event(xc5_ifp, header);
}

/* frames can pass now / no longer: configd starts DHCP on the first */
static void
link_state(bool up)
{
	if (xc5_ifp == NULL || up == link_up) {
		return;
	}
	link_up = up;
	printf("XC5 WLBT: %s link %s\n", if_nm, up ? "up" : "down");
	link_event(up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}

static bool
wait_until(volatile uint32_t *value, uint32_t from, unsigned ms)
{
	for (unsigned t = 0; t < ms / 20; t++) {
		if (*value != from) {
			return true;
		}
		delay(20000);
	}
	return *value != from;
}

int sta_leave_call(void);

static int
sta_power(bool on)
{
	if (!on && sta_phase == REDWOOD_STA_JOINED) {
		return sta_leave_call();
	}
	return 0;
}

/* the radio's interface (vif 1) exists: what was asked for before that waits up to 90 s */
static bool
sta_wait_ready(void)
{
	for (int i = 0; i < 4500 && !ui_ready; i++) {
		delay(20000);
	}
	return ui_ready;
}

static int
sta_scan(const struct redwood_sta_scan *what)
{
	(void)what;                             /* every 2.4 GHz channel, any name, actively: what this file's scan does */
	if (!sta_wait_ready()) {
		return EBUSY;
	}
	/*
	 * wifi27: an ADD_SCAN sent from this (redwood_sta's) thread got no confirm at all -- "sent 3 got 10 scancfm ffff",
	 * nothing more from the firmware after ADD_VIF's confirm -- while every signal sent from the Wi-Fi thread's poll
	 * was answered. So the scan is asked of that thread (want_scan), which sends it as it sent the others.
	 */
	lck_mtx_lock(&xc5w_mtx);
	uint32_t seen = scan_done;
	uint64_t now;
	absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	/* a scan that ended within the last 20 s answers at once (wifid asks every few seconds while Settings is open) */
	if (!scan_inflight && scan_done != 0 && now - scan_done_ns < 20000000000ull) {
		lck_mtx_unlock(&xc5w_mtx);
		ui_scans++;
		return 0;
	}
	scan_res = 0xffff;
	if (sta_phase != REDWOOD_STA_JOINED) {
		sta_phase = REDWOOD_STA_SCANNING;
	}
	want_scan = true;
	lck_mtx_unlock(&xc5w_mtx);
	ui_scans++;
	bool done = wait_until((volatile uint32_t *)&scan_done, seen, 12000);
	if (sta_phase == REDWOOD_STA_SCANNING) {
		sta_phase = REDWOOD_STA_IDLE;
	}
	return done ? 0 : ETIMEDOUT;
}

static void
bss_of(uint32_t k, struct redwood_sta_bss *b)
{
	bzero(b, sizeof(*b));
	memcpy(b->bssid, aps[k].bssid, 6);
	memcpy(b->ssid, aps[k].raw_ssid, aps[k].ssid_len);
	b->ssid_length = aps[k].ssid_len;
	b->frequency = aps[k].mhz;
	b->rssi = aps[k].rssi;
	b->beacon_interval = aps[k].beacon_interval;
	b->capability = aps[k].capability;
	b->elements_length = aps[k].elements_length;
	memcpy(b->elements, aps[k].elements, aps[k].elements_length);
}

static void
sta_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	static struct redwood_sta_bss b;        /* (one caller: redwood_sta.c's thread) */
	for (uint32_t k = 0; k < ap_count && k < APN; k++) {
		lck_mtx_lock(&xc5w_mtx);
		bss_of(k, &b);
		lck_mtx_unlock(&xc5w_mtx);
		each(&b, context);
	}
}

static int
find_ap(const struct redwood_sta_join *what)
{
	static const uint8_t none[6];
	int best = -1;
	for (uint32_t k = 0; k < ap_count && k < APN; k++) {
		if (aps[k].ssid_len == what->ssid_length && memcmp(aps[k].raw_ssid, what->ssid, what->ssid_length) == 0 &&
		    (memcmp(what->bssid, none, 6) == 0 || memcmp(what->bssid, aps[k].bssid, 6) == 0) &&
		    (best < 0 || aps[k].rssi > aps[best].rssi)) {
			best = (int)k;
		}
	}
	return best;
}

static int
sta_join(const struct redwood_sta_join *what)
{
	ui_joins++;
	if (!sta_wait_ready()) {
		return EBUSY;
	}
	if (what->security != REDWOOD_STA_OPEN) {
		printf("XC5 WLBT: a WPA2 join is asked for: not built yet\n");
		return EOPNOTSUPP;              /* (Apple80211's "unable to join", not "wrong password": EAUTH would say that) */
	}
	lck_mtx_lock(&xc5w_mtx);
	int k = find_ap(what);
	lck_mtx_unlock(&xc5w_mtx);
	if (k < 0) {
		static const struct redwood_sta_scan any;
		sta_scan(&any);
		lck_mtx_lock(&xc5w_mtx);
		k = find_ap(what);
		lck_mtx_unlock(&xc5w_mtx);
	}
	if (k < 0) {
		return ENOENT;
	}
	lck_mtx_lock(&xc5w_mtx);
	conn_cfm = 0xffff;
	conn_ind = 0xffff;
	sta_phase = REDWOOD_STA_JOINING;
	want_join = k;
	lck_mtx_unlock(&xc5w_mtx);
	wait_until((volatile uint32_t *)&conn_ind, 0xffff, 15000);
	if (conn_ind != 0) {
		sta_phase = REDWOOD_STA_IDLE;
		return conn_ind == 0xffff ? ETIMEDOUT : ECONNREFUSED;
	}
	sta_phase = REDWOOD_STA_JOINED;
	link_state(true);
	return 0;
}

int
sta_leave_call(void)
{
	ui_leaves++;
	if (ap_sel >= 0 && conn_ind == 0) {
		/* MLME-DISCONNECT.request 0x200e {vif 1; peer_sta_address; reason_code 3 (leaving)} */
		uint8_t s[32] = { 0x0e, 0x20 };
		s[10] = 1;
		lck_mtx_lock(&xc5w_mtx);
		memcpy(s + 12, aps[ap_sel].bssid, 6);
		s[18] = 3;
		memcpy(leave_sig, s, sizeof(s));
		want_leave = true;
		conn_ind = 0xffff;
		lck_mtx_unlock(&xc5w_mtx);
	}
	link_state(false);
	sta_phase = REDWOOD_STA_IDLE;
	return 0;
}

static int
sta_leave(void)
{
	return sta_leave_call();
}

static void
sta_state(struct redwood_sta_state *state)
{
	state->phase = (enum redwood_sta_phase)sta_phase;
	if (ap_sel >= 0 && ap_sel < APN) {
		bss_of((uint32_t)ap_sel, &state->bss);
	}
}

static unsigned
sta_channels(struct redwood_sta_channel *ch, unsigned most)
{
	unsigned n = 0;
	for (unsigned c = 1; c <= 13 && n < most; c++) {
		ch[n].frequency = (uint16_t)(2407 + 5 * c);
		ch[n].flags = 0;
		n++;
	}
	return n;
}

static struct redwood_sta_ops xc5_sta_ops = {
	.power = sta_power, .scan = sta_scan, .heard = sta_heard, .join = sta_join, .leave = sta_leave, .state = sta_state,
	.channels = sta_channels,
};

/* In the Wi-Fi thread once the station interface (vif 1) exists: en0, its registry entries, the station boundary */
static void
ui_up(void)
{
	net_up();
	if (xc5_ifp == NULL) {
		return;
	}
	memcpy(xc5_sta_ops.address, vif_mac, 6);
	/* configd's interface namer waits for IONetworkStack before it calls the registry quiet, and wifid for that */
	MiniIONetworkStackStart();
	RedwoodWLANStart(if_nm, vif_mac);
	redwood_sta_attach(&xc5_sta_ops);
	printf("XC5 WLBT: %s handed to iOS's Wi-Fi side\n", if_nm);
}

/* In the Wi-Fi thread: the network was lost (MLME-DISCONNECTED.indication) */
static void
ui_tick(void)
{
	if (ui_lost) {
		ui_lost = false;
		if (sta_phase == REDWOOD_STA_JOINED) {
			sta_phase = REDWOOD_STA_IDLE;
			link_state(false);
			redwood_sta_lost(ECONNRESET);
		}
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
	/*
	 * wifi23: Settings' Wi-Fi switch stayed off. wifid looks for its device when it starts, and en0 with its registry
	 * entries came only after the firmware, the service and vif 1 were up (tens of seconds later). As y2sios has it
	 * ("so en0 exists before wifid starts"): made here, first, as soon as the network stack is (lo0 there); scans
	 * and joins asked for before the radio is ready wait for it (sta_wait_ready).
	 */
	if (wifi_ui) {
		ifnet_t lo = NULL;
		for (int i = 0; i < 300 && ifnet_find_by_name("lo0", &lo) != 0; i++) {
			delay(100000);
		}
		if (lo != NULL) {
			ifnet_release(lo);
		}
		ui_up();
	}
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
		hip_poll();

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
	/* 10 ms ticks from here on: the network interface's receive path is this loop (step 4c) */
	for (int i = 0, tick = 0;; tick++) {
		if (tick % 50 == 0) {
			read_mgmt();
			poll_log(200 + tick / 10);
		}
		hip_poll();
		if (xc5_ifp == NULL && dhcp_state == 3 && if_err == -1 && !wifi_ui) {
			net_up();
		}
		if (wifi_ui && xc5_ifp == NULL && if_err == -1) {
			ui_up();                        /* (made at the start; this only if that did not happen) */
		}
		if (wifi_ui) {
			ui_tick();
		}
		ping_tick();
		if (tick % 2000 != 0 || i >= 9) {
			delay(10000);
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
		delay(10000);
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
	/*
	 * wifi14/15: the block got so long that its first lines were pushed off the screen by the time it could be filmed:
	 * the Wi-Fi state is now a few short lines, the ones that matter last (just above "XC5 amfi").
	 */
	printf("XC5 wlbt pwr=%u st=%#x issr0=%#x mgmt w%u log w%u fw%u: %.60s\n", hb_power, rd(pmu, WLBT_STATES),
	    rd(mbox, ISSR(0)), *(volatile uint32_t *)(shm + conf->mgmt.to_ap.buf.write_index_loc),
	    *(volatile uint32_t *)(shm + conf->mxlog.buf.write_index_loc), log_recs, loglines[log_recs % LOGN]);
	if (hip == NULL) {
		return;
	}
	printf("XC5 w cfm %x vif %x scan %x ind %u done %u aps %u got %u\n", hb_cfm, vif_res, scan_res, scan_inds, scan_done,
	    ap_count, mlme_got);
	if (ap_sel >= 0) {
		printf("XC5 w ap %.20s %u %d %c\n", aps[ap_sel].ssid, aps[ap_sel].mhz, aps[ap_sel].rssi, aps[ap_sel].open ? 'o' : 's');
	} else {
		for (uint32_t k = 0; k < ap_count && k < 3; k++) {
			printf("XC5 w ap? %.20s %u %d %c\n", aps[k].ssid, aps[k].mhz, aps[k].rssi, aps[k].open ? 'o' : 's');
		}
	}
	printf("XC5 w req %u ids %x %x %x %x %x %x r %x %x %x %x %x %x %x %x\n", req_len, req_ids[0], req_ids[1], req_ids[2],
	    req_ids[3], req_ids[4], req_ids[5], req_rates[0], req_rates[1], req_rates[2], req_rates[3], req_rates[4],
	    req_rates[5], req_rates[6], req_rates[7]);
	printf("XC5 w join cfm %x ind %x st %u disc %x n %u | tx back %u full %u force %u\n", conn_cfm, conn_ind,
	    conn_len >= 28 ? (conn_data[26] | conn_data[27] << 8) : 0xffff, disc_reason, disc_count, tx_back, tx_full, tx_force);
	const uint8_t *ip = (const uint8_t *)&ack_ip, *of = (const uint8_t *)&offer_ip;
	printf("XC5 w data tx %u cfm %x rx %u dhcp %u offer %u.%u.%u.%u ip %u.%u.%u.%u\n", data_tx, data_cfm, data_rx,
	    dhcp_state, of[0], of[1], of[2], of[3], ip[0], ip[1], ip[2], ip[3]);
	printf("XC5 w if %s %d addr %d route %d out %u in %u drop %u ping %u/%u (%d)\n", if_nm, if_err, addr_err, route_err,
	    if_out, if_in, if_drop, ping_got, ping_sent, ping_err);
	const uint8_t *dn = (const uint8_t *)&ack_dns;
	printf("XC5 w net %#x dns %u.%u.%u.%u | ui %d phase %d link %d scans %u joins %u leaves %u\n", xc5net_stat, dn[0],
	    dn[1], dn[2], dn[3], wifi_ui, sta_phase, link_up, ui_scans, ui_joins, ui_leaves);
	if (wifi_ui) {
		extern unsigned xc5_a80211_requests, xc5_a80211_refused, xc5_a80211_results, xc5_a80211_given, xc5_a80211_scandone;
		extern int xc5_a80211_last_type, xc5_a80211_last_error;
		printf("XC5 w a80211 req %u refused %u (last %d err %d) | scan aps %u done %u ev %u | results asked %u given %u\n",
		    xc5_a80211_requests, xc5_a80211_refused, xc5_a80211_last_type, xc5_a80211_last_error, ap_count, scan_done,
		    xc5_a80211_scandone, xc5_a80211_results, xc5_a80211_given);
		/* wifi26: scans asked by wifid never ended (done 0, aps 0): what the firmware said to them */
		printf("XC5 w fw vif %x scancfm %x ind %u sent %u got %u irqclr %u sr0 %x ids %x %x %x %x\n", vif_res, scan_res, scan_inds, mlme_sent,
		    mlme_got, irq_cleared, rd(mbox, INTSR0), mlme_ids[(mlme_got + 4) % 8], mlme_ids[(mlme_got + 5) % 8], mlme_ids[(mlme_got + 6) % 8],
		    mlme_ids[(mlme_got + 7) % 8]);
	}
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
	PE_parse_boot_argn("xc5ssid", want_ssid, sizeof(want_ssid) - 1);
	PE_parse_boot_argn("xc5wretry", &rate_retry, sizeof(rate_retry));
	PE_parse_boot_argn("xc5wifiui", &wifi_ui, sizeof(wifi_ui));
	thread_t thread;
	if (kernel_thread_start(wlbt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
