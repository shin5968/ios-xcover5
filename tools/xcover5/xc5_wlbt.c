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
#include <net/bpf.h>
#include <sys/random.h>
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
static uint32_t ctl_busy, ctl_back, ctl_force, t_loop1_s, t_loop2_s, poll_ms = 10;
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
static uint32_t mib_idle = 0xff, mib_staidle = 0xff, mib_idlelite = 0xff, set_res = 0xffff;
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
static bool sig_quiet;
static uint64_t sig_last_ns;
static uint32_t ka_sent, ka_got;

static void
send_sig(const uint8_t *sigp, uint32_t siglen, const uint8_t *data, uint32_t dlen)
{
	struct hip_cfg_head *h = fw_cfg();
	/*
	 * wifi42n: the firmware stops unjoined in the auto mode too (not iOS's side). The counts of signals it had read
	 * when it stopped were 37 twice and 69 (32 + 5, 64 + 5): the 32 control slots were taken in turn whether or not
	 * the firmware had given them back on FH_RFB (the data slots are tracked, and the joined runs sent under 32
	 * control signals). Now a control slot is used again only once it has come back (ctl_back); with none free the
	 * next in turn is taken (ctl_force).
	 */
	uint32_t slot = 32;
	for (uint32_t i = 0; i < 32; i++) {
		uint32_t c = (tx_slot + i) % 32;
		if (!(ctl_busy & (1u << c))) {
			slot = c;
			break;
		}
	}
	if (slot == 32) {
		slot = tx_slot % 32;
		ctl_force++;
	}
	tx_slot = slot + 1;
	uint32_t moff = HIP_TX + 0x100000 + slot * 2048;
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
		ctl_busy |= 1u << slot;
		absolutetime_to_nanoseconds(mach_absolute_time(), &sig_last_ns);
		if (!sig_quiet)
		printf("XC5 WLBT: signal %#x (%u + %u bytes) sent, mbulk %#x\n", *(const uint16_t *)sigp, siglen, dlen,
		    HIP_OFF + moff);
	}
}

static void
send_mlme_get(void)
{
	/* + unifiHtActivated 0x07FE, unifiVhtActivated 0x07FD, unifiSupportedDataRates 0x07F9 (wifi13: association refused) */
	static const uint8_t keys[] = { 0xE6, 0x07, 0, 0, 0x41, 0x09, 0, 0, 0xE5, 0x07, 0, 0, 0xFE, 0x07, 0, 0, 0xFD, 0x07, 0, 0,
		                        0xF9, 0x07, 0, 0, 0xDF, 0x09, 0, 0, 0xBD, 0x09, 0, 0, 0xDE, 0x09, 0, 0 };
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
static int wifi_idle;           /* xc5widle=1: leave the firmware's idle modes as the hcf sets them */

/*
 * wifi38: kept busy with MLME-GETs, the firmware still stopped (ka 11/10, q0 w30 r13) about 2 s after its first
 * SCAN_DONE, with one station vif and no connection -- the case UnifiIdleModeEnabled (0x09DF, "if single vif station is
 * active or there is no vif, and there has been no activity for a time") and UnifiStaIdleModeEnabled (0x09BD) cover;
 * the auto mode connected at once and never was idle. Our doorbell does not bring it back. So both, and the softAP's
 * UnifiIdleModeLiteEnabled (0x09DE), are switched off with MLME-SET.request 0x2002 before the vif is added.
 */
static void
send_idle_off(void)
{
	static const uint8_t d[] = { 0xDF, 0x09, 1, 0, 0, 0, 0xBD, 0x09, 1, 0, 0, 0, 0xDE, 0x09, 1, 0, 0, 0 };
	uint8_t s[24] = { 0x02, 0x20 };         /* MLME_SET_REQ, vif 0 */
	send_sig(s, sizeof(s), d, sizeof(d));
}

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

static uint64_t scan_sent_ns;
static uint32_t scan_aborts;
static int wifi_country = 1;
static uint32_t country_res = 0xffff;
static int wifi_shadow = 1;    /* xc5wshadow=0: no join of its own before iOS asks */
static uint32_t shadow_tries, shadow_ok, shadow_used;
static int wifi_kdhcp = 1;
static int wifi_chain = 2;      /* 2: the next ADD_SCAN at once, no DEL_SCAN; 1: DEL_SCAN first (wifi34); 0: none */
static uint32_t rfb_back, rfb_chained, rfb_full;
static int wifi_pmu = 1;
static uint32_t pmu_before[3];
static uint32_t fault_n, fault_id, fault_count, fault_arg, fault_cpu;
static uint32_t ka_ms = 200;

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
	absolutetime_to_nanoseconds(mach_absolute_time(), &scan_sent_ns);
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
		} else if (psid == 0x09DF) {
			mib_idle = val;
		} else if (psid == 0x09BD) {
			mib_staidle = val;
		} else if (psid == 0x09DE) {
			mib_idlelite = val;
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
static char want_psk[64];             /* xc5psk=: the passphrase of xc5ssid's network, for the kernel's own (shadow) join */
/* Step 4e (xc5wifiui=1): iOS's own Wi-Fi side asks for scans and joins (y2sios's redwood_sta/redwood_apple80211) */
static int wifi_ui;
static bool ui_ready, ui_lost, want_scan, scan_inflight;
static uint64_t scan_done_ns;
static uint32_t irq_cleared;
static int join_sec = -1, join_len, join_bssid_given, join_stage, join_err = -1;
static int want_join = -1;
static int del_next, del_join = -1, join_after_scan = -1;
static bool del_done;
static uint64_t del_sent_ns;
static uint32_t del_cfm = 0xffff, chain_scans, join_on_running;
static uint32_t q_kicks, q_kick_ok, q_stall_ms;
static uint32_t wdog_at_s, alive_at_s, wdog_n, alive_n;
static uint64_t q_stall_since, q_kick_at;
static uint8_t q_stall_r;
static volatile uint8_t *apmbox;
static uint64_t q_stall_at_s;
static bool q_stall_told;
static volatile int sta_phase = REDWOOD_STA_IDLE;
static bool want_leave;
static uint8_t leave_sig[32];                /* (signals go out from the Wi-Fi thread's poll: see sta_scan) */
static void send_connect_to(int best);
static void send_del_scan(int next, uint64_t now_ns);

/*
 * WPA2-PSK (step 5): the 4-way handshake is the host's to do (Samsung's driver leaves it to wpa_supplicant and installs
 * what comes of it with MLME-SETKEYS.request 0x2018 {vif; length (bits); key_id; key_type 1 pairwise / 0 group;
 * address; sequence_number[8]; cipher_suite_selector}, data the key -- mlme.c slsi_mlme_set_key). Here it is done in
 * the kernel: the PMK (given by wifid, or PBKDF2-SHA1 of the passphrase and the name, 4096 rounds), the PTK (PRF-384
 * "Pairwise key expansion"), EAPOL-Key messages 2 and 4 with their HMAC-SHA1 MICs, and the GTK from message 3's key
 * data (AES key unwrap, RFC 3394, with the KEK). CCMP pairwise, the group cipher the access point names (CCMP or TKIP).
 * The CONNECT carries our RSN element after the SSID element, as slsi_mlme_connect appends the security element.
 */
struct sha1c {
	uint32_t h[5];
	uint64_t n;
	uint8_t b[64];
	uint32_t bl;
};

static inline uint32_t
rol32(uint32_t x, int s)
{
	return x << s | x >> (32 - s);
}

static void
sha1_blk(struct sha1c *c, const uint8_t *p)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 80; i++) {
		w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}
	uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & cc) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ cc ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & cc) | (b & d) | (cc & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ cc ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t t = rol32(a, 5) + f + e + k + w[i];
		e = d;
		d = cc;
		cc = rol32(b, 30);
		b = a;
		a = t;
	}
	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
}

static void
sha1_init(struct sha1c *c)
{
	c->h[0] = 0x67452301;
	c->h[1] = 0xEFCDAB89;
	c->h[2] = 0x98BADCFE;
	c->h[3] = 0x10325476;
	c->h[4] = 0xC3D2E1F0;
	c->n = 0;
	c->bl = 0;
}

