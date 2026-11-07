/*
 * XCover5 drop zone: files for the iOS root from the CACHE partition, so the
 * Mesa libraries reach the phone without reflashing the 12 GB root image.
 * Copied into the xnu tree as bsd/dev/xc5_drop.c by apply-xcover5.py.
 *
 * After wardevsa/ios-a5xelte's herolte_drop.c (patches 0240/0241, the same
 * kernel family). tools/xcover5/mkdrop.py makes cache.img, flashed by Odin
 * into CACHE (p34, PIT file name cache.img; Android is not used):
 *   bytes 0..4095: "XC5DROP1" | u32 version 1 | u32 entry count | u64 bytes | u64 id
 *   from 4096, back to back, 8-byte aligned:
 *     u32 path length | u32 mode (0 = directory) | u64 size | path | data
 * Paths must start with /private/var/xc5/. Once launchd has remounted the
 * root read-write, a thread writes every entry as root:wheel, unless that
 * package id is already installed (/private/var/xc5/.drop-id). Boot-arg
 * xc5drop=0 skips it.
 *
 * Also here: xc5_gl_env(), called from exec_extract_strings(), which puts
 * a library from the drop zone into backboardd (boot-arg xc5gl):
 *   1  /private/var/xc5/lib/libxc5test.dylib   (Mesa's triangle test)
 *   2  /private/var/xc5/lib/libxc5eagl.dylib   (CoreAnimation over GLES, HakiOS)
 *   3  /private/var/xc5/lib/libxc5eagl-b.dylib (CoreAnimation over GLES, Bergamota)
 * with HERO_LOG_STDERR=1, so the bridges' log reaches the console.
 * Only the first two launches of a boot get it, so a broken library costs
 * two backboardd restarts and no more.
 */
#include <pexpert/arm64/board_config.h>

#if defined(XCOVER5)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode_internal.h>
#include <sys/namei.h>
#include <string.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern vnode_t rootvnode;
extern int hz;
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
int xcover5_emmc_read_cache(uint64_t sector, uint32_t count, void *dst);
void xc5_drop_start(void);
int xc5_gl_env(const char *path, int pid, const char **extra, int max);
void xc5_gl_exit(int pid, int rv, uint32_t ns, uint64_t code);
extern void IOSleep(unsigned int milliseconds);
/* For the heartbeat: files written, failed, state (1 no package, 2 already
 * installed, 3 written, 4 root never rw), backboardd launches, insertions. */
uint32_t xcover5_drop_state[5];

#define XDROP_HEADER    4096u
#define XDROP_CHUNK     (1u << 20)              /* read 1 MiB at a time */
#define XDROP_MAX       (192ull << 20)          /* CACHE is 200 MiB */
#define XDROP_PREFIX    "/private/var/xc5/"
#define XDROP_DIR       "/private/var/xc5"
#define XDROP_ID_PATH   "/private/var/xc5/.drop-id"
#define XDROP_MAX_PATH  255u

struct xdrop_header {
	char     magic[8];
	uint32_t version, count;
	uint64_t bytes, id;
};

struct xdrop_entry {
	uint32_t path_len, mode;
	uint64_t size;
};

/* Sequential reader over the payload (CACHE byte 4096 on). */
struct xdrop_reader {
	uint8_t  *buf;
	uint64_t  pos;          /* payload offset of buf[0] */
	uint32_t  have;
	uint64_t  limit;        /* payload bytes */
};

static int
xdrop_fill(struct xdrop_reader *r, uint64_t off)
{
	uint64_t first = off & ~511ull;         /* payload byte, sector aligned */
	uint64_t left = r->limit - first;
	uint32_t n = (uint32_t)MIN((uint64_t)XDROP_CHUNK, (left + 511) & ~511ull) / 512u;
	int error = xcover5_emmc_read_cache((XDROP_HEADER + first) / 512u, n, r->buf);
	r->pos = first;
	r->have = error ? 0 : n * 512u;
	return error;
}

/* Pointer to len bytes at payload offset off (len <= XDROP_CHUNK / 2). */
static const uint8_t *
xdrop_at(struct xdrop_reader *r, uint64_t off, uint32_t len)
{
	if (off + len > r->limit) {
		return NULL;
	}
	if (off < r->pos || off + len > r->pos + r->have) {
		if (xdrop_fill(r, off) || off + len > r->pos + r->have) {
			return NULL;
		}
	}
	return r->buf + (off - r->pos);
}

