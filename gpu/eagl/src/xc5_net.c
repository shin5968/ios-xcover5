// SPDX-License-Identifier: MIT
// XCover5: libxc5net.dylib, inserted into configd by the kernel (boot-arg xc5net=1, bsd/dev/xc5_drop.c).
//
// Wi-Fi step 4c (wifi18): the kernel's own Wi-Fi driver (bsd/dev/xc5_wlbt.c) joins a network, gets an address by
// DHCP and brings up an Ethernet interface (en0) with that address and a default route; the stack pings the gateway.
// But apps need more: configd (its IPMonitor) has to know the interface as a network service, make it the primary
// interface and hand a DNS configuration to the resolver (mDNSResponder). Nobody tells configd here: the interface
// is not an IOKit network interface and no service in the preferences is bound to it.
//
// So, inside configd: wait for the kernel's sysctl kern.xc5wifi ("up <if> <address> <mask> <router> <dns>"), then
// put in the dynamic store what an IP configuration agent would publish for a service of its own:
//   State:/Network/Service/XC5WIFI/IPv4  { Addresses, SubnetMasks, Router, InterfaceName }
//   State:/Network/Service/XC5WIFI/DNS   { ServerAddresses }
//   State:/Network/Interface/<if>/Link    { Active = true }
// and report what came of it in kern.xc5netstat (bit 0 written, bit 1 the global IPv4 primary interface is ours,
// bit 2 a global DNS configuration exists, bit 3 a name resolved; 0x100 + SCError() when the write failed), and
// on the console through the XC5GPU log selector.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
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
#define SERVICE "XC5WIFI"

/*
 * The iOS SDK marks the dynamic store API unavailable on iOS (it is there, configd and its plugins use it): the
 * functions are looked up at run time, the keys and property names written out.
 */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
static SCDynamicStoreRef (*SCDynamicStoreCreate)(CFAllocatorRef, CFStringRef, void *, void *);
static Boolean (*SCDynamicStoreSetMultiple)(SCDynamicStoreRef, CFDictionaryRef, CFArrayRef, CFArrayRef);
static CFPropertyListRef (*SCDynamicStoreCopyValue)(SCDynamicStoreRef, CFStringRef);
static int (*SCError)(void);
static const char *(*SCErrorString)(int);

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

static void report(int stat) {
    sysctlbyname("kern.xc5netstat", NULL, NULL, &stat, sizeof stat);
}

static CFArrayRef one(const char *s) {
    CFStringRef v = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    CFArrayRef a = CFArrayCreate(NULL, (const void **)&v, 1, &kCFTypeArrayCallBacks);
    CFRelease(v);
    return a;
}

static void put(CFMutableDictionaryRef d, CFStringRef k, CFTypeRef v) {
    CFDictionarySetValue(d, k, v);
    CFRelease(v);
}

static CFStringRef str(const char *s) {
    return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static int publish(SCDynamicStoreRef st, const char *ifn, const char *ip, const char *mask, const char *gw,
    const char *dns) {
    CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFMutableDictionaryRef v4 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    put(v4, CFSTR("Addresses"), one(ip));
    put(v4, CFSTR("SubnetMasks"), one(mask));
    put(v4, CFSTR("Router"), str(gw));
    put(v4, CFSTR("InterfaceName"), str(ifn));
    CFStringRef k = CFSTR("State:/Network/Service/" SERVICE "/IPv4");
    CFDictionarySetValue(all, k, v4);
    CFRelease(v4);

    CFMutableDictionaryRef dn = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    put(dn, CFSTR("ServerAddresses"), one(dns));
    CFDictionarySetValue(all, CFSTR("State:/Network/Service/" SERVICE "/DNS"), dn);
    CFRelease(dn);

    CFMutableDictionaryRef link = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(link, CFSTR("Active"), kCFBooleanTrue);
    k = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Interface/%s/Link"), ifn);
    CFDictionarySetValue(all, k, link);
    CFRelease(k);
    CFRelease(link);

    Boolean ok = SCDynamicStoreSetMultiple(st, all, NULL, NULL);
    CFRelease(all);
    return ok ? 0 : SCError();
}

/* bit 1: State:/Network/Global/IPv4 names our interface primary; bit 2: State:/Network/Global/DNS exists */
static int check(SCDynamicStoreRef st, const char *ifn) {
    int bits = 0;
    CFDictionaryRef g = SCDynamicStoreCopyValue(st, CFSTR("State:/Network/Global/IPv4"));
    if (g != NULL && CFGetTypeID(g) == CFDictionaryGetTypeID()) {
        CFStringRef p = CFDictionaryGetValue(g, CFSTR("PrimaryInterface"));
        char name[32] = "";
        if (p != NULL && CFGetTypeID(p) == CFStringGetTypeID())
            CFStringGetCString(p, name, sizeof name, kCFStringEncodingUTF8);
        if (strcmp(name, ifn) == 0) bits |= 2;
        say("xc5net: global IPv4 primary interface %s", name[0] ? name : "(none)");
        CFRelease(g);
    } else {
        if (g != NULL) CFRelease(g);
        say("xc5net: no global IPv4 yet");
    }
    CFPropertyListRef d = SCDynamicStoreCopyValue(st, CFSTR("State:/Network/Global/DNS"));
    if (d != NULL) {
        bits |= 4;
        CFRelease(d);
    }
    return bits;
}

/* bit 3: a name resolves through the system resolver */
static int resolve(void) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo("www.apple.com", "80", &hints, &res);
    if (e != 0 || res == NULL) {
        say("xc5net: getaddrinfo www.apple.com: %d", e);
        return 0;
    }
    char a[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, a, sizeof a);
    say("xc5net: www.apple.com is %s", a);
    freeaddrinfo(res);
    return 8;
}

