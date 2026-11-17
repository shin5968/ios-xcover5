// viva-gpu back-end for the Galaxy XCover5 (VGPU_BACKEND=xc5, the default in this build).
//
// viva-gpu (https://github.com/Nik300/viva-gpu, GPL-3.0-or-later with an attribution term, see NOTICE there) talks
// to its own kernel driver through uapi/viva_gpu_uapi.h. The XCover5's kernel has a smaller GPU user client,
// XC5GPU (tools/xcover5/XC5GPU.cpp in ios-xcover5), with HeroGPU's selectors:
//   0 GetParam(param) -> value            (DRM_PANFROST_PARAM_* numbers, as vgpu_get_param)
//   1 CreateBO(size) -> handle, va, size   (wired kernel memory, 4 KiB GPU pages)
//   2 FreeBO(handle)
//   3 MapBO(handle) -> CPU address         (one mapping per BO, kept until it is freed)
//   4 Submit(jc, js) -> status, us         (synchronous: returns when the chain has finished)
//   5 Log(string)
//   6 SurfaceBO(surface id) -> handle, va, size, bytes per row, width, height, format, offset, planes
//   7 SyncBO(handle)                       (clean the CPU caches of an imported surface)
// This file answers every viva selector with those. Submits are synchronous, so a syncobj is signalled as its
// submit returns, the completion page (kept in this process) always says completed = submitted, and WAIT_BO never
// waits. Written for ios-xcover5 after vgpu_mock.c; distributed under the same licence as viva-gpu.
#include "vgpu.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "iokit_compat.h"
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <signal.h>
#include <sys/ucontext.h>

kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t inputCnt,
                                  const void *inputStruct, size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt,
                                  void *outputStruct, size_t *outputStructCnt);

enum { XSEL_PARAM, XSEL_CREATE, XSEL_FREE, XSEL_MAP, XSEL_SUBMIT, XSEL_LOG, XSEL_SURFACE_BO, XSEL_SYNC_BO };

#define XC5_MAX_BOS 4096        // the kernel's handles are 1..4096
#define XC5_MAX_SYNC 65536
#define XC5_PAGE 4096u          // the A5XELTE/XCover5 kernel runs 4 KiB pages; the GPU maps 4 KiB pages

typedef struct {
    uint64_t va, size, cpu;
    uint64_t gpu_offset;        // imported plane: its first byte's offset in the BO
    uint32_t flags, madv;
    bool imported, used;
} XBO;

typedef struct {
    io_connect_t conn;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    XBO bos[XC5_MAX_BOS + 1];
    uint8_t *syncobj;           // 0 free, 1 unsignalled, 2 signalled
    uint32_t nextSync;
    struct vgpu_completion *completion;
    uint64_t seq, faults;
} XConn;

static int xerr(kern_return_t kr)
{
    switch (kr) {
    case kIOReturnSuccess: return 0;
    case kIOReturnNoMemory: case kIOReturnNoResources: return -ENOMEM;
    case kIOReturnBadArgument: return -EINVAL;
    case kIOReturnNotFound: return -ENOENT;
    case kIOReturnUnsupported: return -ENOTSUP;
    default: return -EIO;
    }
}

static kern_return_t xcall(XConn *c, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
    uint32_t n = nout;
    return IOConnectCallMethod(c->conn, sel, in, nin, NULL, 0, out, out ? &n : NULL, NULL, NULL);
}

// One line into the kernel log ("XC5GPU client: viva: ..."), so that it shows in PREV BOOT LOG.
static void xlog(XConn *c, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    if (c && c->conn) IOConnectCallMethod(c->conn, XSEL_LOG, NULL, 0, line, (size_t)n, NULL, NULL, NULL, NULL);
}