static bool
xdrop_has_dotdot(const char *path)
{
	for (const char *p = path; *p; p++) {
		if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') &&
		    (p[2] == '/' || p[2] == '\0')) {
			return true;
		}
	}
	return false;
}

static int
xdrop_mkdir(const char *path, vfs_context_t ctx)
{
	struct nameidata nd;
	NDINIT(&nd, CREATE, OP_MKDIR, LOCKPARENT | AUDITVNPATH1, UIO_SYSSPACE,
	    CAST_USER_ADDR_T(path), ctx);
	int error = namei(&nd);
	if (error) {
		return error;
	}
	vnode_t dvp = nd.ni_dvp, vp = nd.ni_vp;
	if (vp == NULLVP) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_type, VDIR);
		VATTR_SET(&va, va_mode, 0755);
		error = vn_mkdir(dvp, &vp, &nd, &va, ctx);
	}
	nameidone(&nd);
	if (vp != NULLVP) {
		vnode_put(vp);
	}
	vnode_put(dvp);
	return error;
}

static int
xdrop_write(const char *path, int mode, const void *data, size_t size, vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	int resid = 0;
	int error = vnode_open(path, FWRITE | O_CREAT | O_TRUNC, mode & ALLPERMS, 0, &vp, ctx);
	if (error) {
		return error;
	}
	error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)data, (int)size, 0, UIO_SYSSPACE, IO_UNIT,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_close(vp, FWRITE, ctx);
	return error ? error : (resid ? EIO : 0);
}

static int
xdrop_file(struct xdrop_reader *r, const char *path, int mode, uint64_t data, uint64_t size,
    vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FWRITE | O_CREAT | O_TRUNC, mode & ALLPERMS, 0, &vp, ctx);
	if (error) {
		return error;
	}
	for (uint64_t done = 0; done < size && !error;) {
		uint32_t n = (uint32_t)MIN(size - done, (uint64_t)(XDROP_CHUNK / 2));
		const uint8_t *p = xdrop_at(r, data + done, n);
		int resid = 0;
		if (p == NULL) {
			error = EIO;
			break;
		}
		error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)p, (int)n, (off_t)done, UIO_SYSSPACE,
		    IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (!error && resid) {
			error = EIO;
		}
		done += n;
	}
	if (!error) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_mode, mode & ALLPERMS);
		error = vnode_setattr(vp, &va, ctx);
	}
	vnode_close(vp, FWRITE, ctx);
	return error;
}

/* The whole of a small file (up to max bytes) into buf; its length, or -1. */
static int
xdrop_read_small(const char *path, void *buf, int max, vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	int resid = 0;
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
		return -1;
	}
	int error = vn_rdwr(UIO_READ, vp, (caddr_t)buf, max, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
	    vfs_context_proc(ctx));
	vnode_close(vp, FREAD, ctx);
	return error ? -1 : max - resid;
}

/*
 * launchd's list of disabled jobs, after herolte (wardevsa/ios-a5xelte, branch herolte: hero-run.c writes
 * /private/var/db/com.apple.xpc.launchd/disabled.plist; tools/herolte-disabled.list has com.apple.fairplayd.H2
 * first). Boots 81-83: fairplayd.H2 starts and exits at once, so launchd keeps its port and every FairPlay client
 * (checkpointd, and through it the Photos app) waits for ever. A disabled job has no port, so the client's lookup
 * fails at once instead. The package carries the file as /private/var/xc5/launchd-disabled.plist; it is copied over
 * launchd's own, whose first contents are kept once as /private/var/xc5/launchd-disabled.orig.plist (an empty file
 * if there was none). launchd reads the list when it starts: it takes effect from the NEXT boot.
 */
#define XDROP_LD_SRC    "/private/var/xc5/launchd-disabled.plist"
#define XDROP_LD_ORIG   "/private/var/xc5/launchd-disabled.orig.plist"
#define XDROP_LD_DIR    "/private/var/db/com.apple.xpc.launchd"
#define XDROP_LD_DST    "/private/var/db/com.apple.xpc.launchd/disabled.plist"
#define XDROP_LD_MAX    65536