static void
sha1_update(struct sha1c *c, const uint8_t *p, uint32_t n)
{
	c->n += n;
	while (n > 0) {
		uint32_t k = MIN(n, 64 - c->bl);
		memcpy(c->b + c->bl, p, k);
		c->bl += k;
		p += k;
		n -= k;
		if (c->bl == 64) {
			sha1_blk(c, c->b);
			c->bl = 0;
		}
	}
}

static void
sha1_final(struct sha1c *c, uint8_t out[20])
{
	uint64_t bits = c->n * 8;
	uint8_t pad = 0x80, z = 0;
	sha1_update(c, &pad, 1);
	while (c->bl != 56) {
		sha1_update(c, &z, 1);
	}
	uint8_t len[8];
	for (int i = 0; i < 8; i++) {
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha1_update(c, len, 8);
	for (int i = 0; i < 5; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

/* HMAC-SHA1 of up to three pieces */
static void
hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *d1, uint32_t l1, const uint8_t *d2, uint32_t l2,
    const uint8_t *d3, uint32_t l3, uint8_t out[20])
{
	uint8_t k[64], pad[64], inner[20];
	bzero(k, sizeof(k));
	if (klen > 64) {
		struct sha1c c;
		sha1_init(&c);
		sha1_update(&c, key, klen);
		sha1_final(&c, k);
	} else {
		memcpy(k, key, klen);
	}
	struct sha1c c;
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x36;
	}
	sha1_init(&c);
	sha1_update(&c, pad, 64);
	if (l1) {
		sha1_update(&c, d1, l1);
	}
	if (l2) {
		sha1_update(&c, d2, l2);
	}
	if (l3) {
		sha1_update(&c, d3, l3);
	}
	sha1_final(&c, inner);
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x5c;
	}
	sha1_init(&c);
	sha1_update(&c, pad, 64);
	sha1_update(&c, inner, 20);
	sha1_final(&c, out);
}

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) */
static void
wpa_pbkdf2(const uint8_t *pass, uint32_t plen, const uint8_t *ssid, uint32_t slen, uint8_t pmk[32])
{
	uint8_t out[40];
	for (uint32_t blk = 1; blk <= 2; blk++) {
		uint8_t ctr[4] = { 0, 0, 0, (uint8_t)blk }, u[20], t[20];
		hmac_sha1(pass, plen, ssid, slen, ctr, 4, NULL, 0, u);
		memcpy(t, u, 20);
		for (int i = 1; i < 4096; i++) {
			hmac_sha1(pass, plen, u, 20, NULL, 0, NULL, 0, u);
			for (int j = 0; j < 20; j++) {
				t[j] ^= u[j];
			}
		}
		memcpy(out + (blk - 1) * 20, t, 20);
	}
	memcpy(pmk, out, 32);
}

/* AES-128 decryption of one block, for the key unwrap */
static uint8_t aes_sb[256], aes_isb[256];

static inline uint8_t
rol8(uint8_t x, int s)
{
	return (uint8_t)(x << s | x >> (8 - s));
}

static void
aes_tables(void)
{
	if (aes_sb[0] == 0x63) {
		return;
	}
	uint8_t p = 1, q = 1;
	do {
		p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0));
		q ^= (uint8_t)(q << 1);
		q ^= (uint8_t)(q << 2);
		q ^= (uint8_t)(q << 4);
		q ^= (q & 0x80) ? 0x09 : 0;
		uint8_t x = (uint8_t)(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4));
		aes_sb[p] = x ^ 0x63;
	} while (p != 1);
	aes_sb[0] = 0x63;
	for (int i = 0; i < 256; i++) {
		aes_isb[aes_sb[i]] = (uint8_t)i;
	}
}

static uint8_t
gmul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;
	while (b) {
		if (b & 1) {
			r ^= a;
		}
		a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
		b >>= 1;
	}
	return r;
}

static void
aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
	aes_tables();
	memcpy(rk, key, 16);
	uint8_t rcon = 1;
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
		if (i % 16 == 0) {
			uint8_t u = t[0];
			t[0] = aes_sb[t[1]] ^ rcon;
			t[1] = aes_sb[t[2]];
			t[2] = aes_sb[t[3]];
			t[3] = aes_sb[u];
			rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
		}
		for (int j = 0; j < 4; j++) {
			rk[i + j] = rk[i - 16 + j] ^ t[j];
		}
	}
}

static void
aes128_decrypt(const uint8_t rk[176], uint8_t s[16])
{
	for (int i = 0; i < 16; i++) {
		s[i] ^= rk[160 + i];
	}
	for (int round = 9; round >= 0; round--) {
		uint8_t t[16];
		for (int r = 0; r < 4; r++) {                   /* InvShiftRows (s[r + 4c]) */
			for (int c = 0; c < 4; c++) {
				t[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
			}
		}
		for (int i = 0; i < 16; i++) {                  /* InvSubBytes, AddRoundKey */
			s[i] = aes_isb[t[i]] ^ rk[16 * round + i];
		}
		if (round > 0) {                                /* InvMixColumns */
			for (int c = 0; c < 4; c++) {
				uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
				s[4 * c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
				s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
				s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
				s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
			}
		}
	}
}

/* RFC 3394 key unwrap: in n+1 64-bit blocks, out n; true if the integrity value checks */
static bool
aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t len, uint8_t *out)
{
	if (len < 24 || len % 8 != 0 || len > 256) {
		return false;
	}
	uint32_t n = len / 8 - 1;
	uint8_t rk[176], a[8], b[16];
	aes128_expand(kek, rk);
	memcpy(a, in, 8);
	memcpy(out, in + 8, n * 8);
	for (int j = 5; j >= 0; j--) {
		for (uint32_t i = n; i >= 1; i--) {
			uint64_t t = (uint64_t)n * (uint64_t)j + i;
			for (int x = 0; x < 8; x++) {
				b[x] = a[x] ^ (uint8_t)(t >> (56 - 8 * x));
			}
			memcpy(b + 8, out + (i - 1) * 8, 8);
			aes128_decrypt(rk, b);
			memcpy(a, b, 8);
			memcpy(out + (i - 1) * 8, b + 8, 8);
		}
	}
	for (int x = 0; x < 8; x++) {
		if (a[x] != 0xA6) {
			return false;
		}
	}
	return true;
}

static bool wpa_on;
static volatile uint32_t wpa_done;
static uint8_t wpa_pmk[32], wpa_ptk[48], wpa_anonce[32], wpa_snonce[32], wpa_gtk[32], wpa_gtk_rsc[8];
static uint32_t wpa_gtk_len, wpa_gtk_id, wpa_group_cipher = 0x000FAC04;
static uint32_t wpa_m1, wpa_m3, wpa_g1, wpa_mic_bad, wpa_unwrap_bad, wpa_keys_sent, wpa_keys_res = 0xffff;
static uint64_t wpa_install_at, wpa_conn_at;
static uint32_t wpa_pmk_res = 0xffff, wpa_offload, rx_ind_n, rx_rf_n;
static bool wpa_install_group_only;
static uint8_t wpa_rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
	                       0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };
static void send_frame(const uint8_t *fr, uint32_t len);

/* the group cipher the access point's RSN element names (CCMP 4 or TKIP 2), into our element */
static void
wpa_rsn_for(const uint8_t *ies, uint32_t n)
{
	wpa_group_cipher = 0x000FAC04;
	for (uint32_t p = 0; p + 2 <= n && p + 2 + ies[p + 1] <= n; p += 2u + ies[p + 1]) {
		if (ies[p] == 48 && ies[p + 1] >= 6) {
			uint8_t g = ies[p + 7];
			if (g == 2 || g == 4) {
				wpa_group_cipher = 0x000FAC00u | g;
				wpa_rsn[7] = g;
			}
			return;
		}
	}
	wpa_rsn[7] = 4;
}