/*
 * bit 4: an HTTP request over TCP through en0 gets its answer -- http://captive.apple.com/hotspot-detect.html, which
 * says "Success" (what iOS itself asks to see whether a network reaches the internet). wifi19: names resolve, but
 * Safari's page stays black (white when scrolled past): this tells the network from the drawing.
 */
static int fetch(void) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("captive.apple.com", "80", &hints, &res) != 0 || res == NULL) {
        say("xc5net: captive.apple.com does not resolve");
        return 0;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 10, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (s < 0 || connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        say("xc5net: connect to captive.apple.com:80 failed");
        if (s >= 0) close(s);
        freeaddrinfo(res);
        return 0;
    }
    freeaddrinfo(res);
    static const char req[] = "GET /hotspot-detect.html HTTP/1.0\r\nHost: captive.apple.com\r\n"
        "User-Agent: xc5net\r\n\r\n";
    send(s, req, sizeof req - 1, 0);
    char buf[1024];
    size_t got = 0;
    ssize_t n;
    while (got < sizeof buf - 1 && (n = recv(s, buf + got, sizeof buf - 1 - got, 0)) > 0)
        got += (size_t)n;
    buf[got] = 0;
    close(s);
    int success = strstr(buf, "Success") != NULL;
    char *eol = strchr(buf, '\r');
    if (eol) *eol = 0;
    say("xc5net: HTTP %zu bytes: %.60s%s", got, buf, success ? ", Success" : "");
    return (got > 0 && strncmp(buf, "HTTP/1.", 7) == 0) ? 0x10 : 0;
}

static void *worker(void *arg) {
    (void)arg;
    char info[128], ifn[16], ip[16], mask[16], gw[16], dns[16];
    say("xc5net: in configd (pid %d), waiting for kern.xc5wifi", getpid());
    for (int s = 0; s < 1200; s++) {
        size_t len = sizeof info - 1;
        memset(info, 0, sizeof info);
        if (sysctlbyname("kern.xc5wifi", info, &len, NULL, 0) == 0 &&
            sscanf(info, "up %15s %15s %15s %15s %15s", ifn, ip, mask, gw, dns) == 5)
            break;
        info[0] = 0;
        sleep(1);
    }
    if (strncmp(info, "up ", 3) != 0) {
        say("xc5net: the Wi-Fi interface never came up");
        return NULL;
    }
    say("xc5net: %s %s/%s router %s dns %s", ifn, ip, mask, gw, dns);
    void *sc = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
    if (sc != NULL) {
        SCDynamicStoreCreate = dlsym(sc, "SCDynamicStoreCreate");
        SCDynamicStoreSetMultiple = dlsym(sc, "SCDynamicStoreSetMultiple");
        SCDynamicStoreCopyValue = dlsym(sc, "SCDynamicStoreCopyValue");
        SCError = dlsym(sc, "SCError");
        SCErrorString = dlsym(sc, "SCErrorString");
    }
    if (SCDynamicStoreCreate == NULL || SCDynamicStoreSetMultiple == NULL || SCDynamicStoreCopyValue == NULL ||
        SCError == NULL || SCErrorString == NULL) {
        say("xc5net: SystemConfiguration functions not found (%s)", dlerror());
        report(0x1ff);
        return NULL;
    }
    SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("xc5net"), NULL, NULL);
    if (st == NULL) {
        say("xc5net: SCDynamicStoreCreate: %d", SCError());
        report(0x100 + SCError());
        return NULL;
    }
    int e = publish(st, ifn, ip, mask, gw, dns);
    if (e != 0) {
        say("xc5net: SCDynamicStoreSetMultiple: %d %s", e, SCErrorString(e));
        report(0x100 + e);
        return NULL;
    }
    say("xc5net: service " SERVICE " published");
    int stat = 1;
    report(stat);
    for (int t = 0; t < 60; t++) {
        sleep(2);
        int bits = check(st, ifn);
        if ((bits & 4) && !(stat & 8))
            bits |= resolve();
        if ((bits & 8) || (stat & 8))
            bits |= fetch();
        stat = 1 | bits | (stat & 0x18);
        report(stat);
        if ((stat & 0x1e) == 0x1e) break;
    }
    say("xc5net: done, state %#x", stat);
    return NULL;
}

__attribute__((constructor)) static void xc5net_init(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0)
        pthread_detach(t);
}