// viva5: backboardd crashed (SIGSEGV, far 0xcca8) with the driver loaded and nothing said. Its stderr goes nowhere, so
// the driver's own messages are sent to the kernel log, and a fault says the image, offset and symbol of pc and lr.
static io_connect_t g_logconn;
static void klog(const char *s, size_t n)
{
    if (g_logconn && n) IOConnectCallMethod(g_logconn, XSEL_LOG, NULL, 0, s, n, NULL, NULL, NULL, NULL);
}
static int err_write(void *cookie, const char *buf, int len)
{
    (void)cookie;
    static char line[200];
    static size_t used;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n' || used == sizeof line - 1) { klog(line, used); used = 0; if (buf[i] == '\n') continue; }
        line[used++] = buf[i];
    }
    return len;
}
static void where(const char *what, uint64_t a)
{
    Dl_info d;
    char s[200];
    int n;
    if (dladdr((void *)(uintptr_t)a, &d) && d.dli_fname) {
        const char *b = strrchr(d.dli_fname, '/');
        n = snprintf(s, sizeof s, "viva: %s %s+0x%llx %s+0x%llx", what, b ? b + 1 : d.dli_fname,
                     (unsigned long long)(a - (uint64_t)(uintptr_t)d.dli_fbase), d.dli_sname ? d.dli_sname : "?",
                     (unsigned long long)(d.dli_saddr ? a - (uint64_t)(uintptr_t)d.dli_saddr : 0));
    } else {
        n = snprintf(s, sizeof s, "viva: %s 0x%llx (no image)", what, (unsigned long long)a);
    }
    klog(s, n > 0 ? (size_t)n : 0);
}
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    char s[96];
    int n = snprintf(s, sizeof s, "viva: %s in %s, address %p", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", getprogname(), si->si_addr);
    klog(s, (size_t)n);
    where("pc", u->uc_mcontext->__ss.__pc);
    where("lr", u->uc_mcontext->__ss.__lr);
    uint64_t fp = u->uc_mcontext->__ss.__fp;
    for (int i = 0; i < 6 && fp; i++) {
        uint64_t *f = (uint64_t *)(uintptr_t)fp;
        where("bt", f[1]);
        fp = f[0];
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
static void install_diag(io_connect_t conn)
{
    if (g_logconn) return;
    g_logconn = conn;
    FILE *f = funopen(NULL, NULL, err_write, NULL, NULL);
    if (f) { setvbuf(f, NULL, _IOLBF, 0); *stderr = *f; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
static int xc5_open(void **out)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("XC5GPU"));
    if (!svc) return -ENODEV;
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) return xerr(kr);
    XConn *c = calloc(1, sizeof *c);
    if (!c) { IOServiceClose(conn); return -ENOMEM; }
    c->conn = conn;
    c->syncobj = calloc(XC5_MAX_SYNC, 1);
    c->nextSync = 1;
    c->completion = mmap(NULL, (size_t)getpagesize(), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (c->completion == MAP_FAILED) c->completion = NULL;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    install_diag(conn);
    static int said;
    if (!said++) xlog(c, "viva: XC5GPU back-end opened (pid %d)", getpid());
    *out = c;
    return 0;
}

static void xc5_close(void *conn)
{
    XConn *c = conn;
    for (uint32_t h = 1; h <= XC5_MAX_BOS; h++)
        if (c->bos[h].used) { uint64_t in = h; xcall(c, XSEL_FREE, &in, 1, NULL, 0); }
    IOServiceClose(c->conn);
    free(c->syncobj);
    free(c);
}

static XBO *xbo(XConn *c, uint32_t h) { return h && h <= XC5_MAX_BOS && c->bos[h].used ? &c->bos[h] : NULL; }

static int get_param(XConn *c, struct vgpu_get_param *p)
{
    switch (p->param) {
    case VGPU_PARAM_SELECTED_COHERENCY: p->value = VGPU_COHERENCY_NONE; return 0;
    case VGPU_PARAM_SYSTEM_TIMESTAMP: p->value = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 38; return 0;  // ~26 MHz
    case VGPU_PARAM_SYSTEM_TIMESTAMP_FREQUENCY: p->value = 26000000; return 0;
    case VGPU_PARAM_ALLOWED_JM_CTX_PRIORITIES: p->value = 1u << 1; return 0;
    default: break;
    }
    uint64_t in = p->param, out = 0;
    if (xcall(c, XSEL_PARAM, &in, 1, &out, 1) != kIOReturnSuccess) return -EINVAL;
    // ASTC hidden (as herodrm): the G52 here took iOS's ASTC wallpapers and showed black; Mesa decodes them instead.
    if (p->param == VGPU_PARAM_TEXTURE_FEATURES0 && !getenv("VIVA_XC5_ASTC")) out &= ~(0xfull << 20);
    p->value = out;
    return 0;
}

static int wait_syncobjs(XConn *c, struct vgpu_syncobj_wait *w)
{
    const uint32_t *h = (const uint32_t *)(uintptr_t)w->handles;
    int64_t left = w->timeout_nsec - (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
    for (;;) {
        uint32_t done = 0, first = UINT32_MAX;
        for (uint32_t i = 0; i < w->count_handles; i++) {
            if (!h[i] || h[i] >= XC5_MAX_SYNC || !c->syncobj[h[i]]) return -EINVAL;
            if (c->syncobj[h[i]] == 2) { done++; if (first == UINT32_MAX) first = i; }
        }
        if ((w->flags & VGPU_SYNCOBJ_WAIT_ALL) ? done == w->count_handles : done > 0) {
            w->first_signaled = first == UINT32_MAX ? 0 : first;
            return 0;
        }
        if (left <= 0) return -ETIME;
        struct timespec rel = { (time_t)(left / 1000000000), (long)(left % 1000000000) };
        pthread_cond_timedwait_relative_np(&c->cond, &c->lock, &rel);
        left = w->timeout_nsec - (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
    }
}

static int xc5_submit(XConn *c, struct vgpu_submit *a)
{
    if (!a->jc) return -EINVAL;
    if (a->out_sync && (a->out_sync >= XC5_MAX_SYNC || !c->syncobj[a->out_sync])) return -ENOENT;
    uint64_t in[2] = { a->jc, (a->requirements & VGPU_JD_REQ_FS) ? 0 : 1 }, out[2] = { 0, 0 };
    // viva8: fragment jobs failed 0x5a with fault_ptr inside the job's own descriptor BO (mmu 0), ~12% of chains:
    // the GPU read descriptors the CPU had not yet written back (the "write-combine" mapping can come back
    // cacheable here). Clean every CPU-mapped BO to the point of coherency before the GPU reads it.
    if (!getenv("VIVA_XC5_NOCLEAN"))
        for (uint32_t h = 1; h <= XC5_MAX_BOS; h++) {
            XBO *b = &c->bos[h];
            if (b->used && b->cpu && !b->imported && !(b->flags & VGPU_BO_HEAP))
                sys_dcache_flush((void *)(uintptr_t)b->cpu, (size_t)b->size);
        }
    __asm__ volatile ("dsb sy" ::: "memory");
    kern_return_t kr = xcall(c, XSEL_SUBMIT, in, 2, out, 2);
    if ((kr || out[0]) && c->faults++ < 8)
    {
        xlog(c, "viva: submit jc 0x%llx js %llu: kr 0x%x status 0x%llx", (unsigned long long)a->jc,
             (unsigned long long)in[1], kr, (unsigned long long)out[0]);
        // the job header: status, first_incomplete, fault pointer (the address that was out of range)
        for (uint32_t h = 1; h <= XC5_MAX_BOS; h++) {
            XBO *b = &c->bos[h];
            if (!b->used || a->jc < b->va || a->jc + 32 > b->va + b->size) continue;
            if (!b->cpu) { uint64_t i2 = h, o2 = 0; if (xcall(c, XSEL_MAP, &i2, 1, &o2, 1) == 0) b->cpu = o2; }
            if (b->cpu) {
                const uint32_t *j = (const uint32_t *)(uintptr_t)(b->cpu + (a->jc - b->va));
                uint64_t fault = (uint64_t)j[2] | (uint64_t)j[3] << 32;
                const char *where = "no BO";
                for (uint32_t k = 1; k <= XC5_MAX_BOS; k++)
                    if (c->bos[k].used && fault >= c->bos[k].va && fault < c->bos[k].va + c->bos[k].size) {
                        where = c->bos[k].imported ? "an imported surface" : (c->bos[k].flags & VGPU_BO_HEAP) ? "the heap" : "a BO";
                        break;
                    }
                xlog(c, "viva:   job status 0x%x incomplete %u fault_ptr 0x%llx (in %s) type %u", j[0], j[1],
                     (unsigned long long)fault, where, (j[4] >> 1) & 0x7f);
            }
            break;
        }
    }
    // A faulting chain is reported, not fatal (as herodrm): its fence still signals so nobody waits forever.
    a->seq = (uint32_t)++c->seq;
    if (c->completion) {
        c->completion->completed64 = c->completion->submitted64 = c->seq;
        c->completion->submitted = c->completion->completed = (uint32_t)c->seq;
    }
    if (a->out_sync) {
        c->syncobj[a->out_sync] = 2;
        pthread_cond_broadcast(&c->cond);
    }
    return kr ? -EIO : 0;
}

static int import_surface(XConn *c, struct vgpu_import_iosurface *a)
{
    uint64_t in = a->surface_id, out[9] = { 0 };
    kern_return_t kr = xcall(c, XSEL_SURFACE_BO, &in, 1, out, 9);
    if (kr || !out[0] || out[0] > XC5_MAX_BOS) return kr ? xerr(kr) : -ENOENT;
    uint32_t h = (uint32_t)out[0], planes = (uint32_t)out[8];
    // The kernel maps the whole surface from its first byte; only plane 0 (offset 0, what CoreAnimation's surfaces
    // use) can be given as the BO's first byte with the CPU mapping at its page.
    uint64_t poff = out[7];
    if (a->plane != 0 || poff >= XC5_PAGE) {
        uint64_t f = h;
        xcall(c, XSEL_FREE, &f, 1, NULL, 0);
        return -ENOTSUP;
    }
    (void)planes;
    c->bos[h] = (XBO){ .va = out[1], .size = out[2], .gpu_offset = poff, .madv = VGPU_MADV_WILLNEED,
                       .imported = true, .used = true };
    a->handle = h;
    a->offset = out[1] + poff;
    a->size = out[2] - poff;
    a->bytes_per_row = (uint32_t)out[3];
    a->width = (uint32_t)out[4];
    a->height = (uint32_t)out[5];
    a->pixel_format = (uint32_t)out[6];
    return 0;
}

static int xc5_ioctl(void *conn, enum vgpu_selector sel, void *arg, size_t size)
{
    XConn *c = conn;
    (void)size;
    int ret = 0;
    pthread_mutex_lock(&c->lock);
    switch (sel) {
    case VGPU_SEL_GET_VERSION: {
        struct vgpu_version *v = arg;
        uint64_t in = 0, id = 0;
        xcall(c, XSEL_PARAM, &in, 1, &id, 1);   // GPU_PROD_ID
        v->major = VGPU_UAPI_MAJOR;
        v->minor = 2;                           // completion page and SUBMIT_BATCH (both answered here)
        v->gpu_id = (uint32_t)(id << 16);
        v->page_size = XC5_PAGE;
        snprintf(v->name, sizeof v->name, "Mali-G52 MP1 (XCover5)");
        break;
    }
    case VGPU_SEL_GET_PARAM: ret = get_param(c, arg); break;
    case VGPU_SEL_CREATE_BO: {
        struct vgpu_create_bo *a = arg;
        // viva1: a fragment chain faulted with 0x5a (ADDR_RANGE_FAULT). XC5GPU's BOs never grow on a fault, so a HEAP
        // BO (the tiler heap, Mesa asks for 128 MiB) is given whole, capped as herodrm did (VIVA_XC5_HEAP_MB, 32).
        uint64_t sz = a->size;
        if (a->flags & VGPU_BO_HEAP) {
            const char *e = getenv("VIVA_XC5_HEAP_MB");
            // viva7: backboardd (the full-screen compositor) faulted ~15% of its chains with 0x5a/0x5b on 32 MiB: 96.
            int def = !strcmp(getprogname(), "backboardd") ? 96 : 32;
            uint64_t cap = (uint64_t)(e ? atoi(e) : def) << 20;
            if (cap && sz > cap) sz = cap;
        }
        uint64_t in = sz, out[3] = { 0, 0, 0 };
        kern_return_t kr = xcall(c, XSEL_CREATE, &in, 1, out, 3);
        if (kr || !out[0] || out[0] > XC5_MAX_BOS) {
            xlog(c, "viva: CREATE_BO %u bytes (flags 0x%x) failed: 0x%x", a->size, a->flags, kr);
            ret = -ENOMEM;
            break;
        }
        c->bos[out[0]] = (XBO){ .va = out[1], .size = out[2], .flags = a->flags, .madv = VGPU_MADV_WILLNEED, .used = true };
        a->handle = (uint32_t)out[0];
        a->offset = out[1];
        if ((a->flags & VGPU_BO_HEAP) && c->faults < 4)
            xlog(c, "viva: heap BO %u: %llu bytes at 0x%llx (asked %u)", a->handle, (unsigned long long)out[2],
                 (unsigned long long)out[1], a->size);
        break;
    }
    case VGPU_SEL_MMAP_BO: {
        struct vgpu_mmap_bo *a = arg;
        if (!xbo(c, a->handle)) { ret = -ENOENT; break; }
        a->offset = a->handle;
        break;
    }
    case VGPU_SEL_GET_BO_OFFSET: {
        struct vgpu_get_bo_offset *a = arg;
        XBO *b = xbo(c, a->handle);
        if (!b) { ret = -ENOENT; break; }
        a->offset = b->va + b->gpu_offset;
        break;
    }
    case VGPU_SEL_GEM_CLOSE: {
        struct vgpu_gem_close *a = arg;
        if (!xbo(c, a->handle)) { ret = -ENOENT; break; }
        uint64_t in = a->handle;
        xcall(c, XSEL_FREE, &in, 1, NULL, 0);
        c->bos[a->handle] = (XBO){ 0 };
        break;
    }
    case VGPU_SEL_SUBMIT: ret = xc5_submit(c, arg); break;
    case VGPU_SEL_SUBMIT_BATCH: {
        struct vgpu_submit_batch *a = arg;
        a->done = 0;
        if (a->count > 64 || (a->count && (a->stride != sizeof(struct vgpu_submit) || !a->submits))) { ret = -EINVAL; break; }
        struct vgpu_submit *subs = (struct vgpu_submit *)(uintptr_t)a->submits;
        for (uint32_t i = 0; i < a->count && !ret; i++) if (!(ret = xc5_submit(c, &subs[i]))) a->done++;
        break;
    }
    case VGPU_SEL_SURFACE_PROMISE: {
        struct vgpu_surface_promise *a = arg;
        if (!a->count || a->count > 16 || !a->syncobj || a->syncobj >= XC5_MAX_SYNC || !c->syncobj[a->syncobj]) { ret = -EINVAL; break; }
        c->syncobj[a->syncobj] = 1;
        break;
    }
    case VGPU_SEL_WAIT_BO: {
        struct vgpu_wait_bo *a = arg;
        if (!xbo(c, a->handle)) ret = -ENOENT;   // submits are synchronous: every BO is idle
        break;
    }
    case VGPU_SEL_MADVISE: {
        struct vgpu_madvise *a = arg;
        XBO *b = xbo(c, a->handle);
        if (!b) { ret = -ENOENT; break; }
        b->madv = a->madv;
        a->retained = 1;
        break;
    }
    case VGPU_SEL_SYNCOBJ_CREATE: {
        struct vgpu_syncobj_create *a = arg;
        uint32_t h = c->nextSync;
        while (h < XC5_MAX_SYNC && c->syncobj[h]) h++;
        if (h >= XC5_MAX_SYNC) { ret = -ENOMEM; break; }
        c->syncobj[h] = (a->flags & VGPU_SYNCOBJ_CREATE_SIGNALED) ? 2 : 1;
        c->nextSync = h + 1;
        a->handle = h;
        break;
    }
    case VGPU_SEL_SYNCOBJ_DESTROY: {
        struct vgpu_syncobj_destroy *a = arg;
        if (!a->handle || a->handle >= XC5_MAX_SYNC || !c->syncobj[a->handle]) { ret = -EINVAL; break; }
        c->syncobj[a->handle] = 0;
        if (a->handle < c->nextSync) c->nextSync = a->handle;
        break;
    }
    case VGPU_SEL_SYNCOBJ_WAIT: ret = wait_syncobjs(c, arg); break;
    case VGPU_SEL_SYNCOBJ_RESET:
    case VGPU_SEL_SYNCOBJ_SIGNAL: {
        struct vgpu_syncobj_array *a = arg;
        const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
        for (uint32_t i = 0; i < a->count_handles; i++)
            if (!h[i] || h[i] >= XC5_MAX_SYNC || !c->syncobj[h[i]]) { ret = -EINVAL; break; }
        if (ret) break;
        for (uint32_t i = 0; i < a->count_handles; i++) c->syncobj[h[i]] = sel == VGPU_SEL_SYNCOBJ_SIGNAL ? 2 : 1;
        pthread_cond_broadcast(&c->cond);
        break;
    }
    case VGPU_SEL_SYNCOBJ_TRANSFER: {
        struct vgpu_syncobj_transfer *a = arg;
        if (!a->src || a->src >= XC5_MAX_SYNC || !c->syncobj[a->src] || !a->dst || a->dst >= XC5_MAX_SYNC || !c->syncobj[a->dst]) {
            ret = -EINVAL; break;
        }
        c->syncobj[a->dst] = c->syncobj[a->src];
        pthread_cond_broadcast(&c->cond);
        break;
    }
    case VGPU_SEL_IMPORT_IOSURFACE: ret = import_surface(c, arg); break;
    case VGPU_SEL_SYNC_BO: {
        // GPU writes into surfaces become visible to the CPU and the display (the kernel cleans a surface's lines).
        struct vgpu_sync_bo *a = arg;
        const struct vgpu_bo_sync_op *ops = (const struct vgpu_bo_sync_op *)(uintptr_t)a->ops;
        for (uint32_t i = 0; i < a->op_count; i++) {
            XBO *b = xbo(c, ops[i].handle);
            if (b && b->imported) { uint64_t in = ops[i].handle; xcall(c, XSEL_SYNC_BO, &in, 1, NULL, 0); }
        }
        break;
    }
    case VGPU_SEL_QUERY_BO_INFO: {
        struct vgpu_query_bo_info *a = arg;
        XBO *b = xbo(c, a->handle);
        if (!b) { ret = -ENOENT; break; }
        a->create_flags = b->flags;
        a->extra_flags = b->imported ? VGPU_BO_IS_IMPORTED : 0;
        break;
    }
    case VGPU_SEL_IMPORT_USERPTR: ret = -ENOTSUP; break;   // no such selector in XC5GPU: the driver copies instead
    case VGPU_SEL_JM_CTX_CREATE: ((struct vgpu_jm_ctx *)arg)->handle = 1; break;
    case VGPU_SEL_JM_CTX_DESTROY: break;
    default: ret = -EINVAL;
    }
    pthread_mutex_unlock(&c->lock);
    return ret;
}

static void *xc5_mmap(void *conn, uint64_t token, size_t size)
{
    XConn *c = conn;
    if (token == VGPU_MAP_COMPLETION) {
        if (!c->completion || size > (size_t)getpagesize()) { errno = EINVAL; return NULL; }
        return c->completion;
    }
    void *p = NULL;
    pthread_mutex_lock(&c->lock);
    XBO *b = xbo(c, (uint32_t)token);
    if (b && size <= b->size) {
        if (!b->cpu) {
            uint64_t in = token, out = 0;
            if (xcall(c, XSEL_MAP, &in, 1, &out, 1) == kIOReturnSuccess) b->cpu = out;
        }
        p = b->cpu ? (void *)(uintptr_t)b->cpu : NULL;
    }
    pthread_mutex_unlock(&c->lock);
    if (!p) errno = EINVAL;
    return p;
}

// The kernel keeps one mapping per BO until the BO is freed.
static int xc5_munmap(void *conn, void *addr, size_t size) { (void)conn; (void)addr; (void)size; return 0; }

const struct vgpu_backend vgpu_backend_xc5 = { "xc5", xc5_open, xc5_close, xc5_ioctl, xc5_mmap, xc5_munmap };
