// SPDX-License-Identifier: MIT
// XCover5: libxc5pb.dylib, inserted into PosterBoard by the kernel (boot-arg xc5pbhook=1, bsd/dev/xc5_drop.c).
//
// Boots 56-63: PosterBoard traps (SIGTRAP) on "Invalid condition not satisfying: !_stateLock_switcherConfiguration"
// (PBFPosterExtensionDataStore.m:3431), at the entry of -[PBFPosterExtensionDataStore
// _stateLock_setupSwitcherConfigurationForFirstTime]; the wallpaper stays black and the widgets "Waiting...".
// From the iOS 16.7 shared cache: the caller (-_stateLock_updateExtensions:...) runs the first-time setup when the
// flag at self+233 is 0 and then sets it; the setup asserts when self+152, the switcher configuration, is already
// there. Of the three stores to self+152 one is a lazy getter that makes a configuration when there is none. So
// something read the configuration before the extensions came in and the setup ran (on this slow phone; or with
// an empty data store), and the setup then found it made. Saved state and the data directory were ruled out in
// boots 61-63.
//
// Here: just before the original first-time setup, a switcher configuration that is already there is let go
// (the ivar set to nil), so the setup makes the real one as on a first boot. Every step is reported to the kernel
// log through XC5GPU's Log selector, which PosterBoard's stderr never reaches.
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
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

#define SEL_LOG 5               /* XC5GPU.cpp kSelLog, gpu/mesa/herodrm.c */

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

static Ivar switcher_ivar;
static void (*orig_setup)(id self, SEL cmd);
static unsigned setups, cleared;

// The ivar at self+152: by name if it is one of the expected ones, else whichever object ivar sits at 152.
static Ivar find_switcher_ivar(Class cls) {
    unsigned count = 0;
    Ivar *ivars = class_copyIvarList(cls, &count), found = NULL, at152 = NULL;

    for (unsigned i = 0; i < count; i++) {
        const char *name = ivar_getName(ivars[i]);
        const char *type = ivar_getTypeEncoding(ivars[i]);
        ptrdiff_t off = ivar_getOffset(ivars[i]);
        if (off == 152 && type != NULL && type[0] == '@') at152 = ivars[i];
        if (name != NULL && strstr(name, "witcherConfiguration") != NULL && strstr(name, "rdering") == NULL &&
            type != NULL && type[0] == '@' && found == NULL)
            found = ivars[i];
    }
    if (found == NULL) found = at152;
    if (found != NULL)
        say("xc5pb: switcher configuration ivar %s at %td (%s)", ivar_getName(found), ivar_getOffset(found),
            ivar_getTypeEncoding(found));
    else {
        say("xc5pb: no switcher configuration ivar among %u; the ivars at 140-170:", count);
        for (unsigned i = 0; i < count; i++) {
            ptrdiff_t off = ivar_getOffset(ivars[i]);
            if (off >= 140 && off <= 170)
                say("xc5pb:   %td %s %s", off, ivar_getName(ivars[i]), ivar_getTypeEncoding(ivars[i]));
        }
    }
    free(ivars);
    return found;
}

#define SEND0(type, obj, name) ((type (*)(id, SEL))objc_msgSend)((id)(obj), sel_registerName(name))

static int responds(id obj, const char *name) {
    return obj != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(obj, sel_registerName("respondsToSelector:"),
        sel_registerName(name));
}

static const char *text(id obj) {
    id d = obj != nil ? SEND0(id, obj, "description") : nil;
    const char *s = d != nil ? SEND0(const char *, d, "UTF8String") : NULL;
    return s != NULL ? s : "nil";
}

// Boot 64: past the first-time setup, PosterBoard traps on a nil "selectedLockPath" (+0x42654 of
// PosterBoard.framework): no lock screen poster was chosen. What extensions does the data store know, and what
// did the setup make? Long descriptions go out in pieces of 180 characters, at most 6.
static void say_long(const char *what, const char *s) {
    size_t len = strlen(s);
    say("xc5pb: %s (%zu chars):", what, len);
    for (size_t at = 0, piece = 0; at < len && piece < 6; at += 180, piece++)
        say("xc5pb:  %.180s", s + at);
}

