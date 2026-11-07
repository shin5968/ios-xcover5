/*
 * XC5GPU: the Galaxy XCover5's Mali-G52 MP1 (Exynos 850) for user space, the
 * kernel half of a Panfrost-style driver (Mesa's panfrost in an iOS process).
 * Copied into the xnu tree as iokit/Kernel/arm/XC5GPU.cpp by
 * tools/xcover5/apply-xcover5.py (patch 0125).
 *
 * Sources: the IOKit face and its selector numbers follow HeroGPU.cpp of
 * wardevsa/ios-a5xelte (branch herolte, patches 0239/0240/0248), whose
 * SweetSurface kernel this tree shares, so its libdrm stand-in (herodrm.c)
 * can talk to this service. The GPU side follows Luminous418/hakios-a32's
 * bring-up of the same Mali-G52 (kernel/bsd/dev/hakios.mali.c): AArch64 4K
 * page tables walked uncached, one address space, polled job slots.
 *
 * Boot 44 found S-Boot leaves the G3D power domain and clock on; boot 46 ran
 * WRITE_VALUE jobs in identity mode and through page tables (xc5gpu=3, in
 * SweetPlatformExpert.cpp). Nothing outside the GPU block is written here.
 *
 * Boot-arg xc5gpu=4 starts the service (absent or lower: it declines to
 * start and the CPU display path is untouched). IOServiceMatching("XC5GPU"),
 * open type 0, then
 *
 *   0 GetParam    [param]           -> [value]   Panfrost DRM param numbers
 *   1 CreateBO    [size]            -> [handle, gpu_va, size]
 *   2 FreeBO      [handle]
 *   3 MapBO       [handle]          -> [user address]  (write-combined)
 *   4 Submit      [jc_va, js]       -> [status, us]    js 0 fragment, 1 the rest
 *   5 Log         struct: text      (to the kernel log; test programs)
 *   6 SurfaceBO   [IOSurface id]    -> [handle, gpu_va, size, bytes per row, width,
 *                                       height, pixel format, offset, planes]
 *   7 SyncBO      [handle]          clean+invalidate an imported surface's CPU caches
 *
 * Submit is synchronous: status 0 = the chain finished cleanly, else the
 * job slot's status (~0 if it never finished). GPU addresses are never
 * reused, so a translation fault names a stale or overrun buffer. The Mali
 * is not IO-coherent (COHERENCY_FEATURES 0): buffers are zeroed and cleaned
 * once, then reach user space write-combined.
 */
#include <pexpert/arm64/board_config.h>

#if defined(XCOVER5)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include "SweetSurface.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
uint64_t kvtophys(vm_offset_t va);
vm_offset_t phystokv(uint64_t pa);
void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
void thread_deallocate(thread_t);
/* For the heartbeat (SweetPlatformExpert.cpp): state, selftest, counters. */
uint32_t xcover5_gpusvc[8];
/* The last four client Log lines (Mesa's test results), and a counter. */
char xcover5_gpusvc_lines[4][120];
uint32_t xcover5_gpusvc_line;
/*
 * All client Log lines for xc5_drop.c's report when the GL backboardd exits: the
 * first XC5_GLLOG_FIRST of the boot, and a ring of the last XC5_GLLOG_RING. The
 * console scrolls them away long before anyone can film them.
 */
#define XC5_GLLOG_FIRST 24
#define XC5_GLLOG_RING  40
char xcover5_gllog_first[XC5_GLLOG_FIRST][100];
char xcover5_gllog_ring[XC5_GLLOG_RING][100];
uint32_t xcover5_gllog_n;
}

/* ---- the GPU ---- */

#define MALI_PHYS           0x11500000u
#define MALI_SIZE           0x5000u
#define PMU_G3D_PAGE        0x11861000u     /* pd-g3d config +0xd00, status +0xd04 */

#define GPU_ID              0x000
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define SHADER_PRESENT      0x100
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS_BASE             0x1800
#define JS_STRIDE           0x80
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, flush both ends */
#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define MMU_INT_MASK        0x2008
#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_FAULTADDR_LO     0x20
#define AS_FAULTADDR_HI     0x24
#define AS_STATUS           0x28
#define AS_TRANSCFG_LO      0x30
#define AS_TRANSCFG_HI      0x34
#define AS_CMD_UPDATE       1
#define AS_CMD_LOCK         2
#define AS_CMD_FLUSH_PT     4
/* AArch64 4K, 55 - 35 input bits (three levels), table walks not cached:
 * a new entry needs no MMU command (hakios-a32's mali_fastmap). */
