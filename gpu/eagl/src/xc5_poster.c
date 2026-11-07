// SPDX-License-Identifier: MIT
// XCover5: libxc5poster.dylib, inserted into the poster extensions by the kernel (boot-arg xc5poster=1,
// bsd/dev/xc5_drop.c): where does CollectionsPoster spend its start?
//
// Boots 71-72: PosterBoard picks CollectionsPoster (iOS 16's stock wallpaper) as the active configuration and launches
// it, but the extension is killed again and again with "reason ns 7 code 0x3: extension check-in timeout" (it never
// tells ExtensionKit it is up), and the 1105x1160 picture backboardd gets from it has 0 lit pixels. Nothing says what
// it waits for. So: every second for 40 s, the main thread's pc, lr and frame-pointer chain (8 return addresses), as
// shared-cache addresses with the slide taken off (symbolised on the host against the 16.7.16 cache), plus the
// image the pc is in. Through XC5GPU's Log selector, as the other libraries here.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
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
extern const void *_dyld_get_shared_cache_range(size_t *length);

#define SEL_LOG 5               /* XC5GPU.cpp kSelLog */
#define CACHE_BASE 0x180000000UL

static io_connect_t log_conn;
static uintptr_t slide, cache_lo, cache_hi;
static mach_port_t main_thread;

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

// A shared-cache address unslid, anything else as it is (marked with a '+').
static void fmt_addr(char *out, size_t size, uintptr_t a) {
    if (a >= cache_lo && a < cache_hi) snprintf(out, size, "%lx", (unsigned long)(a - slide));
    else snprintf(out, size, "+%lx", (unsigned long)a);
}

static int read_word(uintptr_t at, uintptr_t *value) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)at, sizeof *value, (vm_address_t)value, &got) ==
        KERN_SUCCESS && got == sizeof *value;
}

static void sample_thread(int n, mach_port_t th, const char *tag) {
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    char line[220], a[24];
    size_t len = 0;
    Dl_info info;

    if (thread_suspend(th) != KERN_SUCCESS) return;
    kern_return_t kr = thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &count);
    uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(st), lr = (uintptr_t)arm_thread_state64_get_lr(st);
    uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st), frames[8];
    int nf = 0;
    while (kr == KERN_SUCCESS && fp != 0 && nf < 8) {
        uintptr_t next, ret;
        if (!read_word(fp, &next) || !read_word(fp + 8, &ret) || ret == 0) break;
        frames[nf++] = ret & 0x0000000fffffffffUL;          /* PAC bits off */
        if (next <= fp) break;
        fp = next;
    }
    thread_resume(th);
    if (kr != KERN_SUCCESS) { say("xc5poster: #%d%s thread state: %d", n, tag, kr); return; }
    fmt_addr(a, sizeof a, pc & 0x0000000fffffffffUL);
    len += (size_t)snprintf(line + len, sizeof line - len, "xc5poster: #%d%s pc %s", n, tag, a);
    fmt_addr(a, sizeof a, lr & 0x0000000fffffffffUL);
    len += (size_t)snprintf(line + len, sizeof line - len, " lr %s bt", a);
    for (int i = 0; i < nf && len < sizeof line - 20; i++) {
        fmt_addr(a, sizeof a, frames[i]);
        len += (size_t)snprintf(line + len, sizeof line - len, " %s", a);
    }
    say("%s", line);
    if (dladdr((void *)(pc & 0x0000000fffffffffUL), &info) && info.dli_fname != NULL) {
        const char *base = strrchr(info.dli_fname, '/');
        say("xc5poster: #%d%s in %s (%s)", n, tag, base ? base + 1 : info.dli_fname,
            info.dli_sname ? info.dli_sname : "?");
    }
}

/* Boot 81: the Photos app's main thread waits on checkpointd's reply; checkpointd's work runs on queue
 * threads, so in a daemon (not an app) every thread is sampled, as "t1".."t7". */
static int all_threads;

static void sample(int n) {
    if (!all_threads) {
        sample_thread(n, main_thread, "");
        return;
    }
    thread_act_array_t list;
    mach_msg_type_number_t count = 0;
    mach_port_t self = mach_thread_self();
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    int shown = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        char tag[8];
        if (list[i] != self && shown < 7) {
            snprintf(tag, sizeof tag, " t%u", i);
            sample_thread(n, list[i], tag);
            shown++;
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof list[0]);
}

static void *sampler(void *arg) {
    (void)arg;
    for (int n = 1; n <= 40; n++) {
        sleep(all_threads ? 3 : 1);
        sample(n);
    }
    return NULL;
}

__attribute__((constructor)) static void xc5_poster_init(void) {
    size_t length = 0;
    const void *cache = _dyld_get_shared_cache_range(&length);
    pthread_t t;

    if (cache != NULL) {
        cache_lo = (uintptr_t)cache;
        cache_hi = cache_lo + length;
        slide = cache_lo - CACHE_BASE;
    }
    main_thread = mach_thread_self();       /* constructors run on the main thread */
    all_threads = strcmp(getprogname(), "checkpointd") == 0;
    say("xc5poster: loaded in %s (pid %d), cache slide %#lx", getprogname(), getpid(), (unsigned long)slide);
    if (pthread_create(&t, NULL, sampler, NULL) == 0) pthread_detach(t);
}
