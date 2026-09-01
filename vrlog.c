/* vrlog.c -- see vrlog.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include "vrlog.h"

#ifndef VRMIRROR_VERSION
#define VRMIRROR_VERSION "unknown"
#endif

extern char **environ;

static FILE *g_lf = NULL;
static char  g_lpath[512] = "";

static void stamp(char *buf, size_t n){
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    char hms[16]; strftime(hms, sizeof hms, "%H:%M:%S", &tm);   /* fixed 8 chars */
    int ms = (int)((ts.tv_nsec / 1000000) % 1000);   /* bounded: keeps %03d exact */
    snprintf(buf, n, "%s.%03d", hms, ms);
}

/* first line of `file` matching "key=" , with surrounding quotes stripped */
static int read_kv(const char *file, const char *key, char *out, size_t n){
    FILE *f = fopen(file, "r");
    if(!f) return 0;
    size_t klen = strlen(key);
    char line[512]; int got = 0;
    while(!got && fgets(line, sizeof line, f)){
        if(strncmp(line, key, klen) || line[klen] != '=') continue;
        char *v = line + klen + 1;
        v[strcspn(v, "\n")] = 0;
        size_t vl = strlen(v);
        if(vl >= 2 && v[0] == '"' && v[vl-1] == '"'){ v[vl-1] = 0; v++; }
        snprintf(out, n, "%s", v);
        got = 1;
    }
    fclose(f);
    return got;
}

/* "card0: nvidia (10DE:1F91), card1: i915 (8086:8A52)" */
static void gpu_summary(char *out, size_t n){
    out[0] = 0;
    DIR *d = opendir("/sys/class/drm");
    if(!d){ snprintf(out, n, "(unreadable)"); return; }
    struct dirent *e;
    while((e = readdir(d))){
        /* cardN only -- the cardN-CONNECTOR subdirs have no driver of their own */
        if(strncmp(e->d_name, "card", 4) || !e->d_name[4]) continue;
        if(strspn(e->d_name + 4, "0123456789") != strlen(e->d_name + 4)) continue;
        char path[300], drv[64] = "?", pci[64] = "";
        snprintf(path, sizeof path, "/sys/class/drm/%s/device/uevent", e->d_name);
        read_kv(path, "DRIVER", drv, sizeof drv);
        read_kv(path, "PCI_ID", pci, sizeof pci);
        size_t used = strlen(out);
        snprintf(out + used, n - used, "%s%s: %s%s%s%s",
                 used ? ", " : "", e->d_name, drv,
                 pci[0] ? " (" : "", pci, pci[0] ? ")" : "");
    }
    closedir(d);
    if(!out[0]) snprintf(out, n, "(none found)");
}

static void write_header(const char *app){
    char os[192] = "?", ses[128], gpus[512], envs[1024] = "";
    struct utsname u;
    if(uname(&u) != 0) memset(&u, 0, sizeof u);
    read_kv("/etc/os-release", "PRETTY_NAME", os, sizeof os);
    const char *st = getenv("XDG_SESSION_TYPE"), *de = getenv("XDG_CURRENT_DESKTOP");
    snprintf(ses, sizeof ses, "%s / %s", st ? st : "?", de ? de : "?");
    gpu_summary(gpus, sizeof gpus);
    /* only our own knobs -- never dump the whole environment into a file the
     * user is about to mail us */
    for(char **e = environ; *e; e++){
        if(strncmp(*e, "VRMIRROR_", 9)) continue;
        size_t used = strlen(envs);
        snprintf(envs + used, sizeof envs - used, "%s%s", used ? " " : "", *e);
    }

    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char date[64]; strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S %z", &tm);

    vrlog("==== %s %s ====", app, VRMIRROR_VERSION);
    vrlog("started: %s", date);
    vrlog("built:   %s %s", __DATE__, __TIME__);
    vrlog("system:  %s / %s %s", os, u.sysname[0] ? u.sysname : "?", u.release);
    vrlog("session: %s", ses);
    vrlog("gpu:     %s", gpus);
    vrlog("env:     %s", envs[0] ? envs : "(no VRMIRROR_* overrides)");
    vrlog("log:     %s", g_lpath[0] ? g_lpath : "(stderr only)");
    vrlog("----");
}

void vrlog_open(const char *app){
    if(g_lf) return;

    char dir[400];
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    if(xdg && xdg[0] == '/')      snprintf(dir, sizeof dir, "%s/vrmirror", xdg);
    else if(home && home[0])      snprintf(dir, sizeof dir, "%s/.local/state/vrmirror", home);
    else                          dir[0] = 0;

    if(dir[0]){
        /* mkdir -p over the two levels XDG may be missing */
        for(char *p = dir + 1; *p; p++){
            if(*p != '/') continue;
            *p = 0; mkdir(dir, 0700); *p = '/';
        }
        if(mkdir(dir, 0700) == 0 || errno == EEXIST){
            snprintf(g_lpath, sizeof g_lpath, "%s/%s.log", dir, app);
            char prev[540];
            snprintf(prev, sizeof prev, "%s.1", g_lpath);
            rename(g_lpath, prev);              /* keep exactly one previous run */
            g_lf = fopen(g_lpath, "w");
        }
    }
    if(!g_lf){
        g_lpath[0] = 0;
        fprintf(stderr, "[log] no log file (falling back to stderr only)\n");
    }
    write_header(app);
}

void vrlog(const char *fmt, ...){
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    size_t l = strlen(msg);                     /* callers vary; normalise to one \n */
    while(l && (msg[l-1] == '\n' || msg[l-1] == '\r')) msg[--l] = 0;

    char ts[32]; stamp(ts, sizeof ts);
    fprintf(stderr, "[%s] %s\n", ts, msg);
    fflush(stderr);
    if(g_lf){ fprintf(g_lf, "[%s] %s\n", ts, msg); fflush(g_lf); }
}

const char *vrlog_path(void){ return g_lpath[0] ? g_lpath : NULL; }

void vrlog_close(void){
    if(!g_lf) return;
    fclose(g_lf);
    g_lf = NULL;
}