#define TRANSCFG            (6u | (20u << 6) | (1u << 24))
#define PTE_TABLE           3ull
#define PTE_PAGE            (3ull | (1ull << 10) | (3ull << 8) | (1ull << 6))  /* AF, inner shareable, RW */
#define PTE_ADDRESS         0x0000fffffffff000ull
#define VA_FIRST            0x01000000ull
#define VA_END              (1ull << 35)

static vm_offset_t mali;
static uint32_t shader_mask, tiler_mask, l2_mask;
static int up_state;                    /* 0 not tried, 1 up, -1 failed */
static uint64_t *pt_top;
static uint64_t pt_top_phys;
static uint64_t next_va = VA_FIRST;
static IOLock *map_lock, *job_lock;

static inline uint32_t
rd(uint32_t at)
{
	return *(volatile uint32_t *)(mali + at);
}

static inline void
wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(mali + at) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
wait_bits(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (int i = 0; i < 20; i++) {
		if ((rd(at) & mask) == want) {
			return true;
		}
		IODelay(10);
	}
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(at) & mask) == want) {
			return true;
		}
		IOSleep(1);
	}
	return (rd(at) & mask) == want;
}

static void
as0_program(void)
{
	wr(AS0 + AS_TRANSTAB_LO, (uint32_t)pt_top_phys);
	wr(AS0 + AS_TRANSTAB_HI, (uint32_t)(pt_top_phys >> 32));
	wr(AS0 + AS_TRANSCFG_LO, TRANSCFG);
	wr(AS0 + AS_TRANSCFG_HI, 0);
	wr(AS0 + AS_MEMATTR_LO, 0x88888888u);
	wr(AS0 + AS_MEMATTR_HI, 0x88888888u);
	wr(AS0 + AS_COMMAND, AS_CMD_UPDATE);
	(void)wait_bits(AS0 + AS_STATUS, 1, 0, 50);
}

/* Soft reset, all cores powered, address space 0 on our tables. */
static bool
gpu_reset_and_power(void)
{
	wr(GPU_INT_MASK, 0);
	wr(GPU_INT_CLEAR, 0xffffffffu);
	wr(GPU_CMD, 1);
	if (!wait_bits(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 100)) {
		IOLog("XC5GPU: soft reset did not complete (raw %x)\n", rd(GPU_INT_RAWSTAT));
		return false;
	}
	wr(GPU_INT_CLEAR, 0xffffffffu);
	wr(JOB_INT_MASK, 0);
	wr(MMU_INT_MASK, 0);
	wr(L2_PWRON, l2_mask);
	wr(TILER_PWRON, tiler_mask);
	wr(SHADER_PWRON, shader_mask);
	if (!wait_bits(L2_READY, l2_mask, l2_mask, 50) || !wait_bits(TILER_READY, tiler_mask, tiler_mask, 50) ||
	    !wait_bits(SHADER_READY, shader_mask, shader_mask, 50)) {
		IOLog("XC5GPU: cores not ready: l2 %x tiler %x shaders %x\n", rd(L2_READY), rd(TILER_READY),
		    rd(SHADER_READY));
		return false;
	}
	as0_program();
	return true;
}

static uint64_t *
pt_new(uint64_t *phys)
{
	uint64_t *table = (uint64_t *)IOMallocAligned(PAGE_SIZE, PAGE_SIZE);
	if (table == NULL) {
		return NULL;
	}
	bzero(table, PAGE_SIZE);
	FlushPoC_DcacheRegion((vm_offset_t)table, PAGE_SIZE);
	*phys = kvtophys((vm_offset_t)table);
	return table;
}

static bool
gpu_up(void)
{
	if (up_state != 0) {
		return up_state == 1;
	}
	up_state = -1;
	vm_offset_t pmu = ml_io_map(PMU_G3D_PAGE, 0x1000);
	if (pmu == 0 || !(*(volatile uint32_t *)(pmu + 0xd04) & 1)) {
		IOLog("XC5GPU: G3D power domain off\n");
		return false;
	}
	mali = ml_io_map(MALI_PHYS, MALI_SIZE);
	pt_top = mali ? pt_new(&pt_top_phys) : NULL;
	if (mali == 0 || pt_top == NULL) {
		IOLog("XC5GPU: mapping failed\n");
		return false;
	}
	shader_mask = rd(SHADER_PRESENT);
	tiler_mask = rd(TILER_PRESENT);
	l2_mask = rd(L2_PRESENT);
	if (!gpu_reset_and_power()) {
		return false;
	}
	up_state = 1;
	IOLog("XC5GPU: up, GPU_ID %x, shaders %x, user VA %#llx..%#llx\n", rd(GPU_ID), shader_mask,
	    VA_FIRST, VA_END);
	return true;
}