// Boot 65: calling -knownExtensions from here took the data store's state lock, which the caller already holds
// (the setup is a _stateLock_ method): os_unfair_lock trapped. So no methods of the store: its object ivars are
// read directly, those whose names speak of extensions, posters or paths.
static void report_extensions(id self) {
    unsigned count = 0;
    Ivar *ivars = class_copyIvarList(object_getClass(self), &count);
    int shown = 0;

    for (unsigned i = 0; i < count && shown < 8; i++) {
        const char *name = ivar_getName(ivars[i]), *type = ivar_getTypeEncoding(ivars[i]);
        if (name == NULL || type == NULL || type[0] != '@') continue;
        if (strcasestr(name, "xtension") == NULL && strcasestr(name, "oster") == NULL &&
            strcasestr(name, "ath") == NULL && strcasestr(name, "elected") == NULL)
            continue;
        id value = object_getIvar(self, ivars[i]);
        unsigned long n = value != nil && responds(value, "count") ? SEND0(unsigned long, value, "count") : 0;
        say("xc5pb: ivar %s %s: %s, count %lu", name, type, value != nil ? object_getClassName(value) : "nil", n);
        if (value != nil && (n > 0 || !responds(value, "count"))) say_long(name, text(value));
        shown++;
    }
    free(ivars);
}

static void my_setup(id self, SEL cmd) {
    unsigned n = ++setups;
    id existing = switcher_ivar != NULL ? object_getIvar(self, switcher_ivar) : nil;

    if (n == 1) report_extensions(self);

    if (existing != nil) {
        cleared++;
        say("xc5pb: first-time setup #%u: a switcher configuration is already there (%s %p), let go", n,
            object_getClassName(existing), existing);
        object_setIvarWithStrongDefault(self, switcher_ivar, nil);
    } else {
        say("xc5pb: first-time setup #%u: no switcher configuration yet, as expected", n);
    }
    orig_setup(self, cmd);
    id made = switcher_ivar != NULL ? object_getIvar(self, switcher_ivar) : nil;
    say("xc5pb: first-time setup #%u done: switcher configuration %s %p", n,
        made != nil ? object_getClassName(made) : "nil", made);
    if (made != nil) say_long("the configuration made", text(made));
}

// Boot 66: the first-time setup was not called at all (boot 64's setup seems to have been saved), PosterBoard did
// not crash, and the wallpaper stayed black: nothing was reported, since the report was inside the setup. Now the
// data store is caught as it is made (its -initWith... methods, whatever their arguments: on arm64 they are all in
// x2-x7 and pass through), and its state is reported 20 s and 60 s after this library is loaded, from a queue of
// its own: ivars read directly, no lock taken, so a value may be mid-change, which is fine for a look.
#include <dispatch/dispatch.h>

static id the_store;
#define INIT_MAX 4
static SEL init_sels[INIT_MAX];
static IMP init_imps[INIT_MAX];
static int init_count;

typedef id (*init6_t)(id, SEL, id, id, id, id, id, id);

static id my_init(id self, SEL cmd, id a, id b, id c, id d, id e, id f) {
    IMP orig = NULL;
    for (int i = 0; i < init_count; i++)
        if (init_sels[i] == cmd) orig = init_imps[i];
    id made = orig != NULL ? ((init6_t)orig)(self, cmd, a, b, c, d, e, f) : nil;
    if (made != nil) {
        the_store = made;       /* the store lives as long as PosterBoard; not retained */
        say("xc5pb: data store %p made by -%s", made, sel_getName(cmd));
    }
    return made;
}

// Boot 67: the store's extension tables are empty and _stateLock_initialExtensionsLoaded is 0: PosterBoard was
// never told of any poster provider (extension point com.apple.posterkit.provider). Who is asked: the store's
// extension provider (a PBFPosterExtensionProvider), whose object ivars are listed here; and what the system
// answers when asked directly, through NSExtension (the same ExtensionKit/LaunchServices lookup).
static void dump_object(const char *what, id obj) {
    if (obj == nil) { say("xc5pb: %s: nil", what); return; }
    say("xc5pb: %s: %s %p", what, object_getClassName(obj), obj);
    for (Class c = object_getClass(obj); c != Nil && strcmp(class_getName(c), "NSObject") != 0;
         c = class_getSuperclass(c)) {
        unsigned count = 0;
        Ivar *ivars = class_copyIvarList(c, &count);
        for (unsigned i = 0; i < count && i < 16; i++) {
            const char *name = ivar_getName(ivars[i]), *type = ivar_getTypeEncoding(ivars[i]);
            if (name == NULL || type == NULL) continue;
            if (type[0] == 'B' || type[0] == 'c') {
                say("xc5pb:   %s.%s = %u", class_getName(c), name,
                    *((unsigned char *)(void *)obj + ivar_getOffset(ivars[i])));
                continue;
            }
            if (type[0] != '@') continue;
            id v = object_getIvar(obj, ivars[i]);
            unsigned long n = v != nil && responds(v, "count") ? SEND0(unsigned long, v, "count") : 0;
            say("xc5pb:   %s.%s: %s, count %lu", class_getName(c), name, v != nil ? object_getClassName(v) : "nil", n);
            if (v != nil && responds(v, "count") && n > 0) say_long(name, text(v));
        }
        free(ivars);
    }
}

