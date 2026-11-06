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

static void my_setup(id self, SEL cmd) {
    unsigned n = ++setups;
    id existing = switcher_ivar != NULL ? object_getIvar(self, switcher_ivar) : nil;

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
}

__attribute__((constructor)) static void xc5_pbhook(void) {
    if (strcmp(getprogname(), "PosterBoard") != 0) return;
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