static uint64_t
va_alloc(uint64_t bytes)
{
	IOLockLock(map_lock);
	uint64_t va = (next_va + 0xffff) & ~0xffffull;
	uint64_t end = va + ((bytes + 0xfff) & ~0xfffull);
	if (up_state != 1 || bytes == 0 || end > VA_END) {
		va = 0;
	} else {
		next_va = end;
	}
	IOLockUnlock(map_lock);
	return va;
}

/* The table below `entry`, made if missing (map_lock held). */
static uint64_t *
pt_below(uint64_t *entry, bool make)
{
	if (*entry & 1) {
		return (uint64_t *)phystokv(*entry & PTE_ADDRESS);
	}
	if (!make) {
		return NULL;
	}
	uint64_t phys;
	uint64_t *table = pt_new(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* PTEs for n 4 KiB pages at va; pages == NULL removes them. */
static bool
pt_set(uint64_t va, const uint64_t *pages, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		uint64_t a = va + (uint64_t)i * 0x1000;
		if (a < VA_FIRST || a >= VA_END) {
			return false;
		}
		uint64_t *mid = pt_below(&pt_top[(a >> 30) & 31], pages != NULL);
		uint64_t *bot = mid ? pt_below(&mid[(a >> 21) & 511], pages != NULL) : NULL;
		if (bot == NULL) {
			if (pages == NULL) {
				continue;
			}
			return false;
		}
		uint64_t *e = &bot[(a >> 12) & 511];
		*e = pages ? ((pages[i] & PTE_ADDRESS) | PTE_PAGE) : 0;
		FlushPoC_DcacheRegion((vm_offset_t)e, sizeof(*e));
	}
	return true;
}

static bool
gpu_map(uint64_t va, const uint64_t *pages, unsigned n)
{
	IOLockLock(map_lock);
	bool ok = up_state == 1 && pt_set(va, pages, n);
	IOLockUnlock(map_lock);
	return ok;
}

/* Remove, then flush the walk caches over all of the user range (no job runs). */
static void
gpu_unmap(uint64_t va, unsigned n)
{
	IOLockLock(job_lock);
	IOLockLock(map_lock);
	if (up_state == 1) {
		(void)pt_set(va, NULL, n);
		wr(AS0 + AS_LOCKADDR_LO, 34);       /* the whole 2^35 range */
		wr(AS0 + AS_LOCKADDR_HI, 0);
		wr(AS0 + AS_COMMAND, AS_CMD_LOCK);
		(void)wait_bits(AS0 + AS_STATUS, 1, 0, 50);
		wr(AS0 + AS_COMMAND, AS_CMD_FLUSH_PT);
		(void)wait_bits(AS0 + AS_STATUS, 1, 0, 50);
	}
	IOLockUnlock(map_lock);
	IOLockUnlock(job_lock);
}

/*
 * Run the job chain at GPU VA jc on slot js and wait (bounded). 0 when it
 * finished cleanly, else JS_STATUS (or ~0 if it never finished); *us = time.
 * A fault or timeout resets the GPU so the next client starts clean.
 */
static uint32_t
gpu_submit(unsigned js, uint64_t jc, uint64_t *us)
{
	if (js > 1 || jc == 0) {
		return ~0u;
	}
	IOLockLock(job_lock);
	uint32_t status = ~0u;
	uint64_t start = mach_absolute_time();
	if (up_state == 1) {
		uint32_t slot = JS_BASE + js * JS_STRIDE;
		uint32_t done = 1u << js, failed = 1u << (js + 16);
		wr(JOB_INT_CLEAR, 0xffffffffu);
		wr(MMU_INT_CLEAR, 0xffffffffu);
		wr(slot + JS_HEAD_NEXT_LO, (uint32_t)jc);
		wr(slot + JS_HEAD_NEXT_HI, (uint32_t)(jc >> 32));
		wr(slot + JS_AFFINITY_NEXT_LO, shader_mask);
		wr(slot + JS_AFFINITY_NEXT_HI, 0);
		wr(slot + JS_CONFIG_NEXT, JS_CONFIG);
		wr(slot + JS_COMMAND_NEXT, 1);
		/* Stop on done or failed (boot 47 waited the full 2 s on a failure). */
		bool finished = false;
		for (uint32_t ms = 0; ms < 2000 && !finished; ms++) {
			for (int i = 0; i < 20 && !finished; i++) {
				finished = (rd(JOB_INT_RAWSTAT) & (done | failed)) != 0;
				if (!finished) {
					IODelay(10);
				}
			}
			if (!finished) {
				IOSleep(1);
			}
		}
		uint32_t raw = rd(JOB_INT_RAWSTAT), mmu = rd(MMU_INT_RAWSTAT);
		status = finished && !(raw & failed) && !mmu ? 0 : (rd(slot + JS_STATUS) ? rd(slot + JS_STATUS) : ~0u);
		if (status != 0) {
			xcover5_gpusvc[4]++;
			IOLog("XC5GPU: job JS%u at %#llx failed: irq %x mmu %x js_status %x fault %x@%#llx\n", js, jc,
			    raw, mmu, rd(slot + JS_STATUS), rd(AS0 + AS_FAULTSTATUS),
			    ((uint64_t)rd(AS0 + AS_FAULTADDR_HI) << 32) | rd(AS0 + AS_FAULTADDR_LO));
			IOLockLock(map_lock);
			if (!gpu_reset_and_power()) {
				up_state = -1;
			}
			IOLockUnlock(map_lock);
		}
		wr(JOB_INT_CLEAR, 0xffffffffu);
		wr(MMU_INT_CLEAR, 0xffffffffu);
		xcover5_gpusvc[3]++;
	}
	if (us) {
		absolutetime_to_nanoseconds(mach_absolute_time() - start, us);
		*us /= 1000;
	}
	IOLockUnlock(job_lock);
	return status;
}

/* Panfrost DRM_PANFROST_PARAM_* -> register offsets. */
static uint64_t
gpu_param(uint64_t param)
{
	static const uint16_t regs[] = {
		0x000,  /* 0 GPU_PROD_ID (GPU_ID >> 16, below) */
		0x000,  /* 1 GPU_REVISION (GPU_ID & 0xffff, below) */
		0x100,  /* 2 SHADER_PRESENT */
		0x110,  /* 3 TILER_PRESENT */
		0x120,  /* 4 L2_PRESENT */
		0xe00,  /* 5 STACK_PRESENT */
		0x018,  /* 6 AS_PRESENT */
		0x01c,  /* 7 JS_PRESENT */
		0x004,  /* 8 L2_FEATURES */
		0x008,  /* 9 CORE_FEATURES */
		0x00c,  /* 10 TILER_FEATURES */
		0x010,  /* 11 MEM_FEATURES */
		0x014,  /* 12 MMU_FEATURES */
		0x0ac,  /* 13 THREAD_FEATURES */
		0x0a0,  /* 14 MAX_THREADS */
		0x0a4,  /* 15 THREAD_MAX_WORKGROUP_SZ */
		0x0a8,  /* 16 THREAD_MAX_BARRIER_SZ */
		0x300,  /* 17 COHERENCY_FEATURES */
		0x0b0, 0x0b4, 0x0b8, 0x0bc,     /* 18-21 TEXTURE_FEATURES0-3 */
	};
	if (up_state != 1) {
		return 0;
	}
	uint32_t id = rd(GPU_ID);
	switch (param) {
	case 0:
		return id >> 16;
	case 1:
		return id & 0xffff;
	case 38:        /* NR_CORE_GROUPS: one L2 */
		return 1;
	case 39:        /* THREAD_TLS_ALLOC (Bifrost) */
		return rd(0x310);
	case 40:        /* AFBC_FEATURES */
		return rd(0x04c);
	}
	if (param < sizeof(regs) / sizeof(regs[0])) {
		return rd(regs[param]);
	}
	if (param >= 22 && param < 38) {
		return rd(0x0c0 + (uint32_t)(param - 22) * 4);  /* JS_FEATURES0-15 */
	}
	return 0;
}

/* ---- buffers ---- */

#define MAX_BOS         4096
#define MAX_BO_BYTES    (256ull << 20)

struct gpu_bo {
	IOBufferMemoryDescriptor *mem;          /* own buffer, or NULL for an imported surface */
	SweetSurface             *surface;      /* imported IOSurface (retained), or NULL */
	IOMemoryMap              *umap;
	uint64_t                  va;
	uint32_t                  pages;
};

static bool
bo_used(const struct gpu_bo *bo)
{
	return bo->mem != NULL || bo->surface != NULL;
}

static IOMemoryDescriptor *
bo_memory(const struct gpu_bo *bo)
{
	return bo->mem != NULL ? (IOMemoryDescriptor *)bo->mem :
	       bo->surface != NULL ? bo->surface->memory : NULL;
}

static void
cache_clean(uintptr_t k, uint64_t size)
{
	for (uintptr_t p = k & ~(uintptr_t)63; p < k + size; p += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Physical pages of a descriptor (4 KiB each) into the GPU at a new address. */
static bool
bo_map_pages(struct gpu_bo *bo, IOMemoryDescriptor *md, uint64_t size)
{
	bo->pages = (uint32_t)(size / 0x1000);
	uint64_t *pa = (uint64_t *)IOMallocData(bo->pages * sizeof(uint64_t));
	bool ok = pa != NULL;
	for (uint32_t i = 0; ok && i < bo->pages; i++) {
		IOByteCount len = 0;
		pa[i] = md->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);
		ok = pa[i] != 0 && (pa[i] & 0xfff) == 0;
		if (!ok) {
			printf("XC5GPU: page %u of %u: bad PA %#llx\n", i, bo->pages, pa[i]);
		}
	}
	bo->va = ok ? va_alloc(size) : 0;
	ok = bo->va != 0 && gpu_map(bo->va, pa, bo->pages);
	if (pa != NULL) {
		IOFreeData(pa, bo->pages * sizeof(uint64_t));
	}
	if (!ok) {
		bo->va = 0;
		bo->pages = 0;
	}
	return ok;
}

static bool
bo_create(struct gpu_bo *bo, uint64_t size)
{
	size = (size + 0xfff) & ~0xfffull;
	if (size == 0 || size > MAX_BO_BYTES) {
		return false;
	}
	/* Boot 52 panicked in phystokv_range (PA 0xc0e0f000, outside XNU's RAM)
	 * during Mesa's first CreateBO: say each step, on screen, until known. */
	static uint32_t traced;
	bool trace = traced++ < 8;
	if (trace) {
		printf("XC5GPU: CreateBO %llu bytes: allocating\n", size);
	}
	bo->mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, size, page_size);
	if (bo->mem == NULL) {
		return false;
	}
	if (trace) {
		IOByteCount len = 0;
		printf("XC5GPU: CreateBO: kva %p pa0 %#llx; zeroing\n", bo->mem->getBytesNoCopy(),
		    (uint64_t)bo->mem->getPhysicalSegment(0, &len, kIOMemoryMapperNone));
	}
	uintptr_t k = (uintptr_t)bo->mem->getBytesNoCopy();
	bzero((void *)k, size);
	cache_clean(k, size);
	if (trace) {
		printf("XC5GPU: CreateBO: zeroed and cleaned; mapping\n");
	}
	if (!bo_map_pages(bo, bo->mem, size)) {
		OSSafeReleaseNULL(bo->mem);
		return false;
	}
	if (trace) {
		printf("XC5GPU: CreateBO: GPU VA %#llx\n", bo->va);
	}
	xcover5_gpusvc[2]++;
	return true;
}

/* IOSurface `id`'s pixels for the GPU; the surface stays retained until freed. */
static bool
bo_import_surface(struct gpu_bo *bo, uint32_t id)
{
	SweetSurface *s = sweet_surface_copy(id);
	if (s == NULL || s->memory == NULL || s->allocSize == 0) {
		OSSafeReleaseNULL(s);
		return false;
	}
	uint64_t size = ((uint64_t)s->allocSize + 0xfff) & ~0xfffull;
	if (size > s->memory->getLength() || size > MAX_BO_BYTES || !bo_map_pages(bo, s->memory, size)) {
		s->release();
		return false;
	}
	/* Dirty CPU lines written back later would land over the GPU's pixels. */
	uintptr_t k = s->kernelAddress();
	if (k != 0) {
		cache_clean(k, s->allocSize);
	}
	bo->surface = s;
	return true;
}

static void
bo_free(struct gpu_bo *bo)
{
	if (!bo_used(bo)) {
		return;
	}
	gpu_unmap(bo->va, bo->pages);
	OSSafeReleaseNULL(bo->umap);
	OSSafeReleaseNULL(bo->mem);
	OSSafeReleaseNULL(bo->surface);
	bzero(bo, sizeof(*bo));
}

/* ---- IOKit ---- */

enum {
	kSelGetParam = 0,
	kSelCreateBO = 1,
	kSelFreeBO   = 2,
	kSelMapBO    = 3,
	kSelSubmit   = 4,
	kSelLog      = 5,
	kSelSurfaceBO = 6,
	kSelSyncBO   = 7,
};

class XC5GPU : public IOService {
	OSDeclareDefaultStructors(XC5GPU);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(XC5GPU, IOService);

class XC5GPUUserClient : public IOUserClient {
	OSDeclareDefaultStructors(XC5GPUUserClient);

	task_t         fTask;
	IOLock        *fLock;
	struct gpu_bo *fBOs;            /* handle = index + 1 */

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(XC5GPUUserClient, IOUserClient);

static void selftest(void *arg, wait_result_t wr);

/*
 * Metal's device discovery (after y2sios MiniIOMobileFramebuffer.cpp, MIT): MTLRegisterDevices
 * enumerates IOServiceMatching("IOAcceleratorES"), reads "MetalPluginName" off each service,
 * opens "/System/Library/Extensions/" + it + ".bundle" with NSBundle and makes the class
 * "MetalPluginClassName" names (or the bundle's principal class) with -initWithAcceleratorPort:.
 *
 * Boot-arg xc5metal=1 publishes the service with both properties pointing at the drop zone's
 * RedwoodMetal.bundle, readable only by the processes boot-arg xc5metalfor names (below).
 * A process may still change them (IORegistryEntrySetCFProperties; an empty string removes
 * one). Only those two keys, only strings.
 */
#define kXC5MetalNameKey  "MetalPluginName"
#define kXC5MetalClassKey "MetalPluginClassName"

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const char *aKey) const APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const OSString *aKey) const APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const OSSymbol *aKey) const APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const char *aKey, const IORegistryPlane *plane,
	    IOOptionBits options) const APPLE_KEXT_OVERRIDE;
	bool serializeProperties(OSSerialize *s) const APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Boot 78 (xc5metal=1 xc5mtl=1): chronod made the device, a buffer and a command buffer -- and
 * SpringBoard, which asked Metal while the properties were up, took the same device and aborted
 * (MPSImageGaussianBlur, SiriUI's pipelines, "<MTLDepthStencilState> ... not in this process").
 * So the two properties are now seen only by the processes named in boot-arg xc5metalfor
 * (comma separated, default "chronod"); everybody else reads an accelerator without them and
 * goes on as before. The kernel itself sees everything.
 */
static bool
xc5_metal_key(const char *key)
{
	return key != NULL && (strcmp(key, kXC5MetalNameKey) == 0 || strcmp(key, kXC5MetalClassKey) == 0);
}

static bool
xc5_metal_caller_allowed(void)
{
	if (proc_selfpid() == 0) {
		return true;
	}
	char who[MAXCOMLEN + 1];
	char list[128] = "chronod";
	proc_selfname(who, sizeof(who));
	(void)PE_parse_boot_argn("xc5metalfor", list, sizeof(list));
	size_t wl = strlen(who);
	for (const char *p = list; *p != '\0';) {
		const char *e = strchr(p, ',');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n == wl && strncmp(p, who, n) == 0) {
			return true;
		}
		if (e == NULL) {
			break;
		}
		p = e + 1;
	}
	return false;
}