static void
xdrop_launchd_disabled(vfs_context_t ctx)
{
	char *buf = kalloc_data(XDROP_LD_MAX, Z_WAITOK);
	if (buf == NULL) {
		return;
	}
	int n = xdrop_read_small(XDROP_LD_SRC, buf, XDROP_LD_MAX, ctx);
	if (n > 0 && n < XDROP_LD_MAX) {
		if (xdrop_read_small(XDROP_LD_ORIG, buf + n, 1, ctx) < 0) {      /* no backup yet */
			char *old = kalloc_data(XDROP_LD_MAX, Z_WAITOK);
			int m = old ? xdrop_read_small(XDROP_LD_DST, old, XDROP_LD_MAX, ctx) : -1;
			int e = xdrop_write(XDROP_LD_ORIG, 0644, old, m > 0 ? (size_t)m : 0, ctx);
			printf("XC5 DROP: launchd's disabled.plist (%d bytes) kept as %s: %d\n", m, XDROP_LD_ORIG, e);
			if (old) {
				kfree_data(old, XDROP_LD_MAX);
			}
		}
		(void)xdrop_mkdir(XDROP_LD_DIR, ctx);
		int e = xdrop_write(XDROP_LD_DST, 0644, buf, (size_t)n, ctx);
		printf("XC5 DROP: %s: %d bytes from the package: %d (next boot)\n", XDROP_LD_DST, n, e);
	}
	kfree_data(buf, XDROP_LD_MAX);
}

static uint64_t
xdrop_installed_id(vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	uint64_t id = 0;
	int resid = 0;
	if (vnode_open(XDROP_ID_PATH, FREAD, 0, 0, &vp, ctx) == 0) {
		if (vn_rdwr(UIO_READ, vp, (caddr_t)&id, sizeof(id), 0, UIO_SYSSPACE, 0,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) != 0 || resid != 0) {
			id = 0;
		}
		vnode_close(vp, FREAD, ctx);
	}
	return id;
}

static void
xdrop_apply(void)
{
	struct xdrop_reader r = { .buf = kalloc_data(XDROP_CHUNK, Z_WAITOK) };
	if (r.buf == NULL) {
		return;
	}
	struct xdrop_header h;
	if (xcover5_emmc_read_cache(0, XDROP_HEADER / 512u, r.buf) != 0) {
		printf("XC5 DROP: CACHE not readable\n");
		goto out;
	}
	memcpy(&h, r.buf, sizeof(h));
	if (memcmp(h.magic, "XC5DROP1", 8) != 0 || h.version != 1 || h.bytes > XDROP_MAX) {
		printf("XC5 DROP: no drop package in CACHE\n");
		xcover5_drop_state[2] = 1;
		goto out;
	}
	vfs_context_t ctx = vfs_context_kernel();
	(void)xdrop_mkdir(XDROP_DIR, ctx);
	if (xdrop_installed_id(ctx) == h.id) {
		printf("XC5 DROP: package %llu already installed\n", h.id);
		xcover5_drop_state[2] = 2;
		goto out;
	}
	r.limit = h.bytes;
	uint64_t off = 0;
	unsigned files = 0, failed = 0;
	char path[XDROP_MAX_PATH + 1];
	for (uint32_t i = 0; i < h.count; i++) {
		const uint8_t *p = xdrop_at(&r, off, sizeof(struct xdrop_entry));
		struct xdrop_entry e;
		if (p == NULL) {
			failed++;
			break;
		}
		memcpy(&e, p, sizeof(e));
		if (e.path_len == 0 || e.path_len > XDROP_MAX_PATH ||
		    (p = xdrop_at(&r, off + sizeof(e), e.path_len)) == NULL) {
			failed++;
			break;
		}
		memcpy(path, p, e.path_len);
		path[e.path_len] = '\0';
		uint64_t data = off + ((sizeof(e) + e.path_len + 7) & ~7ull);
		off = data + ((e.size + 7) & ~7ull);
		int error;
		if (strncmp(path, XDROP_PREFIX, strlen(XDROP_PREFIX)) != 0 || xdrop_has_dotdot(path)) {
			error = EPERM;
		} else if (e.mode == 0) {
			error = xdrop_mkdir(path, ctx);
			error = error == EEXIST ? 0 : error;
		} else {
			error = xdrop_file(&r, path, (int)e.mode, data, e.size, ctx);
		}
		if (error) {
			failed++;
			printf("XC5 DROP: %s: error %d\n", path, error);
		} else {
			files++;
		}
	}
	if (failed == 0) {
		xdrop_launchd_disabled(ctx);
		(void)xdrop_write(XDROP_ID_PATH, 0644, &h.id, sizeof(h.id), ctx);
	}
	printf("XC5 DROP: package %llu: %u entries written, %u failed\n", h.id, files, failed);
	xcover5_drop_state[0] = files;
	xcover5_drop_state[1] = failed;
	xcover5_drop_state[2] = 3;
out:
	kfree_data(r.buf, XDROP_CHUNK);
}