static void
wpa_prf_ptk(const uint8_t *aa, const uint8_t *spa)
{
	uint8_t data[76];
	const uint8_t *lo = memcmp(aa, spa, 6) < 0 ? aa : spa, *hi = lo == aa ? spa : aa;
	memcpy(data, lo, 6);
	memcpy(data + 6, hi, 6);
	const uint8_t *nlo = memcmp(wpa_anonce, wpa_snonce, 32) < 0 ? wpa_anonce : wpa_snonce;
	const uint8_t *nhi = nlo == wpa_anonce ? wpa_snonce : wpa_anonce;
	memcpy(data + 12, nlo, 32);
	memcpy(data + 44, nhi, 32);
	static const uint8_t label[23] = "Pairwise key expansion";     /* with its terminating 0 */
	uint8_t out[60];
	for (uint8_t i = 0; i < 3; i++) {
		hmac_sha1(wpa_pmk, 32, label, sizeof(label), data, sizeof(data), &i, 1, out + 20 * i);
	}
	memcpy(wpa_ptk, out, 48);
}

/* an EAPOL-Key frame from us: info, replay counter, nonce (or zeros), key data (or none), MIC over it with the KCK */
static void
wpa_send_key(const uint8_t *ap, uint16_t info, const uint8_t replay[8], const uint8_t *nonce, const uint8_t *kd,
    uint16_t kdlen)
{
	uint8_t fr[14 + 4 + 95 + 32];
	uint16_t body = (uint16_t)(95 + kdlen);
	bzero(fr, sizeof(fr));
	memcpy(fr, ap, 6);
	memcpy(fr + 6, vif_mac, 6);
	fr[12] = 0x88;
	fr[13] = 0x8e;
	uint8_t *e = fr + 14;
	e[0] = 1;                                       /* 802.1X-2001 */
	e[1] = 3;                                       /* EAPOL-Key */
	e[2] = (uint8_t)(body >> 8);
	e[3] = (uint8_t)body;
	uint8_t *k = e + 4;
	k[0] = 2;                                       /* RSN key descriptor */
	k[1] = (uint8_t)(info >> 8);
	k[2] = (uint8_t)info;
	memcpy(k + 5, replay, 8);
	if (nonce != NULL) {
		memcpy(k + 13, nonce, 32);
	}
	k[93] = (uint8_t)(kdlen >> 8);
	k[94] = (uint8_t)kdlen;
	if (kdlen) {
		memcpy(k + 95, kd, kdlen);
	}
	uint8_t mic[20];
	hmac_sha1(wpa_ptk, 16, e, 4u + body, NULL, 0, NULL, 0, mic);
	memcpy(k + 77, mic, 16);
	send_frame(fr, 14u + 4u + body);
}

static void
wpa_setkey(uint16_t type, uint16_t id, const uint8_t *addr, const uint8_t *key, uint32_t klen, uint32_t cipher,
    const uint8_t *rsc)
{
	uint8_t s[56] = { 0x18, 0x20 };
	s[10] = 1;                                      /* vif */
	*(uint16_t *)(s + 12) = (uint16_t)(klen * 8);
	*(uint16_t *)(s + 14) = id;
	*(uint16_t *)(s + 16) = type;
	memcpy(s + 18, addr, 6);
	if (rsc != NULL) {
		for (int i = 0; i < 8; i += 2) {
			*(uint16_t *)(s + 24 + i) = (uint16_t)(rsc[i] | rsc[i + 1] << 8);
		}
	}
	*(uint32_t *)(s + 40) = cipher;
	send_sig(s, sizeof(s), key, klen);
	wpa_keys_sent++;
}

/* the GTK KDE in decrypted key data: dd len 00 0f ac 01 {keyid & 3, 0} gtk */
static bool
wpa_gtk_from(const uint8_t *kd, uint32_t n)
{
	for (uint32_t p = 0; p + 2 <= n && p + 2 + kd[p + 1] <= n; p += 2u + kd[p + 1]) {
		if (kd[p] == 0xdd && kd[p + 1] >= 6 + 16 && kd[p + 2] == 0 && kd[p + 3] == 0x0f && kd[p + 4] == 0xac &&
		    kd[p + 5] == 1) {
			wpa_gtk_id = kd[p + 6] & 3;
			wpa_gtk_len = MIN((uint32_t)kd[p + 1] - 6, 32u);
			memcpy(wpa_gtk, kd + p + 8, wpa_gtk_len);
			return true;
		}
	}
	return false;
}

/* an EAPOL frame from the access point (in the Wi-Fi thread's poll); true if it was ours to take */
static bool
wpa_eapol_in(const uint8_t *fr, uint32_t n)
{
	if (!wpa_on || n < 14 + 4 + 95 || fr[12] != 0x88 || fr[13] != 0x8e || fr[15] != 3 || ap_sel < 0) {
		return false;
	}
	const uint8_t *e = fr + 14, *k = e + 4, *ap = aps[ap_sel].bssid;
	uint16_t body = (uint16_t)(e[2] << 8 | e[3]), info = (uint16_t)(k[1] << 8 | k[2]);
	uint16_t kdlen = (uint16_t)(k[93] << 8 | k[94]);
	if (4u + body > n - 14 || 95u + kdlen > body) {
		return true;
	}
	bool pairwise = info & 0x0008, install = info & 0x0040, ack = info & 0x0080, mic = info & 0x0100;
	if (pairwise && ack && !mic) {                  /* message 1 */
		wpa_m1++;
		memcpy(wpa_anonce, k + 13, 32);
		read_random(wpa_snonce, sizeof(wpa_snonce));
		wpa_prf_ptk(ap, vif_mac);
		wpa_send_key(ap, 0x010A, k + 5, wpa_snonce, wpa_rsn, sizeof(wpa_rsn));
		return true;
	}
	if (!mic) {
		return true;
	}
	/* messages 3 and group 1: the MIC is checked first */
	static uint8_t copy[512];
	if (4u + body > sizeof(copy)) {
		return true;
	}
	memcpy(copy, e, 4u + body);
	bzero(copy + 4 + 77, 16);
	uint8_t want[20];
	hmac_sha1(wpa_ptk, 16, copy, 4u + body, NULL, 0, NULL, 0, want);
	if (memcmp(want, k + 77, 16) != 0) {
		wpa_mic_bad++;
		return true;
	}
	static uint8_t kd[256];
	bool have = false;
	if ((info & 0x1000) && kdlen >= 24) {
		have = aes_unwrap(wpa_ptk + 16, k + 95, kdlen, kd);
		if (!have) {
			wpa_unwrap_bad++;
			return true;
		}
	}
	if (pairwise && ack && install) {               /* message 3 */
		wpa_m3++;
		if (have) {
			wpa_gtk_from(kd, kdlen - 8u);
		}
		memcpy(wpa_gtk_rsc, k + 61, 8);
		wpa_send_key(ap, 0x030A, k + 5, NULL, NULL, 0);
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		wpa_install_at = ns + 100000000ull;     /* message 4 out first, then the keys */
		wpa_install_group_only = false;
		return true;
	}
	if (!pairwise && ack) {                         /* group key handshake message 1 */
		wpa_g1++;
		if (have && wpa_gtk_from(kd, kdlen - 8u)) {
			memcpy(wpa_gtk_rsc, k + 61, 8);
			wpa_send_key(ap, 0x0302, k + 5, NULL, NULL, 0);
			uint64_t ns;
			absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
			wpa_install_at = ns + 100000000ull;
			wpa_install_group_only = true;
		}
		return true;
	}
	return true;
}