OSObject *
IOAcceleratorES::copyProperty(const char *aKey) const
{
	return xc5_metal_key(aKey) && !xc5_metal_caller_allowed() ? NULL : IOService::copyProperty(aKey);
}

OSObject *
IOAcceleratorES::copyProperty(const OSString *aKey) const
{
	return aKey && xc5_metal_key(aKey->getCStringNoCopy()) && !xc5_metal_caller_allowed()
	       ? NULL : IOService::copyProperty(aKey);
}

OSObject *
IOAcceleratorES::copyProperty(const OSSymbol *aKey) const
{
	return aKey && xc5_metal_key(aKey->getCStringNoCopy()) && !xc5_metal_caller_allowed()
	       ? NULL : IOService::copyProperty(aKey);
}

OSObject *
IOAcceleratorES::copyProperty(const char *aKey, const IORegistryPlane *plane, IOOptionBits options) const
{
	return xc5_metal_key(aKey) && !xc5_metal_caller_allowed()
	       ? NULL : IOService::copyProperty(aKey, plane, options);
}

bool
IOAcceleratorES::serializeProperties(OSSerialize *s) const
{
	if (xc5_metal_caller_allowed()) {
		return IOService::serializeProperties(s);
	}
	OSDictionary *dict = dictionaryWithProperties();
	if (dict == NULL) {
		return false;
	}
	dict->removeObject(kXC5MetalNameKey);
	dict->removeObject(kXC5MetalClassKey);
	bool ok = dict->serialize(s);
	dict->release();
	return ok;
}

