/*
 * XCover5: y2sios's Metal driver (redwoodmetal.m, redwooddrawable.m) talks to its own kernel's
 * RedwoodGPU/RedwoodMali user client with struct calls numbered as Linux's ioctls. This port's
 * kernel has XC5GPU instead (tools/xcover5/XC5GPU.cpp), whose selectors take scalars:
 *
 *   0 GetParam  in param                out value
 *   1 CreateBO  in size                 out handle, gpu va, size
 *   2 FreeBO    in handle
 *   3 MapBO     in handle               out cpu address (one mapping per BO, kept until FreeBO)
 *   5 Log       in a line (struct)
 *   6 SurfaceBO in IOSurface ID         out handle, va, size, bpr, width, height, format, offset, planes
 *
 * This header is found before the SDK's (build-metal.sh puts its directory first with -I),
 * includes the real one, and then renames the four calls the driver makes to the GPU so that
 * the driver's own source stays y2sios's. RedwoodMali is answered by XC5GPU; RedwoodGPU (their
 * Adreno) by nothing, so the driver takes its Mali (panfrost) paths.
 */
#ifndef XC5_IOKITLIB_SHIM_H
#define XC5_IOKITLIB_SHIM_H

#if __has_include_next(<IOKit/IOKitLib.h>)
#include_next <IOKit/IOKitLib.h>
#else
/* the iOS SDK has IOKit's library but not its headers: the part this driver uses */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t, io_connect_t, io_registry_entry_t;
typedef kern_return_t IOReturn;
typedef UInt32 IOOptionBits;
#define IO_OBJECT_NULL ((io_object_t)0)
#define kIOMainPortDefault ((mach_port_t)0)
#define kIOMasterPortDefault ((mach_port_t)0)
#define kIOMapAnywhere 0x00000001
#define kIOReturnSuccess KERN_SUCCESS
#define kIOReturnError ((IOReturn)0xe00002bc)
#define kIOReturnBadArgument ((IOReturn)0xe00002c2)
#define kIOReturnUnsupported ((IOReturn)0xe00002c7)
#define kIOReturnNotFound ((IOReturn)0xe00002f0)
#ifdef __cplusplus
extern "C" {
#endif
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOObjectRetain(io_object_t object);
kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t entry, uint64_t *entryID);
kern_return_t IOConnectCallMethod(io_connect_t connection, uint32_t selector, const uint64_t *input,
    uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt, uint64_t *output,
    uint32_t *outputCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector,
    const void *inputStruct, size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, IOOptionBits options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    mach_vm_address_t atAddress);
#ifdef __cplusplus
}
#endif
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { XC5_SEL_PARAM, XC5_SEL_CREATE, XC5_SEL_FREE, XC5_SEL_MAP, XC5_SEL_SUBMIT, XC5_SEL_LOG,
       XC5_SEL_SURFACE_BO, XC5_SEL_SYNC_BO };

/* sizes of the buffers made through the shim, for IOConnectMapMemory64's ofSize */
#define XC5_SHIM_BOS 4096
static uint64_t xc5_shim_bo_size[XC5_SHIM_BOS + 1];