/* Wait (up to 3 minutes) for launchd's read-write remount of the root. */
static void
xdrop_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
	for (int s = 0; s < 180; s++) {
		if (rootvnode != NULLVP && !vfs_isrdonly(vnode_mount(rootvnode))) {
			xdrop_apply();
			return;
		}
		(void)tsleep(&chan, PZERO, "xc5drop", hz);
	}
	printf("XC5 DROP: root never read-write, nothing written\n");
	xcover5_drop_state[2] = 4;
}

void
xc5_drop_start(void)
{
	int on = 1;
	thread_t thread;
	(void)PE_parse_boot_argn("xc5drop", &on, sizeof(on));
	if (on && kernel_thread_start(xdrop_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#define XGL_TARGET      "/usr/libexec/backboardd"
#define XGL_MAX_LAUNCHES 2

/*
 * Boot 51: library inserted into the environment, yet XC5GPU never had a
 * client: dyld honours DYLD_* only if AMFI allows it (amfi_check_dyld_policy_
 * self, __mac_syscall("AMFI", 90)), and with no "AMFI" policy it treats the
 * process as restricted. sweet_amfi.c's "AMFI" policy answers through this:
 * the backboardd that got the library may insert it (herolte 0246).
 */
#define AMFI_DYLD_OUTPUT_ALLOW_AT_PATH                  (1ull << 0)
#define AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS                (1ull << 1)
#define AMFI_DYLD_OUTPUT_ALLOW_FAILED_LIBRARY_INSERTION (1ull << 5)
static int xgl_pid = -1;
uint64_t xc5_gl_dyld_flags(void);

/*
 * Boot-arg xc5pbhook=1: PosterBoard gets /private/var/xc5/lib/libxc5pb.dylib
 * (gpu/eagl/src/xc5_pbhook.c). Boots 56-63: it traps on
 * !_stateLock_switcherConfiguration in its first-time switcher setup; the
 * configuration's lazy getter has made one before the setup ran. The library
 * empties it first. The first XPB_MAX launches of a boot get it; their pids
 * are allowed DYLD_INSERT_LIBRARIES like backboardd's.
 */
/*
 * Boot-arg xc5media=1: mediaserverd gets /private/var/xc5/lib/libxc5media.dylib
 * (gpu/eagl/src/xc5_media.c: its capture stack's portrait-filter prewarm is
 * skipped, as HakiOS's inject_media.c does, so the daemon stays up to decode the
 * stock wallpaper's HEIC pictures for everybody). Same table, same pid list.
 */
#define XPB_MAX         24
static int xpb_pids[XPB_MAX];
static int xpb_count;

static const struct {
	const char *suffix, *bootarg, *lib, *env, *name;
} xpb_targets[] = {
	{ "/PosterBoard.app/PosterBoard", "xc5pbhook", "/private/var/xc5/lib/libxc5pb.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5pb.dylib", "PosterBoard" },
	{ "/usr/sbin/mediaserverd", "xc5media", "/private/var/xc5/lib/libxc5media.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5media.dylib", "mediaserverd" },
	/* xc5poster=1: CollectionsPoster's start sampled (gpu/eagl/src/xc5_poster.c, boots 71-72's check-in timeout) */
	{ "/CollectionsPoster.appex/CollectionsPoster", "xc5poster", "/private/var/xc5/lib/libxc5poster.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5poster.dylib", "CollectionsPoster" },
	/* xc5wall=1: a still picture as the wallpaper (gpu/eagl/src/xc5_sb.c, after herolte's herosb.c) */
	{ "/SpringBoard.app/SpringBoard", "xc5wall", "/private/var/xc5/lib/libxc5sb.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5sb.dylib", "SpringBoard" },
	/* xc5photos=1: the Photos app's main thread sampled (same libxc5poster.dylib); boot 80: it stays a white
	 * screen with no crash, and its own start was hidden under that screen */
	{ "/MobileSlideShow.app/MobileSlideShow", "xc5photos", "/private/var/xc5/lib/libxc5poster.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5poster.dylib", "MobileSlideShow" },
	/* xc5ckpt=1: checkpointd's FairPlay Mach call gets a time limit (gpu/eagl/src/xc5_ckpt.c, boot 82:
	 * fairplayd.H2 starts and exits at once, so the call waited forever and so did the Photos app).
	 * Before the xc5photos entry: one library per process, the first entry that is on wins. */
	{ "/usr/libexec/checkpointd", "xc5ckpt", "/private/var/xc5/lib/libxc5ckpt.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5ckpt.dylib:/private/var/xc5/lib/libxc5poster.dylib",
	  "checkpointd" },
	/* boot 81: Photos' main thread waits on checkpointd (MCCopyCheckpointValue); its threads sampled too */
	{ "/usr/libexec/checkpointd", "xc5photos", "/private/var/xc5/lib/libxc5poster.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5poster.dylib", "checkpointd" },
	/* xc5mtl=1/2: Metal M3, chronod (the widgets' renderer) asks Metal for a device with
	 * RedwoodMetal.bundle named on IOAcceleratorES (gpu/metal/xc5_mtl.c; needs xc5metal=1) */
	{ "/ChronoCore.framework/Support/chronod", "xc5mtl", "/private/var/xc5/lib/libxc5mtl.dylib",
	  "DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5mtl.dylib", "chronod" },
};

static int
xpb_pid_allowed(int pid)
{
	for (int i = 0; i < xpb_count && i < XPB_MAX; i++) {
		if (xpb_pids[i] == pid) {
			return 1;
		}
	}
	return 0;
}

static int
xpb_env(const char *path, int pid, const char **extra, int max)
{
	size_t len = path != NULL ? strlen(path) : 0;

	if (max < 1) {
		return 0;
	}
	for (size_t t = 0; t < sizeof(xpb_targets) / sizeof(xpb_targets[0]); t++) {
		size_t tlen = strlen(xpb_targets[t].suffix);
		int on = 0;
		if (len < tlen || strcmp(path + len - tlen, xpb_targets[t].suffix) != 0) {
			continue;
		}
		if (!PE_parse_boot_argn(xpb_targets[t].bootarg, &on, sizeof(on)) || !on) {
			continue;       /* a later entry for the same program may be on (checkpointd: xc5ckpt, xc5photos) */
		}
		if (xpb_count >= XPB_MAX) {
			printf("XC5 PB: %s (pid %d): no room, stock\n", xpb_targets[t].name, pid);
			return 0;
		}
		vnode_t vp = NULLVP;
		if (vnode_lookup(xpb_targets[t].lib, 0, &vp, vfs_context_kernel()) != 0) {
			printf("XC5 PB: %s (pid %d): no %s, stock\n", xpb_targets[t].name, pid, xpb_targets[t].lib);
			return 0;
		}
		vnode_put(vp);
		xpb_pids[xpb_count++] = pid;
		extra[0] = xpb_targets[t].env;
		printf("XC5 PB: %s (pid %d): %s inserted\n", xpb_targets[t].name, pid, xpb_targets[t].lib);
		return 1;
	}
	return 0;
}

uint64_t
xc5_gl_dyld_flags(void)
{
	int self = proc_selfpid();
	if ((xgl_pid < 0 || self != xgl_pid) && !xpb_pid_allowed(self)) {
		return 0;
	}
	return AMFI_DYLD_OUTPUT_ALLOW_AT_PATH | AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS |
	       AMFI_DYLD_OUTPUT_ALLOW_FAILED_LIBRARY_INSERTION;
}

int
xc5_gl_env(const char *path, int pid, const char **extra, int max)
{
	static int mode = -1, launches;
	if (mode < 0) {
		mode = 0;
		(void)PE_parse_boot_argn("xc5gl", &mode, sizeof(mode));
	}
	if (path != NULL && strcmp(path, XGL_TARGET) != 0) {
		return xpb_env(path, pid, extra, max);
	}
	if (mode <= 0 || path == NULL || max < 2) {
		return 0;
	}
	static const char *const libs[] = {
		"/private/var/xc5/lib/libxc5test.dylib",        /* 1: Mesa's triangle test */
		"/private/var/xc5/lib/libxc5eagl.dylib",        /* 2: HakiOS's EAGL bridge */
		"/private/var/xc5/lib/libxc5eagl-b.dylib",      /* 3: Bergamota's EAGL shim */
	};
	static const char *const envs[] = {
		"DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5test.dylib",
		"DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5eagl.dylib",
		"DYLD_INSERT_LIBRARIES=/private/var/xc5/lib/libxc5eagl-b.dylib",
	};
	if (mode > 3) {
		return 0;
	}
	const char *lib = libs[mode - 1];
	int launch = ++launches;
	xcover5_drop_state[3] = (uint32_t)launch;
	if (launch > XGL_MAX_LAUNCHES) {
		printf("XC5 GL: backboardd launch %d: stock\n", launch);
		return 0;
	}
	vnode_t vp = NULLVP;
	if (vnode_lookup(lib, 0, &vp, vfs_context_kernel()) != 0) {
		printf("XC5 GL: backboardd launch %d: no %s, stock\n", launch, lib);
		return 0;
	}
	vnode_put(vp);
	extra[0] = envs[mode - 1];
	/* The bridges' stderr (and Mesa's) goes to the kernel log (gpu/eagl/src/hero_log.c). */
	extra[1] = "HERO_LOG_STDERR=1";
	xgl_pid = pid;
	printf("XC5 GL: backboardd launch %d (pid %d): %s inserted\n", launch, pid, lib);
	xcover5_drop_state[4]++;
	return 2;
}

/*
 * proc_prepareexit() of every process: when a backboardd we inserted a library
 * into exits, say how, then replay the GPU clients' log (XC5GPU.cpp) slowly
 * enough to film: 16 lines, then 5 s, and so on. Only for the inserted
 * launches (two per boot), on the exiting thread.
 */
extern char xcover5_gllog_first[24][100];
extern char xcover5_gllog_ring[40][100];
extern uint32_t xcover5_gllog_n;
uint32_t xcover5_gl_exit[4];    /* heartbeat: pid, wait status, reason namespace, reason code */

static void
xgl_say(const char *line, int *shown)
{
	printf("XC5 GLLOG %s\n", line);
	if (++*shown % 16 == 0) {
		IOSleep(5000);
	}
}

void
xc5_gl_exit(int pid, int rv, uint32_t ns, uint64_t code)
{
	if (xgl_pid < 0 || pid != xgl_pid) {
		return;
	}
	xgl_pid = -1;
	xcover5_gl_exit[0] = (uint32_t)pid;
	xcover5_gl_exit[1] = (uint32_t)rv;
	xcover5_gl_exit[2] = ns;
	xcover5_gl_exit[3] = (uint32_t)code;
	printf("XC5 GL: backboardd pid %d exits: status 0x%x (sig %d, exit %d) reason ns %u code 0x%llx; %u log lines\n",
	    pid, rv, rv & 0x7f, (rv >> 8) & 0xff, ns, code, xcover5_gllog_n);
	IOSleep(3000);
	int shown = 0;
	uint32_t n = xcover5_gllog_n;
	for (uint32_t i = 0; i < n && i < 24; i++) {
		xgl_say(xcover5_gllog_first[i], &shown);
	}
	if (n > 24 + 40) {
		printf("XC5 GLLOG ... %u lines skipped ...\n", n - 24 - 40);
	}
	for (uint32_t i = n > 24 + 40 ? n - 40 : 24; i < n; i++) {
		xgl_say(xcover5_gllog_ring[(i - 24) % 40], &shown);
	}
	printf("XC5 GLLOG end (pid %d)\n", pid);
	IOSleep(5000);
}
#endif /* XCOVER5 */