static void ask_system(void) {
    Class ns = objc_getClass("NSExtension");
    SEL sel = sel_registerName("extensionsWithMatchingAttributes:error:");
    if (ns == Nil || !class_respondsToSelector(object_getClass((id)ns), sel)) {
        say("xc5pb: NSExtension lookup not available%s", "");
        return;
    }
    const void *k = CFSTR("NSExtensionPointName"), *v = CFSTR("com.apple.posterkit.provider");
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &k, &v, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    id error = nil;
    id found = ((id (*)(id, SEL, id, id *))objc_msgSend)((id)ns, sel, (id)attrs, &error);
    CFRelease(attrs);
    unsigned long n = found != nil && responds(found, "count") ? SEND0(unsigned long, found, "count") : 0;
    say("xc5pb: NSExtension lookup of com.apple.posterkit.provider: %lu found, error %s", n,
        error != nil ? "below" : "none");
    if (error != nil) say_long("the lookup's error", text(error));
    if (found != nil && n > 0) say_long("the extensions found", text(found));
}

static void report_state(const char *when) {
    say("xc5pb: --- state %s: data store %p, first-time setups %u ---", when, the_store, setups);
    ask_system();
    if (the_store == nil) return;
    {
        unsigned count = 0;
        Ivar *ivars = class_copyIvarList(object_getClass(the_store), &count);
        for (unsigned i = 0; i < count; i++) {
            const char *name = ivar_getName(ivars[i]), *type = ivar_getTypeEncoding(ivars[i]);
            if (name != NULL && type != NULL && type[0] == '@' && strcasestr(name, "rovider") != NULL &&
                strcasestr(name, "ssertion") == NULL)
                dump_object(name, object_getIvar(the_store, ivars[i]));
        }
        free(ivars);
    }
    unsigned count = 0;
    Ivar *ivars = class_copyIvarList(object_getClass(the_store), &count);
    int shown = 0;
    for (unsigned i = 0; i < count && shown < 14; i++) {
        const char *name = ivar_getName(ivars[i]), *type = ivar_getTypeEncoding(ivars[i]);
        if (name == NULL || type == NULL) continue;
        if (type[0] == 'B' || type[0] == 'c') {         /* the flags: first-time done (233) among them */
            unsigned char v = *((unsigned char *)(void *)the_store + ivar_getOffset(ivars[i]));
            say("xc5pb: ivar %s (%td) = %u", name, ivar_getOffset(ivars[i]), v);
            shown++;
            continue;
        }
        if (type[0] != '@') continue;
        if (strcasestr(name, "xtension") == NULL && strcasestr(name, "oster") == NULL &&
            strcasestr(name, "witcher") == NULL && strcasestr(name, "elected") == NULL &&
            strcasestr(name, "ath") == NULL)
            continue;
        id value = object_getIvar(the_store, ivars[i]);
        unsigned long n = value != nil && responds(value, "count") ? SEND0(unsigned long, value, "count") : 0;
        say("xc5pb: ivar %s: %s, count %lu", name, value != nil ? object_getClassName(value) : "nil", n);
        if (value != nil && (n > 0 || !responds(value, "count"))) say_long(name, text(value));
        shown++;
    }
    free(ivars);
}

static void hook_inits(Class cls) {
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count && init_count < INIT_MAX; i++) {
        const char *name = sel_getName(method_getName(methods[i]));
        if (strncmp(name, "initWith", 8) != 0) continue;
        init_sels[init_count] = method_getName(methods[i]);
        init_imps[init_count] = method_setImplementation(methods[i], (IMP)my_init);
        say("xc5pb: watching -%s", name);
        init_count++;
    }
    free(methods);
}

__attribute__((constructor)) static void xc5_pbhook(void) {
    if (strcmp(getprogname(), "PosterBoard") != 0) return;
    {
        Class store = objc_getClass("PBFPosterExtensionDataStore");
        if (store != Nil) hook_inits(store);
        dispatch_queue_t q = dispatch_queue_create("xc5pb.report", NULL);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), q, ^{ report_state("at 20 s"); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), q, ^{ report_state("at 60 s"); });
    }
    Class cls = objc_getClass("PBFPosterExtensionDataStore");
    SEL sel = sel_registerName("_stateLock_setupSwitcherConfigurationForFirstTime");
    Method m = cls != Nil ? class_getInstanceMethod(cls, sel) : NULL;

    say("xc5pb: loaded in PosterBoard (pid %d), PBFPosterExtensionDataStore %s, setup method %s", getpid(),
        cls != Nil ? "found" : "MISSING", m != NULL ? "found" : "MISSING");
    if (m == NULL) return;
    switcher_ivar = find_switcher_ivar(cls);
    if (switcher_ivar == NULL) return;
    orig_setup = (void (*)(id, SEL))method_setImplementation(m, (IMP)my_setup);
    say("xc5pb: first-time setup hooked");
}