IOReturn
IOAcceleratorES::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	char who[MAXCOMLEN + 1];

	proc_selfname(who, sizeof(who));
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	OSCollectionIterator *it = OSCollectionIterator::withCollection(dict);
	if (it == NULL) {
		return kIOReturnNoMemory;
	}
	OSSymbol *key;
	IOReturn ret = kIOReturnSuccess;
	while ((key = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL) {
		if (!key->isEqualTo(kXC5MetalNameKey) && !key->isEqualTo(kXC5MetalClassKey)) {
			printf("XC5 metal: %s: refused key %s\n", who, key->getCStringNoCopy());
			ret = kIOReturnUnsupported;
		} else if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			printf("XC5 metal: %s: %s is not a string\n", who, key->getCStringNoCopy());
			ret = kIOReturnBadArgument;
		}
	}
	it->release();
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	it = OSCollectionIterator::withCollection(dict);
	if (it == NULL) {
		return kIOReturnNoMemory;
	}
	while ((key = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL) {
		OSString *value = OSDynamicCast(OSString, dict->getObject(key));
		if (value->getLength() == 0) {
			removeProperty(key);
			printf("XC5 metal: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else {
			setProperty(key, value);
			printf("XC5 metal: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		}
	}
	it->release();
	return kIOReturnSuccess;
}

static void
publish_accelerator(IOService *provider)
{
	int metal = 0;
	if (!PE_parse_boot_argn("xc5metal", &metal, sizeof(metal)) || metal == 0) {
		return;
	}
	IOAcceleratorES *acc = OSTypeAlloc(IOAcceleratorES);
	if (acc != NULL && acc->init() && acc->attach(provider)) {
		acc->setName("sgx");
		/* the drop zone's bundle (gpu/metal/build-metal.sh), seen only by xc5metalfor's processes */
		acc->setProperty(kXC5MetalNameKey, "../../../private/var/xc5/metal/RedwoodMetal");
		acc->setProperty(kXC5MetalClassKey, "RedwoodMetalDevice");
		acc->registerService();
		char list[128] = "chronod";
		(void)PE_parse_boot_argn("xc5metalfor", list, sizeof(list));
		printf("XC5 metal: IOAcceleratorES published (xc5metal=%d) for %s\n", metal, list);
	}
	OSSafeReleaseNULL(acc);
}

bool
XC5GPU::start(IOService *provider)
{
	int mode = 0;
	(void)PE_parse_boot_argn("xc5gpu", &mode, sizeof(mode));
	if (mode < 4 || !IOService::start(provider)) {
		return false;
	}
	map_lock = IOLockAlloc();
	job_lock = IOLockAlloc();
	if (map_lock == NULL || job_lock == NULL || !gpu_up()) {
		xcover5_gpusvc[0] = 2;
		return false;
	}
	xcover5_gpusvc[0] = 1;
	setProperty(kIOUserClientClassKey, "XC5GPUUserClient");
	setProperty("GPU", "Mali-G52 MP1");
	registerService();
	IOLog("XC5GPU: registered\n");
	publish_accelerator(this);
	thread_t thread;
	if (kernel_thread_start(selftest, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}

bool
XC5GPUUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fBOs = (struct gpu_bo *)IOMallocZeroData(MAX_BOS * sizeof(*fBOs));
	xcover5_gpusvc[1]++;
	return fLock != NULL && fBOs != NULL && up_state == 1;
}

IOReturn
XC5GPUUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

void
XC5GPUUserClient::free(void)
{
	if (fBOs != NULL) {
		for (unsigned i = 0; i < MAX_BOS; i++) {
			bo_free(&fBOs[i]);
		}
		IOFreeData(fBOs, MAX_BOS * sizeof(*fBOs));
		fBOs = NULL;
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
XC5GPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t out[9] = { 0 };
	uint32_t outCount = 0;
	IOReturn ret = kIOReturnSuccess;
	uint64_t in0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
	uint64_t in1 = args->scalarInputCount > 1 ? args->scalarInput[1] : 0;
	struct gpu_bo *bo = in0 && in0 <= MAX_BOS ? &fBOs[in0 - 1] : NULL;

	IOLockLock(fLock);
	switch (selector) {
	case kSelGetParam:
		out[0] = gpu_param(in0);
		outCount = 1;
		break;
	case kSelCreateBO:
	case kSelSurfaceBO: {
		unsigned i = 0;
		while (i < MAX_BOS && bo_used(&fBOs[i])) {
			i++;
		}
		if (i == MAX_BOS) {
			ret = kIOReturnNoResources;
			break;
		}
		struct gpu_bo *n = &fBOs[i];
		if (selector == kSelCreateBO ? !bo_create(n, in0) : !bo_import_surface(n, (uint32_t)in0)) {
			ret = selector == kSelCreateBO ? kIOReturnNoMemory : kIOReturnNotFound;
			break;
		}
		out[0] = i + 1;
		out[1] = n->va;
		out[2] = (uint64_t)n->pages * 0x1000;
		outCount = 3;
		if (n->surface != NULL) {
			SweetSurface *s = n->surface;
			out[3] = s->bytesPerRow;
			out[4] = s->width;
			out[5] = s->height;
			out[6] = s->pixelFormat;
			out[7] = s->offset;
			out[8] = s->planeCount;
			outCount = 9;
		}
		break;
	}
	case kSelFreeBO:
		if (bo == NULL || !bo_used(bo)) {
			ret = kIOReturnBadArgument;
		} else {
			bo_free(bo);
		}
		break;
	case kSelMapBO:
		if (bo == NULL || !bo_used(bo)) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (bo->umap == NULL) {
			/* A surface keeps the cache mode its other mappings use. */
			bo->umap = bo_memory(bo)->createMappingInTask(fTask, 0,
			    kIOMapAnywhere | (bo->surface ? bo->surface->mapCache : kIOMapWriteCombineCache));
		}
		if (bo->umap == NULL) {
			ret = kIOReturnNoMemory;
		} else {
			out[0] = bo->umap->getAddress();
			outCount = 1;
		}
		break;
	case kSelSubmit:
		out[0] = gpu_submit(in1 ? 1 : 0, in0, &out[1]);
		outCount = 2;
		break;
	case kSelLog:
		if (args->structureInput != NULL && args->structureInputSize > 0) {
			char line[256];
			size_t n = args->structureInputSize < sizeof(line) - 1 ?
			    (size_t)args->structureInputSize : sizeof(line) - 1;
			memcpy(line, args->structureInput, n);
			line[n] = 0;
			IOLog("XC5GPU client: %s\n", line);
			/* The last few, for the heartbeat (the console scrolls them away). */
			strlcpy(xcover5_gpusvc_lines[xcover5_gpusvc_line++ % 4], line,
			    sizeof(xcover5_gpusvc_lines[0]));
			{
				uint32_t k = xcover5_gllog_n++;
				char *slot = k < XC5_GLLOG_FIRST ? xcover5_gllog_first[k] :
				    xcover5_gllog_ring[(k - XC5_GLLOG_FIRST) % XC5_GLLOG_RING];
				strlcpy(slot, line, sizeof(xcover5_gllog_ring[0]));
			}
		}
		break;
	case kSelSyncBO:
		if (bo == NULL || !bo_used(bo)) {
			ret = kIOReturnBadArgument;
		} else if (bo->surface != NULL && bo->surface->kernelAddress() != 0) {
			cache_clean(bo->surface->kernelAddress(), bo->surface->allocSize);
		}
		break;
	default:
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(fLock);

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	static uint32_t logged;
	if (selector < 32 && !(logged & (1u << selector))) {
		logged |= 1u << selector;
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("XC5GPU: %s sel %u in %u -> 0x%x\n", name, selector, args->scalarInputCount, ret);
	}
	return ret;
}

/*
 * A WRITE_VALUE job (32-bit 0xc0ffee01) through the same buffer and submit
 * paths a client uses: xcover5_gpusvc[5] = 1 passed, 2 failed.
 */
static void
selftest(__unused void *arg, __unused wait_result_t wr)
{
	struct gpu_bo bo;
	bzero(&bo, sizeof(bo));
	if (!bo_create(&bo, 0x10000)) {
		IOLog("XC5GPU SELFTEST: buffer failed\n");
		xcover5_gpusvc[5] = 2;
		return;
	}
	IOMemoryMap *map = bo.mem->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	volatile uint32_t *job = map ? (volatile uint32_t *)map->getVirtualAddress() : NULL;
	if (job == NULL) {
		IOLog("XC5GPU SELFTEST: mapping failed\n");
		bo_free(&bo);
		xcover5_gpusvc[5] = 2;
		return;
	}
	job[4] = (2u << 1) | 1u | (1u << 16);   /* WRITE_VALUE, 64-bit pointers, index 1 */
	job[8] = (uint32_t)(bo.va + 0x800);
	job[9] = (uint32_t)((bo.va + 0x800) >> 32);
	job[10] = 6;                            /* 32-bit immediate */
	job[12] = 0xc0ffee01u;
	volatile uint32_t *output = job + 0x800 / 4;
	*output = 0;
	/*
	 * Boot 47: DATA_INVALID_FAULT (js_status 0x58, no MMU fault): the GPU read
	 * a zero header, so these writes had not reached memory. A kernel_task
	 * mapping of this buffer can come back cacheable whatever was asked for;
	 * clean it to the point of coherency like hakios-a32 does.
	 */
	cache_clean((uintptr_t)job, 0x1000);
	uint64_t us = 0;
	uint32_t status = gpu_submit(1, bo.va, &us);
	cache_clean((uintptr_t)job, 0x1000);
	bool ok = status == 0 && *output == 0xc0ffee01u;
	xcover5_gpusvc[5] = ok ? 1 : 2;
	xcover5_gpusvc[6] = (uint32_t)us;
	IOLog("XC5GPU SELFTEST: buffer at GPU VA %#llx, status %#x in %llu us, word %#x -> %s\n",
	    bo.va, status, us, *output, ok ? "OK" : "FAILED");
	map->release();
	bo_free(&bo);
}
#endif /* XCOVER5 */
