/*
 * XCover5: what the Galaxy A50 port's Wi-Fi files (bsd/dev/hakios.*.c, bsd/dev/xc5_scsc.c; tools/xcover5/a50/,
 * github.com/nespaces/ios-a50-exynos9610, APSL 2.0) ask of the rest of the A50's kernel, made on this one:
 *   loader_log_line      the A50 loader's log ring: here the console (say() prints as well), so nothing
 *   hakios_wifi_tell     a line of the Wi-Fi's log: printf
 *   hakios_image_file    a firmware file of the A50's small image (fw/NAME): here the drop zone /private/var/xc5/wifi/,
 *                        read once and kept (kalloc); the A50 names map to the drop's
 *   hakios_partition_file  a file of a partition (the EFS's factory Wi-Fi address): none here (ENOENT)
 *   hakios_virtio_net_init QEMU's virtio network device: none here
 */
#include <pexpert/arm64/board_config.h>
#if defined(XCOVER5)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <stdarg.h>
#include <string.h>

extern void delay(int usec);

void loader_log_line(const char *line);
int hakios_wifi_tell(const char *format, ...) __printflike(1, 2);
const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
int hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size);
void hakios_virtio_net_init(void);

void
loader_log_line(const char *line)
{
	(void)line;
}

int
hakios_wifi_tell(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	return printf("%s", line);
}

void
hakios_virtio_net_init(void)
{
}

int
hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size)
{
	(void)partition;
	(void)mount;
	(void)path;
	(void)max;
	if (data != NULL) {
		*data = NULL;
	}
	if (size != NULL) {
		*size = 0;
	}
	return ENOENT;
}

/* the A50's fw/ names, and the drop zone's (tools/xcover5/dev/wlbtfw.sh makes them, mkdrop.py puts them in wifi/) */
static const struct {
	const char *a50, *drop;
} names[] = {
	{ "mx140.bin", "mx140.bin" },
	{ "scsc_ka.bin", "ka.bin" },
	{ "mx140_wlan.hcf", "wlan.hcf" },
	{ "mx140_wlan_sw.hcf", "wlan_sw.hcf" },
	{ "scsc_country.bin", "country.bin" },
	{ "mac.info", "mac.info" },
	{ "mx140_bt.hcf", "bt.hcf" },
};

#define KEPT_MOST 8
static struct {
	char name[24];
	uint8_t *data;
	uint64_t size;
} kept[KEPT_MOST];
static unsigned kept_n;
static LCK_GRP_DECLARE(glue_grp, "xc5-hakios-glue");
static LCK_MTX_DECLARE(glue_mtx, &glue_grp);

/* A file of the drop zone, whole, into a new buffer: its length, or -1 (no such file, or larger than max). */
static int64_t
read_whole(const char *path, uint8_t **out, uint64_t max)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	int64_t done = 0;
	uint8_t *buf;

	*out = NULL;
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
		return -1;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	if (vnode_getattr(vp, &va, ctx) != 0 || va.va_data_size == 0 || va.va_data_size > max) {
		vnode_close(vp, FREAD, ctx);
		return -1;
	}
	buf = kalloc_data((vm_size_t)va.va_data_size, Z_WAITOK);
	if (buf == NULL) {
		vnode_close(vp, FREAD, ctx);
		return -1;
	}
	while ((uint64_t)done < va.va_data_size) {
		int resid = 0, n = (int)MIN((uint64_t)(1u << 20), va.va_data_size - (uint64_t)done);
		if (vn_rdwr(UIO_READ, vp, (caddr_t)(buf + done), n, (off_t)done, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx)) != 0 || resid == n) {
			break;
		}
		done += n - resid;
	}
	vnode_close(vp, FREAD, ctx);
	if ((uint64_t)done != va.va_data_size) {
		kfree_data(buf, (vm_size_t)va.va_data_size);
		return -1;
	}
	*out = buf;
	return done;
}

const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	const char *drop = NULL;
	char path[96];
	uint8_t *data = NULL;
	int64_t n = -1;

	(void)dir;
	if (disk_out != NULL) {
		*disk_out = 0;
	}
	if (size != NULL) {
		*size = 0;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(names[i].a50, name) == 0) {
			drop = names[i].drop;
		}
	}
	if (drop == NULL) {
		printf("XC5 SCSC: firmware file %s has no place in the drop\n", name);
		return NULL;
	}
	lck_mtx_lock(&glue_mtx);
	for (unsigned i = 0; i < kept_n; i++) {
		if (strcmp(kept[i].name, drop) == 0) {
			if (size != NULL) {
				*size = kept[i].size;
			}
			lck_mtx_unlock(&glue_mtx);
			return kept[i].data;
		}
	}
	lck_mtx_unlock(&glue_mtx);
	snprintf(path, sizeof(path), "/private/var/xc5/wifi/%s", drop);
	/* the firmware itself may come with a drop that is being written now (first boot after a flash): waited for */
	for (int s = 0; s < (strcmp(drop, "mx140.bin") == 0 ? 120 : 1) && n < 0; s++) {
		n = read_whole(path, &data, 8u << 20);
		if (n < 0 && strcmp(drop, "mx140.bin") == 0) {
			delay(1000000);
		}
	}
	if (n < 0) {
		printf("XC5 SCSC: no %s\n", path);
		return NULL;
	}
	printf("XC5 SCSC: %s: %lld bytes (the A50's fw/%s)\n", path, n, name);
	lck_mtx_lock(&glue_mtx);
	if (kept_n < KEPT_MOST) {
		strlcpy(kept[kept_n].name, drop, sizeof(kept[kept_n].name));
		kept[kept_n].data = data;
		kept[kept_n].size = (uint64_t)n;
		kept_n++;
	}
	lck_mtx_unlock(&glue_mtx);
	if (size != NULL) {
		*size = (uint64_t)n;
	}
	return data;
}
#endif /* XCOVER5 */
