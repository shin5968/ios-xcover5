/*
 * libxc5mtl.dylib -- Metal M3 on the XCover5: inserted into chronod (boot-arg xc5mtl=1/2,
 * bsd/dev/xc5_drop.c; needs xc5metal=1 for the IOAcceleratorES service, XC5GPU.cpp).
 *
 * Before chronod's main: put MetalPluginName / MetalPluginClassName on IOAcceleratorES, so that
 * this process's first MTLCreateSystemDefaultDevice (Metal answers it once per process) finds
 * gpu/metal's RedwoodMetal.bundle. Metal builds "/System/Library/Extensions/" + name + ".bundle";
 * the bundle lives in the drop zone, hence the name "../../../private/var/xc5/metal/RedwoodMetal".
 *
 * Then, on a thread of its own after a few seconds: ask Metal for the device, and if there is
 * one, a buffer, a command queue and an empty command buffer committed and waited on. Every step
 * says itself on the console first ("gl: mtl ..." through XC5GPU's Log selector), so that an
 * abort names the step it happened in. xc5mtl=1 takes the properties off again afterwards
 * (processes started later go back to no Metal); xc5mtl=2 leaves them.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_object_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_object_t service, task_port_t task, uint32_t type, io_object_t *connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(io_object_t connection, uint32_t selector, const uint64_t *input,
    uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt, uint64_t *output,
    uint32_t *outputCnt, void *outputStruct, size_t *outputStructCnt);
CFTypeRef IORegistryEntryCreateCFProperty(io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t set_cf_properties(io_object_t entry, CFTypeRef properties)
    __asm("_IORegistryEntrySetCFProperties");

#define PLUGIN_NAME  "../../../private/var/xc5/metal/RedwoodMetal"
#define PLUGIN_CLASS "RedwoodMetalDevice"
#define BUNDLE_EXE   "/private/var/xc5/metal/RedwoodMetal.bundle/RedwoodMetal"

static io_object_t log_conn;
static int mode = 1;

static void
say(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "mtl[%d] ", getpid());

	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(line)) {
		n = sizeof(line) - 1;
	}
	if (!log_conn) {
		io_object_t s = IOServiceGetMatchingService(0, IOServiceMatching("XC5GPU"));
		if (s) {
			if (IOServiceOpen(s, mach_task_self(), 0, &log_conn) != KERN_SUCCESS) {
				log_conn = 0;
			}
			IOObjectRelease(s);
		}
	}
	if (log_conn) {
		IOConnectCallMethod(log_conn, 5, NULL, 0, line, (size_t)n, NULL, NULL, NULL, NULL);
	}
	fprintf(stderr, "%s\n", line);
}

static void
read_mode(void)
{
	char args[1024] = { 0 };
	size_t len = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0) {
		const char *p = strstr(args, "xc5mtl=");
		if (p) {
			mode = atoi(p + 7);
		}
	}
}

static kern_return_t
set_props(io_object_t acc, const char *name, const char *cls)
{
	CFStringRef keys[2] = { CFSTR("MetalPluginName"), CFSTR("MetalPluginClassName") };
	CFStringRef vals[2] = {
		CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8),
		CFStringCreateWithCString(NULL, cls, kCFStringEncodingUTF8) };
	CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 2,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	kern_return_t kr = set_cf_properties(acc, d);

	CFRelease(d);
	CFRelease(vals[0]);
	CFRelease(vals[1]);
	return kr;
}

static const char *
cstr(id s)
{
	const char *c = s ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : NULL;
	return c ? c : "(nil)";
}

static id
send0(id o, const char *sel)
{
	return ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel));
}

static void *
probe(void *arg)
{
	(void)arg;
	sleep(6);
	say("step 1: bundle %s %s", BUNDLE_EXE, access(BUNDLE_EXE, R_OK) == 0 ? "is there" : "is MISSING");
	void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
	id (*create)(void) = metal ? (id (*)(void))dlsym(metal, "MTLCreateSystemDefaultDevice") : NULL;
	if (!create) {
		say("step 2: no MTLCreateSystemDefaultDevice: %s", dlerror());
		goto out;
	}
	say("step 2: asking Metal for the device");
	id dev = create();
	say("step 3: device %p class %s name %s", (void *)dev,
	    dev ? class_getName(object_getClass(dev)) : "-", dev ? cstr(send0(dev, "name")) : "-");
	if (!dev) {
		goto out;
	}
	say("step 4: buffer of 4096 bytes");
	id buf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(dev,
	    sel_registerName("newBufferWithLength:options:"), 4096, 0);
	void *contents = buf ? ((void *(*)(id, SEL))objc_msgSend)(buf, sel_registerName("contents")) : NULL;
	if (contents) {
		memset(contents, 0x5a, 4096);
	}
	say("step 5: buffer %p contents %p gpuAddress %#llx", (void *)buf, contents,
	    buf ? ((unsigned long long (*)(id, SEL))objc_msgSend)(buf, sel_registerName("gpuAddress")) : 0ull);
	say("step 6: command queue");
	id queue = send0(dev, "newCommandQueue");
	id cb = queue ? send0(queue, "commandBuffer") : nil;
	say("step 7: queue %p command buffer %p; commit and wait", (void *)queue, (void *)cb);
	if (cb) {
		send0(cb, "commit");
		send0(cb, "waitUntilCompleted");
		say("step 8: command buffer status %ld", ((long (*)(id, SEL))objc_msgSend)(cb, sel_registerName("status")));
	}
	say("done: Metal device works this far");
out:
	if (mode == 1) {
		io_object_t acc = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
		if (acc) {
			say("properties off: %#x", set_props(acc, "", ""));
			IOObjectRelease(acc);
		}
	}
	return NULL;
}

__attribute__((constructor)) static void
xc5_mtl_init(void)
{
	read_mode();
	io_object_t acc = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
	if (!acc) {
		say("no IOAcceleratorES (boot with xc5metal=1); nothing to do");
		return;
	}
	kern_return_t kr = set_props(acc, PLUGIN_NAME, PLUGIN_CLASS);
	CFTypeRef v = IORegistryEntryCreateCFProperty(acc, CFSTR("MetalPluginName"), NULL, 0);
	say("xc5mtl=%d: properties set %#x, MetalPluginName %s", mode, kr, v ? "reads back" : "is NOT there");
	if (v) {
		CFRelease(v);
	}
	IOObjectRelease(acc);
	pthread_t t;
	pthread_create(&t, NULL, probe, NULL);
	pthread_detach(t);
}
