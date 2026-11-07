// SPDX-License-Identifier: MIT
// XCover5: libxc5sb.dylib, inserted into SpringBoard by the kernel (boot-arg xc5wall=1, bsd/dev/xc5_drop.c): a still
// picture as the wallpaper.
//
// Boots 56-72: iOS 16's wallpaper is PosterBoard's, drawn by poster extensions; here CollectionsPoster (the stock one)
// is killed on "extension check-in timeout" and its picture reaches the screen with 0 lit pixels, so the lock and home
// screens are black. As the Galaxy S7 port did (wardevsa/ios-a5xelte herolte, gpu/eagl/src/herosb.c, MIT): the drop
// zone's /private/var/xc5/wallpaper.png goes into SpringBoard's wallpaper windows and behind the home screen. Not
// Apple's poster: no parallax, depth or changes from Settings. /private/var/xc5/wallpaper.off turns it off.
// Reported through XC5GPU's Log selector (no libOSMesa in SpringBoard).
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
#define DROP_DIR "/private/var/xc5/"
#define VIEW_TAG 0x58433557     /* 'XC5W' */

typedef struct { double x, y, w, h; } rect_t;

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

static id nsstring(const char *text) {
    return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
        sel_registerName("stringWithUTF8String:"), text);
}

static id send0(id target, const char *selector) {
    return ((id (*)(id, SEL))objc_msgSend)(target, sel_registerName(selector));
}

static id picture;
static unsigned rounds, given;

