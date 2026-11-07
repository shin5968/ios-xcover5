// SPDX-License-Identifier: MIT
// XCover5: boot-arg xc5posterretry=1 (or 2), in backboardd: the wallpaper's second chance.
//
// After danstronics/ios-s9plus-exynos core/userland/src/poster_retry.c (HakiOS/Scion fork, Galaxy S9), which found the
// cause of what boots 71-72 show here: launchd gives an extension 5 s from its spawn to check in and SIGKILLs it
// otherwise ("exited with exit reason (namespace: 7 code: 0x3) - XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT"), and
// PosterBoard starts each poster once, in the middle of SpringBoard's start-up rush; on a slow phone every poster is
// killed at exactly 5 s and the lock and home screens have no wallpaper. A minute later the same extensions check in in
// well under a second. So: wait for the rush to pass (PosterBoard up for 60 s and the load average down), count the
// poster processes (two or more are PosterBoard's: SpringBoard starts only the current one for itself), and when
// PosterBoard's are not there, stop PosterBoard; launchd starts it again on SpringBoard's next request, and its posters
// then have the processors to themselves. At most three times. xc5posterretry=2 also restarts chronod (WidgetKit's
// host) once after the rush, so every widget asks its extension for a timeline again.
//
// Runs on a thread of backboardd (which our library is already in, and which starts before PosterBoard); reports
// through herodrm_log as the rest of this library does.
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PROC_ALL_PIDS   1
int proc_listpids(unsigned type, unsigned typeinfo, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, unsigned buffersize);

#define MAX_PROCS       2048
#define LOOK            5       /* seconds between looks */
#define SETTLE          60      /* PosterBoard up for this long before anything is judged */
#define QUIET           3.0     /* load average below which the rush is over (eight cores here; the S9 used 1.5 on two) */
#define QUIET_WAIT      180     /* ... but no longer than this is waited for it */
#define RECOVER         90      /* after a restart, how long the posters are given to come up */
#define TRIES           3
#define LIMIT           900

static void (*klog)(const char *fmt, ...);
static int pids[MAX_PROCS];
static int mode;

#define SAY(...) do { if (klog) klog(__VA_ARGS__); else { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int find(const char *name) {
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
    char here[256];
    for (int i = 0; bytes > 0 && i < bytes / (int)sizeof(int); i++)
        if (pids[i] > 0 && proc_name(pids[i], here, sizeof(here)) > 0 && strcmp(here, name) == 0) return pids[i];
    return 0;
}

static int posters(char *list, size_t size) {
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), count = 0;
    size_t used = 0;
    char here[256];

    list[0] = '\0';
    for (int i = 0; bytes > 0 && i < bytes / (int)sizeof(int); i++) {
        if (pids[i] <= 0 || proc_name(pids[i], here, sizeof(here)) <= 0 || strstr(here, "Poster") == NULL ||
            strcmp(here, "PosterBoard") == 0)
            continue;
        count++;
        if (used < size) used += (size_t)snprintf(list + used, size - used, " %s[%d]", here, pids[i]);
    }
    return count;
}

static double load(void) {
    double avg[3] = { 0, 0, 0 };
    return getloadavg(avg, 3) > 0 ? avg[0] : 0;
}

static void stop(int pid, const char *name) {
    if (kill(pid, SIGTERM) != 0) {
        SAY("xc5posterretry: %s[%d]: SIGTERM refused: %s", name, pid, strerror(errno));
        return;
    }
    for (int i = 0; i < 15; i++) {
        usleep(200000);
        if (kill(pid, 0) != 0 && errno == ESRCH) return;
    }
    SAY("xc5posterretry: %s[%d] did not leave on SIGTERM; SIGKILL", name, pid);
    kill(pid, SIGKILL);
}

static void *retry(void *arg) {
    char list[512];
    int board, count, tries = 0, chronod_done = 0, not_before = -1, quiet_by = -1;

    (void)arg;
    for (int waited = 0; waited < LIMIT; sleep(LOOK), waited += LOOK) {
        if ((board = find("PosterBoard")) == 0) continue;
        if (not_before < 0) {
            not_before = waited + SETTLE;
            quiet_by = not_before + QUIET_WAIT;
        }
        if (waited < not_before || (load() >= QUIET && waited < quiet_by)) continue;
        if (mode == 2 && !chronod_done) {
            int chronod = find("chronod");
            chronod_done = 1;
            if (chronod != 0) {
                SAY("xc5posterretry: chronod[%d] restarted, for the widgets' timelines", chronod);
                stop(chronod, "chronod");
            }
        }
        if ((count = posters(list, sizeof(list))) >= 2) {
            SAY("xc5posterretry: PosterBoard[%d]'s posters are up after %d s (load %.2f):%s%s", board, waited, load(),
                list, tries ? "" : "; nothing to do");
            return NULL;
        }
        if (tries == TRIES) {
            SAY("xc5posterretry: still no posters of PosterBoard's after %d restarts (%d:%s); giving up", TRIES, count,
                count ? list : " none");
            return NULL;
        }
        tries++;
        SAY("xc5posterretry: PosterBoard[%d] has none of its posters %d s in (load %.2f; %d:%s): killed at the 5 s "
            "check-in deadline; restarting it (%d of %d)", board, waited, load(), count, count ? list : " none", tries,
            TRIES);
        stop(board, "PosterBoard");
        not_before = waited + RECOVER;
        quiet_by = not_before + QUIET_WAIT / 3;
    }
    SAY("xc5posterretry: no PosterBoard within %d s; nothing done", LIMIT);
    return NULL;
}

__attribute__((constructor)) static void xc5_posterretry_init(void) {
    char args[1024] = "";
    size_t size = sizeof(args) - 1;
    const char *at;
    pthread_t t;

    if (strcmp(getprogname(), "backboardd") != 0) return;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (at = strstr(args, "xc5posterretry=")) == NULL)
        return;
    mode = atoi(at + strlen("xc5posterretry="));
    if (mode <= 0) return;
    klog = (void (*)(const char *, ...))dlsym(RTLD_DEFAULT, "herodrm_log");
    SAY("xc5posterretry: on (mode %d), watching for PosterBoard", mode);
    if (pthread_create(&t, NULL, retry, NULL) == 0) pthread_detach(t);
}