/* in the Wi-Fi thread's poll */
static void
wpa_tick(uint64_t now_ns)
{
	if (wpa_on && !wpa_done && wpa_m1 == 0 && conn_ind == 0 && wpa_conn_at != 0 && now_ns - wpa_conn_at > 3000000000ull) {
		wpa_offload++;
		wpa_done = 1;                   /* no EAPOL came up: the firmware did the handshake with the PMK */
	}
	if (wpa_install_at == 0 || now_ns < wpa_install_at || ap_sel < 0) {
		return;
	}
	wpa_install_at = 0;
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	if (!wpa_install_group_only) {
		wpa_setkey(1, 0, aps[ap_sel].bssid, wpa_ptk + 32, 16, 0x000FAC04, NULL);
	}
	if (wpa_gtk_len != 0) {
		wpa_setkey(0, (uint16_t)wpa_gtk_id, bcast, wpa_gtk, wpa_gtk_len, wpa_group_cipher, wpa_gtk_rsc);
	}
	wpa_done = true;
}

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
	/*
	 * wifi51: with S24 secured the shadow join (open networks only) never happened, the firmware stopped unjoined at 48 s
	 * and the WPA2 join from Settings came too late. A secured network named by xc5ssid with xc5psk is joined by the
	 * kernel as well: the PMK is made here and the 4-way handshake done; Settings then takes that join.
	 */
	if (!aps[best].open) {
		size_t pl = strlen(want_psk);
		if (pl < 8) {
			printf("XC5 WLBT: %s is secured and no xc5psk= is given\n", aps[best].ssid);
			return;
		}
		wpa_pbkdf2((const uint8_t *)want_psk, (uint32_t)pl, aps[best].raw_ssid, aps[best].ssid_len, wpa_pmk);
		wpa_on = true;
		wpa_done = 0;
		wpa_install_at = 0;
		wpa_gtk_len = 0;
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
	uint8_t d[34 + sizeof(wpa_rsn)];
	d[0] = 0;
	d[1] = aps[best].ssid_len;
	memcpy(d + 2, aps[best].raw_ssid, aps[best].ssid_len);
	uint32_t dl = 2u + aps[best].ssid_len;
	if (wpa_on) {
		/*
		 * wifi53: S24 (WPA2) was associated (shadow ok 1) but no EAPOL frame ever came (rx 0, m1 0) and the AP left
		 * with reason 15 (4-way handshake timeout); the firmware also reported a fault on our CONNECT (2c76 arg
		 * 200b). Samsung's driver gives the firmware the PMK (MLME-SET-PMK 0x201c, cfg80211_ops.c add_key with
		 * WLAN_CIPHER_SUITE_PMK) when the handshake is offloaded: then the firmware does it and passes no EAPOL up.
		 * So the PMK goes in first; if keys are not done by the host within 3 s of the join, the firmware's are taken.
		 */
		uint8_t ps[24] = { 0x1c, 0x20 };
		ps[10] = 1;
		send_sig(ps, sizeof(ps), wpa_pmk, sizeof(wpa_pmk));
		wpa_rsn_for(aps[best].elements, aps[best].elements_length);
		memcpy(d + dl, wpa_rsn, sizeof(wpa_rsn));
		dl += sizeof(wpa_rsn);
	}
	send_sig(s, sizeof(s), d, dl);
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
/*
 * wifi47 (shadow join used): iOS's 12 frames all went out (tx back 12) and nothing came in (rx 0), where the kernel's own
 * DHCP had worked. iOS gives each network a private (random) Wi-Fi address: frames from it, DHCP's chaddr in it, and
 * the AP answers to it -- an address the firmware's vif does not take. So (xc5wmacfix=1, the default) a frame's source
 * and a DHCP chaddr equal to iOS's address are rewritten to the vif's, and frames coming in for the vif get iOS's
 * address back. The first source, the ethertypes and counts are said in "XC5 w txmac".
 */
static int wifi_macfix = 1;
static uint8_t ios_mac[6];
static bool ios_mac_set;
static uint32_t tx_foreign, rx_mapped, tx_types[4], tx_ntypes;
static uint8_t tx_first[14];

static void
mac_out(uint8_t *fr, uint32_t len)
{
	if (len < 14) {
		return;
	}
	if (tx_ntypes == 0) {
		memcpy(tx_first, fr, 14);
	}
	uint32_t et = (uint32_t)(fr[12] << 8 | fr[13]);
	if (tx_ntypes < 4) {
		tx_types[tx_ntypes] = et;
	}
	tx_ntypes++;
	if (memcmp(fr + 6, vif_mac, 6) == 0) {
		return;
	}
	tx_foreign++;
	if (!ios_mac_set) {
		memcpy(ios_mac, fr + 6, 6);
		ios_mac_set = true;
	}
	if (!wifi_macfix) {
		return;
	}
	memcpy(fr + 6, vif_mac, 6);
	/* DHCP (IPv4, UDP 68 -> 67): chaddr too, and the UDP checksum left out (0, allowed for IPv4) */
	if (et == 0x0800 && len >= 14 + 20 + 8 + 34 && fr[14 + 9] == 17) {
		uint8_t *ip = fr + 14, *udp = ip + (ip[0] & 15) * 4;
		if (udp + 8 + 34 <= fr + len && udp[1] == 68 && udp[3] == 67 && memcmp(udp + 8 + 28, ios_mac, 6) == 0) {
			memcpy(udp + 8 + 28, vif_mac, 6);
			udp[6] = 0;
			udp[7] = 0;
		}
	}
}

static void
mac_in(uint8_t *fr, uint32_t len)
{
	if (!wifi_macfix || !ios_mac_set || len < 14) {
		return;
	}
	if (memcmp(fr, vif_mac, 6) == 0) {
		memcpy(fr, ios_mac, 6);
		rx_mapped++;
	}
	if (fr[12] == 0x08 && fr[13] == 0 && len >= 14 + 20 + 8 + 34 && fr[14 + 9] == 17) {
		uint8_t *ip = fr + 14, *udp = ip + (ip[0] & 15) * 4;
		if (udp + 8 + 34 <= fr + len && udp[1] == 67 && udp[3] == 68 && memcmp(udp + 8 + 28, vif_mac, 6) == 0) {
			memcpy(udp + 8 + 28, ios_mac, 6);
			udp[6] = 0;
			udp[7] = 0;
		}
	}
}

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
	mac_out(m + 20 + head, len);
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
	if (wpa_eapol_in(fr, n)) {
		return;
	}
	if (wifi_ui) {
		mac_in((uint8_t *)fr, n);
	}
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
	/*
	 * wifi48: Settings took the kernel's join (stage 6, link 1) but iOS sent only IPv6 (12 frames, all 0x86dd, from the
	 * vif's own address) and never a DHCP DISCOVER; nothing came in. So, joined, the kernel does DHCP itself as in the
	 * auto mode that worked (xc5wdhcp=0: not), puts the address on en0 and hands it to configd (kern.xc5wifi, libxc5net).
	 */
	if (wifi_ui && !(wifi_kdhcp && wifi_shadow && ap_sel >= 0)) {
		return;
	}
	if (conn_ind != 0 || dhcp_state >= 3 || (wpa_on && !wpa_done)) {
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
		if (vif_res == 0xffff && set_res == 0xffff && !wifi_idle) {
			send_idle_off();        /* ADD_VIF follows its confirm */
		} else if (vif_res == 0xffff) {
			send_add_vif();
		}
		break;
	case 0x2102:                            /* MLME_SET_CFM */
		set_res = *(uint16_t *)(sig + 12);
		if (vif_res == 0xffff && country_res == 0xffff && wifi_country) {
			/*
			 * A14 (Galaxy A14 4G, Exynos 850, Android 15) source compared with ours: the HIP4, mailbox and PMU code match;
			 * what Samsung's slsi_start sends that we never did is MLME-SET-COUNTRY.request 0x2033 {vif 0; country_code
			 * (alpha2[0] << 8 | alpha2[1]); dfs_regulatory_domain} before any vif (mgt.c, mlme.c slsi_mlme_set_country).
			 * Without it the firmware runs in its world domain. "KR", no rules (no regdb here). xc5wcountry=0: not sent.
			 */
			uint8_t s[24] = { 0x33, 0x20 };
			s[12] = 'R';
			s[13] = 'K';
			send_sig(s, sizeof(s), NULL, 0);
		} else if (vif_res == 0xffff) {
			send_add_vif();
		}
		break;
	case 0x2133:                            /* MLME_SET_COUNTRY_CFM */
		country_res = *(uint16_t *)(sig + 12);
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
	case 0x8302: {                          /* DEBUG_FAULT_IND {vif; faultid; count; timestamp; arg; cpu} */
		/*
		 * wifi39: with the idle modes off the firmware lasted ~13 s more and, before it stopped, sent DEBUG_FAULT_IND
		 * (ids 8302 2101 8302 2101): its own report of what went wrong. Kept and said.
		 */
		fault_n++;
		fault_id = *(uint16_t *)(sig + 12);
		fault_count = *(uint16_t *)(sig + 14);
		fault_arg = *(uint32_t *)(sig + 20);
		fault_cpu = *(uint16_t *)(sig + 24);
		if (fault_n <= 12) {
			printf("XC5 WLBT: FW FAULT id %#x count %u arg %#x cpu %u (n %u, got %u)\n", fault_id, fault_count, fault_arg,
			    fault_cpu, fault_n, mlme_got);
		}
		break;
	}
	case 0x211c:                            /* MLME_SET_PMK_CFM */
		wpa_pmk_res = *(uint16_t *)(sig + 12);
		break;
	case 0x230d:                            /* MLME_RECEIVED_FRAME_IND */
		rx_rf_n++;
		break;
	case 0x2118:                            /* MLME_SETKEYS_CFM */
		wpa_keys_res = *(uint16_t *)(sig + 12);
		break;
	case 0x2106:                            /* MLME_DEL_SCAN_CFM */
		del_cfm = *(uint16_t *)(sig + 12);
		del_done = true;
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
		if (join_after_scan >= 0) {
			send_connect_to(join_after_scan);       /* at once, as the working joins did */
			join_after_scan = -1;
		} else if (wifi_ui && wifi_shadow && ap_sel < 0 && conn_cfm == 0xffff && sta_phase != REDWOOD_STA_JOINED &&
		    sta_phase != REDWOOD_STA_JOINING && want_join < 0) {
			/*
			 * wifi37-46: unjoined, the firmware stopped every time, 1 s to ~15 s after it started, whatever was sent
			 * (nothing, GETs, 28 chained scans, idle modes off, country set); every buffer had come back (wifi46: rfb
			 * 8388 chained 4137 full 0, q3 w155 r155). Joined the moment its first scan ended (auto mode, wifi19-22,
			 * 36a) it ran for minutes. So the Settings mode does the same: at this SCAN_DONE the kernel joins the open
			 * network (xc5ssid=NAME, or the strongest open one) and keeps it; the link stays down for iOS until
			 * Settings asks for that network (sta_join then answers at once). Before iOS's own DHCP nothing is sent.
			 */
			send_connect();
			if (ap_sel >= 0) {
				shadow_tries++;
			}
		}
		if (ap_sel < 0 && wifi_ui && wifi_shadow && want_join < 0 && join_after_scan < 0 && !scan_inflight &&
		    sta_phase != REDWOOD_STA_JOINED && sta_phase != REDWOOD_STA_JOINING) {
			scan_inflight = true;                   /* no open network yet: look again at once */
			chain_scans++;
			send_add_scan();
		} else if (wifi_chain && wifi_ui && sta_phase != REDWOOD_STA_JOINED && sta_phase != REDWOOD_STA_JOINING && del_next == 0 &&
		    want_join < 0 && del_join < 0) {
			/*
			 * wifi33: after the first scan's SCAN_DONE the firmware sent nothing more (got 86 from 35 s to 55 s, last id
			 * 0x2301): the join's DEL_SCAN (del ffff) and ADD_SCAN went unanswered, as every signal sent after an idle
			 * gap had (wifi28, wifi29: "[DEEPSLEEP] infrastructure"). The joins that worked sent CONNECT the moment a
			 * scan ended. So until joined the firmware is never left idle: each SCAN_DONE starts the next scan, and a
			 * join's CONNECT goes out at the end of the scan running when it is asked for.
			 */
			uint64_t ns;
			absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
			scan_inflight = true;
			chain_scans++;
			if (wifi_chain == 2) {
				send_add_scan();
			} else {
				send_del_scan(1, ns);
			}
		}
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
			absolutetime_to_nanoseconds(mach_absolute_time(), &wpa_conn_at);
			if (wifi_ui && sta_phase != REDWOOD_STA_JOINING) {
				shadow_ok++;
				printf("XC5 WLBT: joined %s on its own (shadow), link kept down for iOS\n", ap_sel >= 0 ? aps[ap_sel].ssid : "?");
			}
		} else if (wifi_ui && sta_phase != REDWOOD_STA_JOINING) {
			ap_sel = -1;                    /* shadow join refused: the next SCAN_DONE tries again */
			conn_cfm = 0xffff;
			scan_inflight = true;
			send_add_scan();
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
			if (wifi_shadow) {
				ap_sel = -1;            /* and the kernel joins again on the next SCAN_DONE */
				scan_inflight = true;
				send_add_scan();
			}
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
		rx_ind_n++;
		uint16_t pil = *(uint16_t *)(sig + 20);
		if (pil < n) {
			on_frame(data + pil, n - pil);
		}
		break;
	}
	default:
		break;
	}
	if (id == 0x2101 && ka_sent != 0) {
		ka_got++;
	}
	if (id != 0x1300 && id != 0x8301 && id != 0x8302 && !(id == 0x2101 && ka_sent != 0)) {
		printf("XC5 WLBT: signal %#x from the firmware, %u data bytes\n", id, n);
	}
}

