// SPDX-License-Identifier: MIT
// XCover5: libxc5media.dylib, inserted into mediaserverd by the kernel (boot-arg xc5media=1, bsd/dev/xc5_drop.c).
//
// Boot 71: PosterBoard runs CollectionsPoster (iOS 16's stock wallpaper) and backboardd shows its 1105x1160 picture,
// but every pixel of it is 0. The stock wallpaper's layers are HEIC pictures, HEVC inside, which ImageIO decodes
// through VideoToolbox's decompression service in mediaserverd (com.apple.coremedia.decompressionsession), with the
// software HEVC decoder where there is no hardware one. HakiOS (oosexgang/hakios-xnu-kernel, userland/src/
// inject_media.c, MIT) found mediaserverd dying at start: its capture stack (CMCapture) "prewarms" the portrait
// mode's Core Image filters, Core Image wants a GPU context that is not there and the daemon jumps to address 0, every
// time launchd restarts it; without the daemon "ImageIO has no way to decode a HEIC" and the stock wallpaper is its
// gradient alone. HakiOS's fix: +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] does
// nothing. Here the same, by class and selector (no address check: this is the 16.7.16 cache, not their 16.5), as
// soon as CMCapture's image has the class. Reported through XC5GPU's Log selector.
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t master, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t connection, uint32_t selector, const uint64_t *input,
    uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt,
    void *outputStruct, size_t *outputStructCnt);

#define SEL_LOG 5               /* XC5GPU.cpp kSelLog */

static io_connect_t log_conn;

static void say(const char *fmt, ...) {
    char line[240];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    if (log_conn == MACH_PORT_NULL) {
        io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("XC5GPU"));
        if (svc != MACH_PORT_NULL) {
            IOServiceOpen(svc, mach_task_self(), 0, &log_conn);
            IOObjectRelease(svc);
        }
    }
    if (log_conn != MACH_PORT_NULL)
        IOConnectCallMethod(log_conn, SEL_LOG, NULL, 0, line, (size_t)n, NULL, NULL, NULL, NULL);
    fprintf(stderr, "%s\n", line);
}

static unsigned prewarms_skipped;

static void prewarm_portrait_filter(id self, SEL cmd, int version, BOOL styles) {
    (void)self; (void)cmd;
    if (prewarms_skipped++ < 3)
        say("xc5media: portrait filter prewarm (version %d, styles %d) skipped", version, (int)styles);
}

static bool replaced;

static void image_added(const struct mach_header *header, intptr_t image_slide) {
    (void)header; (void)image_slide;
    if (replaced) return;
    Class cls = objc_getClass("BWCoreImageFilterRenderer");
    if (cls == Nil) return;
    replaced = true;
    Method m = class_getClassMethod(cls, sel_registerName("prewarmPortraitFilterVersion:semanticStyleFilters:"));
    if (m == NULL) {
        say("xc5media: +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] not found");
        return;
    }
    method_setImplementation(m, (IMP)prewarm_portrait_filter);
    say("xc5media: +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] replaced");
}

__attribute__((constructor)) static void xc5_media_init(void) {
    if (strcmp(getprogname(), "mediaserverd") != 0) return;
    say("xc5media: loaded in mediaserverd (pid %d)", getpid());
    _dyld_register_func_for_add_image(image_added);         /* also called for every image already loaded */
}
