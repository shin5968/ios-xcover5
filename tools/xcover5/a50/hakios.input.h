/*
 * XCover5: the part of the A50 port's sys/hakios.input.h that its Wi-Fi files use (the rest is the A50's touch,
 * keys, storage and other drivers, not here). Defined by bsd/dev/xc5_hakios_glue.c.
 */
#ifndef _SYS_HAKIOS_INPUT_H_
#define _SYS_HAKIOS_INPUT_H_

#include <sys/types.h>
#include <stdint.h>

/* a line for the Wi-Fi's own log (the A50 also put it on its USB console) */
int hakios_wifi_tell(const char *format, ...) __printflike(1, 2);

/* a firmware file: the A50 read them from its small image's fw/; here from the drop zone, /private/var/xc5/wifi/ */
const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* a file of a partition (the A50's EFS for the factory Wi-Fi address): none here, always ENOENT */
int hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size);

#endif /* _SYS_HAKIOS_INPUT_H_ */