/* MLME-DEL-SCAN.request 0x2006 {vif 0; scan_id 0x100}; then (next) 1 an ADD_SCAN or 2 the CONNECT, once confirmed */
static void
send_del_scan(int next, uint64_t now_ns)
{
	uint8_t d[24] = { 0x06, 0x20 };
	d[12] = 0x00;
	d[13] = 0x01;
	del_done = false;
	del_next = next;
	del_sent_ns = now_ns;
	send_sig(d, sizeof(d), NULL, 0);
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
		} else if (ref >= HIP_OFF + HIP_TX + 0x100000 && ref < HIP_OFF + HIP_TX + 0x100000 + 32 * 2048) {
			ctl_busy &= ~(1u << ((ref - HIP_OFF - HIP_TX - 0x100000) / 2048));
			ctl_back++;
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
	/*
	 * wifi31: a join from Settings sent its CONNECT (stage 4) and nothing came back (cfm ffff, 15 s): the scan after the
	 * first one had not ended ("ind 3 done 1") -- DEL_SCAN and ADD_SCAN had gone out back to back -- and the firmware
	 * keeps a CONNECT waiting behind a running scan. So now each DEL_SCAN is waited for (its confirm, 0x2106, or 2 s)
	 * before what it makes room for: the next ADD_SCAN, or the CONNECT. A join always deletes the scan first.
	 */
	uint64_t now_ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	/*
	 * wifi34: the firmware stopped reading the FH control queue with one signal in it (q0 w47 r46). If what we queued
	 * waits more than 300 ms, the doorbell (INTGR1 bit 5) is rung again, once a second; kick_ok counts the times the
	 * queue moved after a ring.
	 */
	{
		uint8_t qw = *scb(0, 0), qr = *scb(0, 1);
		if (qw == qr) {
			q_stall_since = 0;
		} else if (q_stall_since == 0 || qr != q_stall_r) {
			if (q_stall_since != 0 && q_kick_at != 0) {
				q_kick_ok++;
			}
			q_stall_since = now_ns;
			q_stall_at_s = now_ns / 1000000000ull;
			q_stall_r = qr;
			q_kick_at = 0;
		} else {
			q_stall_ms = (uint32_t)((now_ns - q_stall_since) / 1000000ull);
			if (q_stall_ms > 300 && (q_kick_at == 0 || now_ns - q_kick_at > 1000000000ull)) {
				q_kick_at = now_ns;
				q_kicks++;
				if (q_kicks <= 6) {
					printf("XC5 WLBT: FW STALL q0 w%u r%u %u ms | st %x sr1 %x msr1 %x | got %u done %u\n", qw, qr,
					    q_stall_ms, rd(pmu, WLBT_STATES), rd(mbox, INTSR1), rd(mbox, INTMSR1), mlme_got, scan_done);
				}
				wr(mbox, 0x01c, 1u << HIP_FH_BIT);      /* INTGR1 */
				if (!q_stall_told && q_stall_ms > 5000) {
					/* wifi35/35r: the firmware's last log line was always "Speedy drive strength changed": its lines before */
					q_stall_told = true;
					for (uint32_t k = 1; k <= LOGN; k++) {
						printf("XC5 WLBT: FW LOG -%u: %.100s\n", LOGN - k, loglines[(log_recs + k) % LOGN]);
					}
				}
			}
		}
	}
	/*
	 * wifi37: with no DEL_SCAN and no chaining, the firmware read the first 3 signals (GET, ADD_VIF, ADD_SCAN), sent the
	 * first scan's results and SCAN_DONE, and read nothing after (q0 w53 r3, got 87, INTSR1 bit 5 never cleared). Every
	 * ui-mode stall came after a gap with nothing sent; the auto mode never had one (DHCP, ARP and pings at once). The
	 * firmware's idle deep sleep ("[DEEPSLEEP] infrastructure", wifi29) is not woken by our doorbell. So it is never left
	 * idle: when nothing was sent for xc5wka ms (default 200; 0 off) and the queue is empty, a one-key MLME-GET goes out.
	 */
	if (ka_ms != 0 && vif_res == 0 && *scb(0, 0) == *scb(0, 1) && sig_last_ns != 0 &&
	    now_ns - sig_last_ns > (uint64_t)ka_ms * 1000000ull) {
		static const uint8_t key[] = { 0xE6, 0x07, 0, 0 };     /* unifiChipVersion */
		uint8_t s[24] = { 0x01, 0x20 };
		sig_quiet = true;
		send_sig(s, sizeof(s), key, sizeof(key));
		sig_quiet = false;
		ka_sent++;
	}
	wpa_tick(now_ns);
	if (del_next != 0 && (del_done || now_ns - del_sent_ns > 2000000000ull)) {
		int next = del_next;
		del_next = 0;
		if (next == 1) {
			scan_inflight = true;
			send_add_scan();
		} else if (next == 2 && del_join >= 0) {
			/*
			 * wifi32: CONNECT after the DEL_SCAN still went unanswered. The CONNECTs that worked (wifi17-22) went out
			 * the moment a scan ended, and Samsung's driver scans for the network just before it connects
			 * (cfg80211_ops.c slsi_connect: slsi_mlme_connect_scan). So: a fresh scan, and the CONNECT on its end.
			 */
			scan_inflight = true;
			join_after_scan = del_join;
			del_join = -1;
			send_add_scan();
		}
	}
	/*
	 * wifi36a: the same kernel in the auto mode (xc5net=1) stayed up for minutes (ping 68/68, 8846 frames out, got 32304):
	 * that mode never sends MLME-DEL-SCAN and scans once. The ui mode deleted every finished scan before the next one
	 * (wifi28 on) and stalled the firmware after 1-51 of them. Samsung's driver deletes a scan only to abort one still
	 * running (cfg80211_ops.c slsi_abort_hw_scan, mgt.c "Mlme_scan_done_ind not received"), never a finished one. So now:
	 * a scan or a join's scan is an ADD_SCAN alone; DEL_SCAN only for a scan with no SCAN_DONE after 15 s; no chaining
	 * (xc5wchain=1 brings it back).
	 */
	if (scan_inflight && del_next == 0 && scan_sent_ns != 0 && now_ns - scan_sent_ns > 15000000000ull) {
		scan_sent_ns = 0;
		scan_aborts++;
		send_del_scan(1, now_ns);               /* (then a new ADD_SCAN) */
	}
	if (want_join >= 0 && scan_inflight && del_next == 0) {
		/* a scan is running (the firmware is awake): the CONNECT goes out at its SCAN_DONE, nothing is deleted */
		join_after_scan = want_join;
		want_join = -1;
		want_scan = false;
		join_on_running++;
	} else if (want_join >= 0 && del_next == 0) {
		join_after_scan = want_join;            /* scan for it, CONNECT at SCAN_DONE (as the auto mode and slsi_connect) */
		want_join = -1;
		want_scan = false;
		scan_inflight = true;
		send_add_scan();
	} else if (want_scan && del_next == 0) {
		want_scan = false;
		if (!scan_inflight) {
			scan_inflight = true;
			send_add_scan();
		}
	}
	/*
	 * wifi43: the stop came at 35 s, in the first poll loop (loop2 began at 51 s) and with every control slot given back
	 * (back 23 = r23): neither the poll period nor the slots. Over all runs the firmware lived exactly as long as its
	 * radio had work: joined (minutes), scans chained (22-51 scans, 30-66 s; the 2 s DEL_SCAN waits were the gaps it
	 * died in), and 1-13 s after the last scan otherwise, keepalive GETs or not. So until joined the radio is never left
	 * idle: the next ADD_SCAN goes out at SCAN_DONE itself (on_signal) or here, with no DEL_SCAN (Samsung's driver adds
	 * the same scan id again without deleting a finished one).
	 */
	if (wifi_ui && wifi_chain == 2 && ui_ready && !scan_inflight && del_next == 0 && want_join < 0 && join_after_scan < 0 &&
	    !(wifi_shadow && ap_sel >= 0) &&
	    sta_phase != REDWOOD_STA_JOINED && sta_phase != REDWOOD_STA_JOINING) {
		scan_inflight = true;
		chain_scans++;
		send_add_scan();
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
			/*
			 * wifi41/41k: the firmware stopped reading our queue after exactly 33 keepalives both with 200 ms and with
			 * 5 ms between them -- a count, not a time. Every buffer of a chained signal (signal + data: GET confirms,
			 * scan results) has to go back to the firmware (hip4.c hip4_mbulk_to_skb: to_free[] holds the head and each
			 * chain_next, all put on TH_RFB); only the head went back, so its pool ran dry, it could not answer, and it
			 * stopped taking requests. The auto mode sends few such signals, so it never ran out.
			 */
			uint32_t back = ref;
			for (int seg = 0; seg < 8; seg++) {
				if (!q_put(5, back)) {
				    rfb_full++;
				}
				rfb_back++;
				uint8_t *mb = shm + back;
				if (!(mb[4] & 0x80)) {          /* MBULK_F_CHAIN: another buffer follows */
					break;
				}
				back = *(uint32_t *)(mb + 16); /* chain_next_offset */
				if (back < HIP_OFF || back + 64 >= SHMEM_MAP) {
					break;
				}
				rfb_chained++;
			}
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
static int bpf_err = -1;
static uint32_t bpf_in, bpf_out;
static void net_addr(void);
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
	/*
	 * wifi44: chained scans did not help (done 3, stall at 33 s, ~3 s after the firmware started at 30 s). Did the
	 * core's own watchdog or reset request fire? WLBT's WDOG and ALIVE interrupts (never taken, only watched): the first
	 * uptime each was seen pending.
	 */
	{
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		if (spi_pending(SPI_WDOG)) {
			wdog_n++;
			if (wdog_at_s == 0) {
				wdog_at_s = (uint32_t)(ns / 1000000000ull);
				printf("XC5 WLBT: WDOG interrupt pending at %u s\n", wdog_at_s);
			}
		}
		if (spi_pending(SPI_ALIVE)) {
			alive_n++;
			if (alive_at_s == 0) {
				alive_at_s = (uint32_t)(ns / 1000000000ull);
				printf("XC5 WLBT: ALIVE interrupt pending at %u s\n", alive_at_s);
			}
		}
	}
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
		for (mbuf_t p = head; p != NULL; p = mbuf_nextpkt(p)) {
			bpf_tap_in(ifp, DLT_EN10MB, p, mbuf_pkthdr_header(p), ETHER_HDR_LEN);
			bpf_in++;
		}
		ifnet_input(ifp, head, &st);
	}
}

