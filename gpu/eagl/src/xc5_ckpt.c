// SPDX-License-Identifier: MIT
// XCover5: libxc5ckpt.dylib, inserted into checkpointd by the kernel (boot-arg xc5ckpt=1, bsd/dev/xc5_drop.c).
//
// Boots 81-82: the Photos app stays a white screen. Its main thread waits in MCCopyCheckpointValue
// ("ImageSpecifications", PhotoLibraryServicesCore's PLAssetFormats) for checkpointd's XPC reply, and checkpointd's
// own request thread waits in mach_msg (send+receive, no timeout) inside its FairPlay code: a MIG request to
// fairplayd.H2, which on this phone starts and exits at once (as on y2sios / the S9+ port, where appstored waited
// the same way in _AMSFairPlayGetDeviceType). The reply never comes, so neither does checkpointd's.
//
// Here: dyld interposing of mach_msg in checkpointd (its MIG stubs call the mach_msg import; XPC uses mach_msg2
// and is not touched). A send+receive with no receive timeout gets one of XC5_CKPT_MS milliseconds. When it runs
// out, checkpointd's own error path takes over (MACH_RCV_TIMED_OUT) and it answers its client without FairPlay;
// PhotoLibraryServices then reads ImageSpecifications from /System/Library/CoreServices/Checkpoint.xml itself.
// Every call that is limited, and every one that runs out, is said on the console ("gl: xc5ckpt ...").
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdarg.h>
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
#define XC5_CKPT_MS 3000

static io_connect_t log_conn;
static int said;

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

static mach_msg_return_t my_mach_msg(mach_msg_header_t *msg, mach_msg_option_t option, mach_msg_size_t send_size,
                                     mach_msg_size_t rcv_size, mach_port_name_t rcv_name, mach_msg_timeout_t timeout,
                                     mach_port_name_t notify) {
    static int seen;
    /* boot 83: no "limited" line at all, so every call is said once now (the first 12), whatever its options */
    if (seen < 12) {
        seen++;
        say("xc5ckpt: mach_msg #%d option %#x id %d send %u rcv %u timeout %u", seen, option,
            msg ? msg->msgh_id : -1, send_size, rcv_size, timeout);
    }
    if ((option & MACH_SEND_MSG) && (option & MACH_RCV_MSG) && !(option & MACH_RCV_TIMEOUT) && msg != NULL) {
        mach_msg_id_t id = msg->msgh_id;
        if (said < 20) {
            said++;
            say("xc5ckpt: pid %d mach_msg id %d (%#x) to port %#x: receive limited to %d ms", getpid(), id, id,
                msg->msgh_remote_port, XC5_CKPT_MS);
        }
        mach_msg_return_t r = mach_msg(msg, option | MACH_RCV_TIMEOUT, send_size, rcv_size, rcv_name, XC5_CKPT_MS,
                                       notify);
        if (r != MACH_MSG_SUCCESS)
            say("xc5ckpt: id %d answered %#x%s", id, r, r == MACH_RCV_TIMED_OUT ? " (no reply in time)" : "");
        return r;
    }
    return mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement, *original;
} interposers[] = {
    { (const void *)my_mach_msg, (const void *)mach_msg },
};

__attribute__((constructor)) static void xc5_ckpt_init(void) {
    say("xc5ckpt: loaded in %s (pid %d): FairPlay waits limited to %d ms", getprogname(), getpid(), XC5_CKPT_MS);
}