static void give_picture(id window, bool behind_all) {
    if (((id (*)(id, SEL, long))objc_msgSend)(window, sel_registerName("viewWithTag:"), VIEW_TAG)) return;
    id view = ((id (*)(id, SEL, id))objc_msgSend)(send0((id)objc_getClass("UIImageView"), "alloc"),
        sel_registerName("initWithImage:"), picture);
    rect_t bounds = ((rect_t (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds"));
    ((void (*)(id, SEL, rect_t))objc_msgSend)(view, sel_registerName("setFrame:"), bounds);
    ((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setContentMode:"), 2);         /* aspect fill */
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(view, sel_registerName("setAutoresizingMask:"), 18);
    ((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setTag:"), VIEW_TAG);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setUserInteractionEnabled:"), NO);
    if (behind_all)
        ((void (*)(id, SEL, id, long))objc_msgSend)(window, sel_registerName("insertSubview:atIndex:"), view, 0);
    else
        ((void (*)(id, SEL, id))objc_msgSend)(window, sel_registerName("addSubview:"), view);
    given++;
    say("xc5sb: picture in %s, %.0f x %.0f", object_getClassName(window), bounds.w, bounds.h);
}

static void look_at_windows(void *unused) {
    (void)unused;
    rounds++;
    Class windows_class = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = windows_class && class_respondsToSelector(object_getClass((id)windows_class), all) ?
        ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)windows_class, all, YES, NO) : nil;
    if (!picture && objc_getClass("UIImage")) {
        picture = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("UIImage"),
            sel_registerName("imageWithContentsOfFile:"), nsstring(DROP_DIR "wallpaper.png"));
        if (picture) send0(picture, "retain");
    }
    size_t count = windows ? ((size_t (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count")) : 0;
    if (rounds == 1 || rounds == 6)
        say("xc5sb: round %u, %zu windows, picture %s, %u given", rounds, count, picture ? "loaded" : "MISSING", given);
    for (size_t i = 0; picture && i < count; i++) {
        id window = ((id (*)(id, SEL, size_t))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
        const char *name = object_getClassName(window);
        if (rounds == 6) say("xc5sb: window %s", name);
        if (strstr(name, "Wallpaper") != NULL || strstr(name, "Poster") != NULL)
            give_picture(window, false);
        else if (strcmp(name, "SBHomeScreenWindow") == 0)
            give_picture(window, true);
    }
    if (rounds < 60)
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL,
            look_at_windows);
}

/*
 * Boots 73-74: SpringBoard aborts at start, every time, with
 *   NSInternalInconsistencyException: 'Invalid parameter not satisfying: extensionBundleURL != nil'
 * thrown in PosterKit (+0x3d324 of the 16.7.16 cache: the assertion string sits with "timeFontConfiguration != nil" in
 * PRTimeFontConfiguration.m) from SpringBoard's wallpaper code (+0x342678, near SBWallpaperOrientationSourceDescription).
 * The saved poster (CollectionsPoster, set up in boot 64) is described with no URL for its extension bundle, and every
 * PosterKit entry that takes one asserts. The time font configuration is the font of the lock screen's clock:
 * -[PRTimeFontConfiguration initWithFont:extensionBundleURL:], +configurationWithTimeFontConfiguration:extensionBundleURL:
 * and -[UIView pr_set...extensionBundleURL:]. With a nil URL, each of those is now given the main bundle's URL in its
 * place (the font is a system one; the URL only says where a custom font would be looked up), so the lock screen comes up
 * and our picture goes behind it. The first three are logged. This is done whatever xc5wall says (it is what lets
 * SpringBoard live at all now); the picture is still only with xc5wall=1.
 */
static unsigned nil_urls;

static id main_bundle_url(void) {
    id bundle = send0((id)objc_getClass("NSBundle"), "mainBundle");
    return bundle ? send0(bundle, "bundleURL") : nil;
}

static void note_nil(const char *what) {
    if (nil_urls++ < 3) say("xc5sb: %s with a nil extensionBundleURL: the main bundle's given instead", what);
}

/* Every method of PosterKit's classes (PR...) and UIView/UILabel whose selector has an argument labelled
 * ...extensionBundleURL, objects only, up to six arguments (x2-x7): that argument is replaced when nil. */
typedef id (*objs6_t)(id, SEL, id, id, id, id, id, id);
#define MAX_WRAPPED 48
static struct { Class cls; SEL sel; IMP orig; int urlarg; } wrapped[MAX_WRAPPED];
static int nwrapped;

static IMP find_orig(id self, SEL cmd, int *urlarg) {
    for (Class c = object_getClass(self); c != Nil; c = class_getSuperclass(c))
        for (int i = 0; i < nwrapped; i++)
            if (wrapped[i].sel == cmd && wrapped[i].cls == c) {
                *urlarg = wrapped[i].urlarg;
                return wrapped[i].orig;
            }
    return NULL;
}

static id my_url_arg(id self, SEL cmd, id a, id b, id c, id d, id e, id f) {
    int k = -1;
    IMP orig = find_orig(self, cmd, &k);
    id *args[] = { &a, &b, &c, &d, &e, &f };
    if (k >= 0 && k < 6 && *args[k] == nil) {
        note_nil(sel_getName(cmd));
        *args[k] = main_bundle_url();
    }
    return orig ? ((objs6_t)orig)(self, cmd, a, b, c, d, e, f) : nil;
}

/* Which argument (0-based) of SEL is labelled ...extensionBundleURL / ...ExtensionBundleURL; -1 if none. */
static int url_arg(const char *sel) {
    int k = 0;
    const char *start = sel;
    for (const char *p = sel; *p; p++) {
        if (*p != ':') continue;
        size_t len = (size_t)(p - start);
        if (len >= 18 && (strncmp(p - 18, "extensionBundleURL", 18) == 0 || strncmp(p - 18, "ExtensionBundleURL", 18) == 0))
            return k;
        k++;
        start = p + 1;
    }
    return -1;
}

static void wrap_class(Class cls) {
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count && nwrapped < MAX_WRAPPED; i++) {
        SEL sel = method_getName(methods[i]);
        const char *name = sel_getName(sel), *types = method_getTypeEncoding(methods[i]);
        int k = url_arg(name), n = 0;
        if (k < 0 || types == NULL) continue;
        for (const char *p = name; *p; p++) n += *p == ':';
        if (n > 6) continue;
        /* objects only, and an object back: '@' for the return, self and each argument (':' is _cmd) */
        int objs = 0, others = 0;
        for (const char *p = types; *p; p++) {
            if (*p == '@') objs++;
            else if (*p >= 'A' && *p <= 'z' && *p != '@') others++;
        }
        if (objs != n + 2 || others != 0) continue;
        wrapped[nwrapped].cls = cls;
        wrapped[nwrapped].sel = sel;
        wrapped[nwrapped].urlarg = k;
        wrapped[nwrapped].orig = method_setImplementation(methods[i], (IMP)my_url_arg);
        nwrapped++;
    }
    free(methods);
}

static bool guarded;

static void guard_poster_kit(void) {
    Class cls = objc_getClass("PRTimeFontConfiguration");
    if (guarded || cls == Nil) return;
    guarded = true;
    int classes = objc_getClassList(NULL, 0);
    Class *list = calloc((size_t)classes, sizeof(Class));
    classes = objc_getClassList(list, classes);
    for (int i = 0; i < classes; i++) {
        const char *name = class_getName(list[i]);
        if (strncmp(name, "PR", 2) != 0 && strcmp(name, "UIView") != 0 && strcmp(name, "UILabel") != 0) continue;
        wrap_class(list[i]);
        wrap_class(object_getClass((id)list[i]));
    }
    free(list);
    say("xc5sb: PosterKit guarded: %d methods taking an extensionBundleURL", nwrapped);
    for (int i = 0; i < nwrapped && i < 6; i++)
        say("xc5sb:   %c[%s %s]", class_isMetaClass(wrapped[i].cls) ? '+' : '-', class_getName(wrapped[i].cls),
            sel_getName(wrapped[i].sel));
}

static void image_added(const struct mach_header *header, intptr_t image_slide) {
    (void)header; (void)image_slide;
    guard_poster_kit();         /* once PosterKit's classes are there */
}

__attribute__((constructor)) static void xc5_sb_start(void) {
    if (strcmp(getprogname(), "SpringBoard") != 0) return;
    _dyld_register_func_for_add_image(image_added);         /* also called for every image already loaded */
    if (access(DROP_DIR "wallpaper.off", F_OK) == 0) {
        say("xc5sb: wallpaper.off: left alone");
        return;
    }
    say("xc5sb: loaded in SpringBoard (pid %d)", getpid());
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL,
        look_at_windows);
}