static errno_t
xc5_if_output(ifnet_t ifp, mbuf_t m)
{
	static uint8_t buf[1600];
	size_t len = mbuf_pkthdr_len(m);
	bpf_tap_out(ifp, DLT_EN10MB, m, NULL, 0);
	bpf_out++;
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
	/*
	 * wifi48/49n: joined from Settings, iOS sent IPv6 but never a DHCP DISCOVER, and Settings never showed the network
	 * as joined: IPConfiguration does DHCP through BPF (bootp over /dev/bpf on the interface), and this interface had
	 * none attached. Attached here; frames in and out are tapped (bpf_tap_in / bpf_tap_out).
	 */
	if (if_err == 0) {
		bpf_err = bpf_attach(ifp, DLT_EN10MB, sizeof(struct ether_header), NULL, NULL);
	}
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
	net_addr();
}

static void
net_addr(void)
{
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
	if ((!scan_inflight && scan_done != 0 && now - scan_done_ns < 20000000000ull) || (wifi_shadow && ap_sel >= 0 && scan_done != 0)) {
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
	join_sec = what->security;
	join_len = what->ssid_length;
	join_bssid_given = (what->bssid[0] | what->bssid[1] | what->bssid[2] | what->bssid[3] | what->bssid[4] |
	    what->bssid[5]) != 0;
	join_stage = 1;
	if (!sta_wait_ready()) {
		return join_err = EBUSY;
	}
	join_stage = 2;
	if (what->security == REDWOOD_STA_WPA2_PSK) {
		if (what->passphrase_length >= 8) {
			wpa_pbkdf2(what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, wpa_pmk);
		} else {
			memcpy(wpa_pmk, what->pmk, sizeof(wpa_pmk));
		}
	} else if (what->security != REDWOOD_STA_OPEN) {
		return join_err = EOPNOTSUPP;
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
		join_stage = 3;
		return join_err = ENOENT;
	}
	if (wifi_shadow && ap_sel >= 0 && conn_ind == 0 && (!wpa_on || wpa_done) && aps[ap_sel].ssid_len == what->ssid_length &&
	    memcmp(aps[ap_sel].raw_ssid, what->ssid, what->ssid_length) == 0) {
		/* already joined by the kernel (shadow): nothing to send, the link comes up for iOS now */
		shadow_used++;
		join_stage = 6;
		join_err = 0;
		sta_phase = REDWOOD_STA_JOINED;
		link_state(true);
		printf("XC5 WLBT: joined (from Settings, already joined by the kernel)\n");
		return 0;
	}
	if (wifi_shadow && ap_sel >= 0 && conn_ind == 0) {
		/* another network asked for: the kernel's join is left first (then the usual scan + CONNECT) */
		uint8_t s[32] = { 0x0e, 0x20 };
		s[10] = 1;
		lck_mtx_lock(&xc5w_mtx);
		memcpy(s + 12, aps[ap_sel].bssid, 6);
		s[18] = 3;
		memcpy(leave_sig, s, sizeof(s));
		want_leave = true;
		conn_ind = 0xffff;
		ap_sel = -1;
		lck_mtx_unlock(&xc5w_mtx);
		delay(300000);
	}
	join_stage = 4;
	lck_mtx_lock(&xc5w_mtx);
	wpa_on = what->security == REDWOOD_STA_WPA2_PSK;
	wpa_done = false;
	wpa_install_at = 0;
	wpa_gtk_len = 0;
	conn_cfm = 0xffff;
	conn_ind = 0xffff;
	sta_phase = REDWOOD_STA_JOINING;
	want_join = k;
	lck_mtx_unlock(&xc5w_mtx);
	wait_until((volatile uint32_t *)&conn_ind, 0xffff, 25000);     /* (a scan comes first now) */
	join_stage = 5;
	if (conn_ind != 0) {
		sta_phase = REDWOOD_STA_IDLE;
		/* said at once on the console (the heartbeat line came too seldom to be filmed, wifi31/32) */
		printf("XC5 WLBT: JOIN FAILED cfm %x ind %x del %x scans done %u sent %u got %u\n", conn_cfm, conn_ind, del_cfm,
		    scan_done, mlme_sent, mlme_got);
		return join_err = conn_ind == 0xffff ? ETIMEDOUT : ECONNREFUSED;
	}
	if (wpa_on) {
		/* the 4-way handshake: messages 1 and 3 come within a second or two; none, or a bad MIC: a wrong passphrase */
		wait_until((volatile uint32_t *)&wpa_done, 0, 10000);
		if (!wpa_done) {
			join_stage = 7;
			printf("XC5 WLBT: WPA2 FAILED m1 %u m3 %u mic bad %u unwrap bad %u\n", wpa_m1, wpa_m3, wpa_mic_bad,
			    wpa_unwrap_bad);
			sta_leave_call();
			wpa_on = false;
			return join_err = EAUTH;
		}
	}
	join_err = 0;
	sta_phase = REDWOOD_STA_JOINED;
	link_state(true);
	printf("XC5 WLBT: joined (from Settings)%s\n", wpa_on ? " with WPA2" : "");
	return 0;
}

int
sta_leave_call(void)
{
	ui_leaves++;
	if (ap_sel >= 0 && conn_ind == 0 && (!wifi_shadow || wpa_on)) {
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
	apmbox = (volatile uint8_t *)ml_io_map(0x119B0000, 0x1000);    /* MAILBOX_APM2WLBT (mif_reg_S5E3830.h) */
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
	/*
	 * wifi37-40: in the Settings mode the core stops 1-13 s after its first scan (q0 never read again, the doorbell left
	 * pending, no panic, PMU STATES unchanged), while connected (auto mode) it runs for minutes. Samsung's reset-release
	 * sequence (platform_mif_3830.c comment, PMUCAL v2) also has MIF_CTRL 0x3810 [0], TCXO_BUF_CTRL 0x3b78 [0] and
	 * WLBT_INT_EN 0x3144 [3] [5] -- the memory interface and the 26 MHz clock kept for WLBT and its wake-up interrupts --
	 * which this file never set: a core that idles may lose its clock or memory and not come back. Set here
	 * (xc5wpmu=0: not); the values before and after are said.
	 */
	pmu_before[0] = rd(pmu, 0x3810);
	pmu_before[1] = rd(pmu, 0x3b78);
	pmu_before[2] = rd(pmu, 0x3144);
	if (wifi_pmu) {
		upd(pmu, 0x3810, 1u, 1u);
		upd(pmu, 0x3b78, 1u, 1u);
		upd(pmu, 0x3144, (1u << 3) | (1u << 5), (1u << 3) | (1u << 5));
	}
	printf("XC5 WLBT: MIF_CTRL %#x -> %#x, TCXO_BUF_CTRL %#x -> %#x, WLBT_INT_EN %#x -> %#x\n", pmu_before[0],
	    rd(pmu, 0x3810), pmu_before[1], rd(pmu, 0x3b78), pmu_before[2], rd(pmu, 0x3144));
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

	{
		uint64_t ns0;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns0);
		t_loop1_s = (uint32_t)(ns0 / 1000000000ull);
	}
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
	{
		uint64_t ns0;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns0);
		t_loop2_s = (uint32_t)(ns0 / 1000000000ull);
	}
	/* xc5wpoll=100: this loop at the first one's pace (the firmware stopped a few seconds after the change, each boot) */
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
			if (xc5_ifp != NULL && dhcp_state == 3 && addr_err == -1) {
				ifnet_set_flags(xc5_ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
				net_addr();
			}
		}
		ping_tick();
		if (tick % 2000 != 0 || i >= 9) {
			delay(poll_ms * 1000);
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
	/* (both modes) the control slots, when the two poll loops began, and where the firmware stopped reading */
	printf("XC5 w bpf %d in %u out %u | wpa %d done %d m1 %u m3 %u g1 %u micbad %u unwrapbad %u keys %u res %x gc %x gtk %u | pmk %x offload %u ind %u rf %u\n", bpf_err, bpf_in, bpf_out,
	    wpa_on, wpa_done, wpa_m1, wpa_m3, wpa_g1, wpa_mic_bad, wpa_unwrap_bad, wpa_keys_sent, wpa_keys_res, wpa_group_cipher, wpa_gtk_len, wpa_pmk_res, wpa_offload, rx_ind_n, rx_rf_n);
	printf("XC5 w txmac ios %02x:%02x:%02x:%02x:%02x:%02x foreign %u mapped %u | types %x %x %x %x n %u | cfm %x rx %u\n",
	    ios_mac[0], ios_mac[1], ios_mac[2], ios_mac[3], ios_mac[4], ios_mac[5], tx_foreign, rx_mapped, tx_types[0],
	    tx_types[1], tx_types[2], tx_types[3], tx_ntypes, data_cfm, data_rx);
	printf("XC5 w shadow tries %u ok %u used %u | ap %d ind %x\n", shadow_tries, shadow_ok, shadow_used, ap_sel, conn_ind);
	printf("XC5 w country %x | wdog %u at %u s alive %u at %u s | pmu STATUS %x STATES %x CTRL_NS %x CTRL_S %x DEBUG %x\n", country_res, wdog_n, wdog_at_s,
	    alive_n, alive_at_s, rd(pmu, WLBT_STATUS), rd(pmu, WLBT_STATES), rd(pmu, WLBT_CTRL_NS), rd(pmu, WLBT_CTRL_S), rd(pmu, WLBT_DEBUG));
	printf("XC5 w ctl back %u force %u busy %x | loop1 %u s loop2 %u s poll %u ms | q0 w%u r%u ka %u/%u stall at %llu s kick %u\n",
	    ctl_back, ctl_force, ctl_busy, t_loop1_s, t_loop2_s, poll_ms, *scb(0, 0), *scb(0, 1), ka_sent, ka_got, q_stall_at_s,
	    q_kicks);
	if (wifi_ui) {
		extern unsigned xc5_a80211_requests, xc5_a80211_refused, xc5_a80211_results, xc5_a80211_given, xc5_a80211_scandone;
		extern int xc5_a80211_last_type, xc5_a80211_last_error;
		printf("XC5 w a80211 req %u refused %u (last %d err %d) | scan aps %u done %u ev %u | results asked %u given %u\n",
		    xc5_a80211_requests, xc5_a80211_refused, xc5_a80211_last_type, xc5_a80211_last_error, ap_count, scan_done,
		    xc5_a80211_scandone, xc5_a80211_results, xc5_a80211_given);
		/* wifi30: S24 listed, its join failed with nothing sent: how far a join came, and why it stopped */
		extern unsigned xc5_a80211_joins;
		extern int xc5_a80211_join_why, xc5_a80211_join_cipher, xc5_a80211_join_keylen;
		printf("XC5 w ui join assoc %u why %d key %d | stage %d err %d | cfm %x ind %x | del %x next %d\n",
		    xc5_a80211_joins, xc5_a80211_join_why, xc5_a80211_join_keylen, join_stage, join_err, conn_cfm, conn_ind, del_cfm,
		    del_next);
		/* wifi26: scans asked by wifid never ended (done 0, aps 0): what the firmware said to them */
		printf("XC5 w fw vif %x scancfm %x ind %u sent %u got %u irqclr %u sr0 %x ids %x %x %x %x\n", vif_res, scan_res, scan_inds, mlme_sent,
		    mlme_got, irq_cleared, rd(mbox, INTSR0), mlme_ids[(mlme_got + 4) % 8], mlme_ids[(mlme_got + 5) % 8], mlme_ids[(mlme_got + 6) % 8],
		    mlme_ids[(mlme_got + 7) % 8]);
		/* wifi33: whether the firmware still takes what we queue (FH ctrl r/w) and what it last logged */
		printf("XC5 w rfb %u chained %u full %u | keep idle %x %x %x set %x | done %u chain %u abort %u onrun %u ka %u/%u | q0 w%u r%u q3 w%u r%u | fw: %.48s\n", rfb_back, rfb_chained, rfb_full, mib_idle, mib_staidle, mib_idlelite, set_res, scan_done, chain_scans,
		    scan_aborts, join_on_running, ka_sent, ka_got, *scb(0, 0), *scb(0, 1), *scb(3, 0), *scb(3, 1), loglines[log_recs % LOGN]);
		/*
		 * wifi34: scans went on (done 22) until the firmware stopped reading the FH control queue (q0 w47 r46, from 44 s
		 * to 59 s) and sent nothing more: asleep (PMU STATES), hung or panicked (panic words), or our doorbell left
		 * pending (INTSR1 bit 5 still set) -- and whether ringing it again (kick) brought it back.
		 */
		const uint32_t *pw = (const uint32_t *)(shm + hb_panic_off);
		printf("XC5 w fault n %u id %x count %u arg %x cpu %u | pmu mif %x>%x tcxo %x>%x inten %x>%x\n", fault_n, fault_id,
		    fault_count, fault_arg, fault_cpu, pmu_before[0], rd(pmu, 0x3810), pmu_before[1], rd(pmu, 0x3b78), pmu_before[2],
		    rd(pmu, 0x3144));
		printf("XC5 w why st %x sr1 %x msr1 %x issr0 %x logw %u recs %u kick %u/%u stall %ums pan %x %x\n",
		    rd(pmu, WLBT_STATES), rd(mbox, INTSR1), rd(mbox, INTMSR1), rd(mbox, ISSR(0)),
		    *(volatile uint32_t *)(shm + conf->mxlog.buf.write_index_loc), log_recs, q_kicks, q_kick_ok, q_stall_ms,
		    hb_panic_off ? pw[0] : 0, hb_panic_off ? pw[1] : 0);
		/*
		 * wifi35r: the halt came ~35 s after boot with or without our RTC code (xc5rtc=0). The firmware reaches the
		 * PMIC through the APM ("the APM IPC in FW will be used instead", platform_mif_3830.c): the APM2WLBT mailbox
		 * says whether it asked the APM and was not answered. And the uptime at which the queue stopped.
		 */
		if (apmbox != NULL) {
			printf("XC5 w apm sr0 %x msr0 %x sr1 %x msr1 %x issr %x %x %x %x | stall at %llu s\n", rd(apmbox, INTSR0),
			    rd(apmbox, INTMSR0), rd(apmbox, INTSR1), rd(apmbox, INTMSR1), rd(apmbox, ISSR(0)), rd(apmbox, ISSR(1)),
			    rd(apmbox, ISSR(2)), rd(apmbox, ISSR(3)), q_stall_at_s);
		}
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
	PE_parse_boot_arg_str("xc5ssid", want_ssid, sizeof(want_ssid));
	/* (PE_parse_boot_argn takes an all-digit value as a number: "12345678" came out as binary, wifi52) */
	PE_parse_boot_arg_str("xc5psk", want_psk, sizeof(want_psk));
	PE_parse_boot_argn("xc5wretry", &rate_retry, sizeof(rate_retry));
	PE_parse_boot_argn("xc5wifiui", &wifi_ui, sizeof(wifi_ui));
	PE_parse_boot_argn("xc5wchain", &wifi_chain, sizeof(wifi_chain));
	PE_parse_boot_argn("xc5wka", &ka_ms, sizeof(ka_ms));
	PE_parse_boot_argn("xc5wpoll", &poll_ms, sizeof(poll_ms));
	PE_parse_boot_argn("xc5widle", &wifi_idle, sizeof(wifi_idle));
	PE_parse_boot_argn("xc5wpmu", &wifi_pmu, sizeof(wifi_pmu));
	PE_parse_boot_argn("xc5wcountry", &wifi_country, sizeof(wifi_country));
	PE_parse_boot_argn("xc5wshadow", &wifi_shadow, sizeof(wifi_shadow));
	PE_parse_boot_argn("xc5wmacfix", &wifi_macfix, sizeof(wifi_macfix));
	PE_parse_boot_argn("xc5wdhcp", &wifi_kdhcp, sizeof(wifi_kdhcp));
	thread_t thread;
	if (kernel_thread_start(wlbt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