static inline void
xc5_shim_log(io_connect_t c, const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (c != 0 && n > 0) {
		IOConnectCallMethod(c, XC5_SEL_LOG, NULL, 0, line,
		    (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1, NULL, NULL, NULL, NULL);
	}
}

static inline CFMutableDictionaryRef
xc5_service_matching(const char *name)
{
	if (name != NULL && strcmp(name, "RedwoodMali") == 0) {
		return IOServiceMatching("XC5GPU");
	}
	if (name != NULL && strcmp(name, "RedwoodGPU") == 0) {
		return IOServiceMatching("XC5GPU-no-adreno");     /* matches nothing */
	}
	return IOServiceMatching(name);
}

static inline uint32_t
xc5_scalar(io_connect_t c, uint32_t sel, uint64_t in, uint64_t *out, uint32_t nout)
{
	uint32_t n = nout;
	return IOConnectCallMethod(c, sel, &in, 1, NULL, 0, out, out ? &n : NULL, NULL, NULL);
}

/* y2sios's struct requests, answered by XC5GPU's scalar ones */
static inline kern_return_t
xc5_struct_call(io_connect_t c, uint32_t sel, const void *in, size_t in_size, void *out,
		size_t *out_size)
{
	switch (sel) {
	case 0x44: {            /* panfrost GET_PARAM {u32 param, pad; u64 value} */
		struct { uint32_t param, pad; uint64_t value; } p;
		uint64_t v = 0;
		kern_return_t kr;

		if (in_size < sizeof(p) || out == NULL || out_size == NULL || *out_size < sizeof(p)) {
			return kIOReturnBadArgument;
		}
		memcpy(&p, in, sizeof(p));
		kr = xc5_scalar(c, XC5_SEL_PARAM, p.param, &v, 1);
		if (kr == KERN_SUCCESS) {
			p.value = v;
			memcpy(out, &p, sizeof(p));
			*out_size = sizeof(p);
		}
		return kr;
	}
	case 0x42: {            /* panfrost CREATE_BO {u32 size, flags, handle, pad; u64 offset} */
		struct { uint32_t size, flags, handle, pad; uint64_t offset; } bo;
		uint64_t o[3] = { 0 };
		kern_return_t kr;

		if (in_size < sizeof(bo) || out == NULL || out_size == NULL || *out_size < sizeof(bo)) {
			return kIOReturnBadArgument;
		}
		memcpy(&bo, in, sizeof(bo));
		kr = xc5_scalar(c, XC5_SEL_CREATE, bo.size, o, 3);
		if (kr != KERN_SUCCESS || o[0] == 0) {
			xc5_shim_log(c, "metal shim: CreateBO %u bytes: %#x", bo.size, kr);
			return kr != KERN_SUCCESS ? kr : kIOReturnError;
		}
		bo.handle = (uint32_t)o[0];
		bo.offset = o[1];
		if (o[0] <= XC5_SHIM_BOS) {
			xc5_shim_bo_size[o[0]] = o[2];
		}
		memcpy(out, &bo, sizeof(bo));
		*out_size = sizeof(bo);
		return KERN_SUCCESS;
	}
	case 0x09: {            /* GEM_CLOSE {u32 handle, pad} */
		uint32_t handle;

		if (in_size < sizeof(handle)) {
			return kIOReturnBadArgument;
		}
		memcpy(&handle, in, sizeof(handle));
		if (handle <= XC5_SHIM_BOS) {
			xc5_shim_bo_size[handle] = 0;
		}
		return xc5_scalar(c, XC5_SEL_FREE, handle, NULL, 0);
	}
	case 0x7e: {            /* kRedwoodSurface: struct rwd_surface (redwood_drawable.h) */
		struct {
			uint32_t surface, handle;
			uint64_t size;
			uint32_t offset, rowBytes, width, height, pixelFormat, planes;
			uint32_t planeOffset[2], planeRowBytes[2];
		} s;
		uint64_t o[9] = { 0 };
		kern_return_t kr;
		int describe;

		if (in_size < sizeof(s) || out == NULL || out_size == NULL || *out_size < sizeof(s)) {
			return kIOReturnBadArgument;
		}
		memcpy(&s, in, sizeof(s));
		describe = s.handle == 0xffffffffu;
		kr = xc5_scalar(c, XC5_SEL_SURFACE_BO, s.surface, o, 9);
		if (kr != KERN_SUCCESS || o[0] == 0) {
			return kr != KERN_SUCCESS ? kr : kIOReturnNotFound;
		}
		s.handle = describe ? 0 : (uint32_t)o[0];
		s.size = o[2];
		s.rowBytes = (uint32_t)o[3];
		s.width = (uint32_t)o[4];
		s.height = (uint32_t)o[5];
		s.pixelFormat = (uint32_t)o[6];
		s.offset = (uint32_t)o[7];
		s.planes = (uint32_t)o[8];
		s.planeOffset[0] = s.offset;
		s.planeRowBytes[0] = s.rowBytes;
		s.planeOffset[1] = s.planeRowBytes[1] = 0;
		if (describe) {
			xc5_scalar(c, XC5_SEL_FREE, o[0], NULL, 0);
		} else if (o[0] <= XC5_SHIM_BOS) {
			xc5_shim_bo_size[o[0]] = o[2];
		}
		memcpy(out, &s, sizeof(s));
		*out_size = sizeof(s);
		return KERN_SUCCESS;
	}
	default:                /* msm's (0x40, 0x43) and anything else: the Adreno's, not here */
		xc5_shim_log(c, "metal shim: request %#x has no XC5GPU form", sel);
		return kIOReturnUnsupported;
	}
}

static inline kern_return_t
xc5_map_memory(io_connect_t c, uint32_t handle, task_port_t task, mach_vm_address_t *address,
	       mach_vm_size_t *size, IOOptionBits options)
{
	uint64_t cpu = 0;
	kern_return_t kr;

	(void)task;
	(void)options;
	kr = xc5_scalar(c, XC5_SEL_MAP, handle, &cpu, 1);
	if (kr != KERN_SUCCESS || cpu == 0) {
		return kr != KERN_SUCCESS ? kr : kIOReturnError;
	}
	*address = cpu;
	if (size != NULL) {
		*size = handle <= XC5_SHIM_BOS ? xc5_shim_bo_size[handle] : 0;
	}
	return KERN_SUCCESS;
}

/* the kernel keeps one mapping per BO until FreeBO */
static inline kern_return_t
xc5_unmap_memory(io_connect_t c, uint32_t handle, task_port_t task, mach_vm_address_t address)
{
	(void)c;
	(void)handle;
	(void)task;
	(void)address;
	return KERN_SUCCESS;
}

#define IOServiceMatching(name) xc5_service_matching(name)
#define IOConnectCallStructMethod xc5_struct_call
#define IOConnectMapMemory64 xc5_map_memory
#define IOConnectUnmapMemory64 xc5_unmap_memory

#endif /* XC5_IOKITLIB_SHIM_H */
