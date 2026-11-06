// SPDX-License-Identifier: MIT
// XCover5: boot-arg xc5pbreset=1, in backboardd (which starts before PosterBoard): move PosterBoard's saved
// poster state aside once, so its data store starts empty.
//
// Boots 56-60: PosterBoard dies with SIGTRAP on "Invalid condition not satisfying: !_stateLock_switcherConfiguration"
// (PBFPosterExtensionDataStore.m:3431) in its first-time switcher setup: a switcher configuration is already
// there. Our guess is a half-made state saved while app extensions were refused (before the AMFI work); a fresh
// simulator is what cleared the same first-time-setup crash in Xcode 14 betas (developer.apple.com/forums/
// thread/711121). The state lives in PosterBoard's data container under Library/Application Support
// (PBPosterExtensionDataSource: SelectedConfigurationIdentifier.plist, SwitcherConfigurationOrdering.plist,
// Extensions/, per pookjw.github.io's iOS 16 PosterBoard notes).
//
// The container is found by its metadata plist naming com.apple.PosterBoard. Its "Application Support" becomes
// "Application Support.xc5old"; when that already exists nothing is done, so it happens once per container
// without a marker file. Group containers that name PosterBoard are only logged.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define APP_DATA    "/private/var/mobile/Containers/Data/Application"
#define APP_GROUPS  "/private/var/mobile/Containers/Shared/AppGroup"
#define METADATA    ".com.apple.mobile_container_manager.metadata.plist"

static void (*klog)(const char *fmt, ...);

static void say(const char *fmt, const char *a, const char *b) {
    char line[400];
    snprintf(line, sizeof line, fmt, a, b);
    if (klog) klog("xc5pbreset: %s", line);
    else fprintf(stderr, "xc5pbreset: %s\n", line);
}

// Does the file hold the bytes of `needle` (binary plists keep ASCII strings as they are)?
static int file_has(const char *path, const char *needle) {
    char buf[8192];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    size_t len = strlen(needle);
    for (ssize_t i = 0; i + (ssize_t)len <= n; i++)
        if (memcmp(buf + i, needle, len) == 0) return 1;
    return 0;
}

// Boot 62: the container had no Library/Application Support at all, so there was no saved state to move.
// The assertion sits at the entry of -_stateLock_setupSwitcherConfigurationForFirstTime (self+152, the
// switcher configuration, already set): in one run the first-time setup runs twice, which fits a store that
// is never written. So: show what the container holds, and make the missing directories (as backboardd's
// user, mobile, like PosterBoard), then check that a file can be written there.
static void list_tree(const char *dir, int depth, int *lines) {
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024], line[64];
    struct stat st;

    if (d == NULL) {
        say("  %s: cannot open (%s)", dir, strerror(errno));
        return;
    }
    while ((e = readdir(d)) != NULL && *lines < 40) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (lstat(path, &st) != 0) continue;
        snprintf(line, sizeof line, "%s uid %u mode %o size %lld", S_ISDIR(st.st_mode) ? "dir" : "file",
            st.st_uid, st.st_mode & 07777, (long long)st.st_size);
        say("  %s: %s", path + strlen(APP_DATA) + 1, line);
        (*lines)++;
        if (S_ISDIR(st.st_mode) && depth > 0) list_tree(path, depth - 1, lines);
    }
    closedir(d);
}

static void make_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) == 0) say("made %s%s", path + strlen(APP_DATA) + 1, "");
    else say("mkdir %s failed: %s", path + strlen(APP_DATA) + 1, strerror(errno));
}

static void prepare(const char *container) {
    char path[1024];
    int lines = 0, fd;

    say("container %s holds:%s", container + strlen(APP_DATA) + 1, "");
    list_tree(container, 2, &lines);
    snprintf(path, sizeof path, "%s/Library", container);
    make_dir(path);
    snprintf(path, sizeof path, "%s/Library/Application Support", container);
    make_dir(path);
    snprintf(path, sizeof path, "%s/Library/Application Support/.xc5-write-test", container);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        say("write test in Application Support failed: %s%s", strerror(errno), "");
    } else {
        close(fd);
        unlink(path);
        say("write test in Application Support ok%s%s", "", "");
    }
}

static int scan(const char *root, int move) {
    DIR *d = opendir(root);
    struct dirent *e;
    int found = 0;
    char path[1024], from[1024], to[1024];
    struct stat st;

    if (d == NULL) {
        say("cannot open %s (%s)", root, strerror(errno));
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/%s", root, e->d_name, METADATA);
        if (!file_has(path, "com.apple.PosterBoard")) continue;
        found++;
        if (!move) {
            say("group container %s names PosterBoard (left alone)", e->d_name, "");
            continue;
        }
        snprintf(from, sizeof from, "%s/%s/Library/Application Support", root, e->d_name);
        snprintf(to, sizeof to, "%s.xc5old", from);
        if (stat(to, &st) == 0) {
            say("container %s: already reset once (%s exists), nothing done", e->d_name, "Application Support.xc5old");
            snprintf(path, sizeof path, "%s/%s", root, e->d_name);
            prepare(path);
        } else if (stat(from, &st) != 0) {
            say("container %s: no Application Support (%s)", e->d_name, strerror(errno));
            snprintf(path, sizeof path, "%s/%s", root, e->d_name);
            prepare(path);
        } else if (rename(from, to) != 0) {
            say("container %s: rename failed: %s", e->d_name, strerror(errno));
        } else {
            say("container %s: Application Support moved to %s; PosterBoard starts fresh", e->d_name,
                "Application Support.xc5old");
        }
    }
    closedir(d);
    return found;
}

__attribute__((constructor)) static void xc5_pbreset(void) {
    char args[1024] = "";
    size_t size = sizeof args - 1;

    if (strcmp(getprogname(), "backboardd") != 0) return;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || strstr(args, "xc5pbreset=1") == NULL) return;
    klog = (void (*)(const char *, ...))dlsym(RTLD_DEFAULT, "herodrm_log");
    say("looking for PosterBoard's data container (uid %s)%s", getuid() == 0 ? "0" : "not 0", "");
    if (scan(APP_DATA, 1) == 0) say("no data container names com.apple.PosterBoard%s%s", "", "");
    scan(APP_GROUPS, 0);
}
