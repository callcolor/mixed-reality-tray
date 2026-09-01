/* vrhead.c -- WMR gyro -> per-eye pixel offset for vrmirror. See vrhead.h.
 *
 * The WMR gen1 "Microsoft HoloLens Sensors" HID device does not stream until it
 * is told to: writing the control message {0x02,0x07} starts the sensor reports.
 * After that it emits report id 0x01 packets carrying, among other things, a
 * block of gyro samples. Byte layout of the sensors packet (little-endian),
 * confirmed against the hardware and matching OpenHMD/Monado:
 *
 *   off  0   uint8   id (0x01)
 *   off  1   uint16  temperature[4]        (8 bytes)
 *   off  9   uint64  gyro_timestamp[4]     (32 bytes)
 *   off 41   int16   gyro[3][32]           (192 bytes)  <- axis-major, 32 samples
 *   ...      (accel + video timestamps follow; unused here)
 *
 * We average the 32 samples per axis to get an angular-rate estimate, subtract
 * an auto-zeroed bias, integrate to a yaw/pitch angle (clamped to the visible
 * range so it holds at the edge instead of winding up), and map it to a pixel
 * offset. No calibration data, no fusion; recentre is optional and off by default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <dirent.h>

#include "vrhead.h"
#include "vrlog.h"

#define GYRO_OFF   41            /* byte offset of gyro[3][32] in the 0x01 packet */
#define GYRO_N     32            /* samples per axis */
#define PKT_MIN    (GYRO_OFF + 3*GYRO_N*2)   /* smallest packet we can parse */
#define BIAS_SECS  0.6           /* auto-zero the gyro bias over this long at rest */
#define BIAS_MINPK 8             /* ...and at least this many packets */

struct vr_head {
    int fd;
    /* bias auto-zero */
    int biased; double bias[3]; double bsum[3]; int bcount; double t_open;
    /* integrator */
    double t_last; double yaw_deg, pitch_deg;
    /* config (env-tunable) */
    int yaw_axis, pitch_axis; double yaw_sign, pitch_sign;
    double cpd;      /* raw gyro counts per degree/second */
    double gain;     /* pixels per degree of view angle */
    int maxx, maxy;  /* current per-axis reach (px), set live from the presenter */
    int margin;      /* look-around allowance added past the aspect crop, each axis */
    double cappx;    /* optional cap on reach (VRMIRROR_HEAD_MAX); 0 = uncapped */
    double tau;      /* optional recentre time constant (s); 0 = hold, no drift-back */
    int debug, dbg;
};

static double now_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static int16_t rd16(const unsigned char *p){ return (int16_t)(p[0] | (p[1] << 8)); }

static double envf(const char *k, double d){ const char *v = getenv(k); return v ? atof(v) : d; }
static int    envi(const char *k, int d){ const char *v = getenv(k); return v ? atoi(v) : d; }

/* Find the hidraw node whose HID_NAME contains "HoloLens Sensors". Returns 1 and
 * fills `out` on success. */
static int autodetect(char *out, size_t outsz){
    DIR *d = opendir("/sys/class/hidraw");
    if(!d) return 0;
    struct dirent *e; int found = 0;
    while(!found && (e = readdir(d))){
        if(strncmp(e->d_name, "hidraw", 6) != 0) continue;
        char path[sizeof "/sys/class/hidraw//device/uevent" + 256];
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(path, "r"); if(!f) continue;
        char line[512];
        while(fgets(line, sizeof line, f)){
            if(strncmp(line, "HID_NAME=", 9) == 0 && strstr(line, "HoloLens Sensors")){
                snprintf(out, outsz, "/dev/%s", e->d_name); found = 1; break;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

vr_head *vr_head_open(const char *path){
    const char *en = getenv("VRMIRROR_HEAD");
    if(en && (!strcmp(en,"0") || !strcmp(en,"off"))) return NULL;

    char autopath[sizeof "/dev/" + 256];
    if(!path){
        if(!autodetect(autopath, sizeof autopath)){
            vrlog("[head] no HoloLens Sensors hidraw found; head tracking off\n");
            return NULL;
        }
        path = autopath;
    }
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if(fd < 0){ vrlog("[head] open %s: %s\n", path, strerror(errno)); return NULL; }

    /* start the sensor stream (0x02 control msg, 0x07 = IMU on) */
    unsigned char cmd[64] = {0}; cmd[0] = 0x02; cmd[1] = 0x07;
    if(write(fd, cmd, sizeof cmd) < 0){
        vrlog("[head] enable write failed: %s\n", strerror(errno));
        close(fd); return NULL;
    }

    vr_head *h = calloc(1, sizeof *h);
    if(!h){ close(fd); return NULL; }
    h->fd = fd; h->t_open = h->t_last = now_s();
    /* Axis map measured on the Acer AH101 (gyro[0..2] = X,Y,Z):
     *   X = yaw   (left = -X, right = +X)
     *   Y = pitch (up   = -Y, down  = +Y)
     *   Z = roll  (unused)
     * Signs give a world-fixed view: turn right -> image shifts left so you look
     * "around" the fixed window; look up -> image shifts down to reveal the top.
     * Flip a sign if the window chases the wrong way once you can see the panel. */
    h->yaw_axis   = envi("VRMIRROR_HEAD_YAW_AXIS",   0);
    h->pitch_axis = envi("VRMIRROR_HEAD_PITCH_AXIS", 1);
    h->yaw_sign   = envf("VRMIRROR_HEAD_YAW_SIGN",   -1.0) < 0 ? -1.0 : 1.0;
    h->pitch_sign = envf("VRMIRROR_HEAD_PITCH_SIGN", -1.0) < 0 ? -1.0 : 1.0;
    h->cpd   = envf("VRMIRROR_HEAD_CPD",  16.4);   /* ~16.4 counts/(deg/s) for a +/-2000dps 16-bit gyro */
    h->gain  = envf("VRMIRROR_HEAD_GAIN", 40.0);   /* px per degree of view angle */
    h->cappx  = envf("VRMIRROR_HEAD_MAX", 0.0);    /* 0 = reach follows window aspect, uncapped */
    h->margin = (int)envf("VRMIRROR_HEAD_MARGIN", 0.0);  /* extra pan past edge-at-centre, each axis */
    h->tau   = envf("VRMIRROR_HEAD_RECENTER", 0.0);   /* 0 = hold (no drift-back) */
    h->debug = getenv("VRMIRROR_HEAD_DEBUG") ? 1 : 0;
    if(h->cpd < 1e-3) h->cpd = 16.4;
    if(h->tau < 0) h->tau = 0;

    vrlog("[head] tracking on (%s): yaw=axis%d*%+g pitch=axis%d*%+g "
                    "gain=%gpx/deg reach=edge→centre%+dpx%s recentre=%s\n",
            path, h->yaw_axis, h->yaw_sign, h->pitch_axis, h->pitch_sign,
            h->gain, h->margin, h->cappx>0 ? " (capped)" : "",
            h->tau>0 ? "on" : "off (hold)");
    return h;
}

void vr_head_close(vr_head *h){
    if(!h) return;
    if(h->fd >= 0) close(h->fd);
    free(h);
}

int vr_head_fd(const vr_head *h){ return h ? h->fd : -1; }

void vr_head_recenter(vr_head *h){ if(h){ h->yaw_deg = 0; h->pitch_deg = 0; } }

void vr_head_set_range(vr_head *h, int rx, int ry){
    if(!h) return;
    if(rx < 0) rx = 0;
    if(ry < 0) ry = 0;
    rx += h->margin; ry += h->margin;          /* look past the edge into a little black */
    if(h->cappx > 0){                          /* optional cap so ultrawide needn't crane the neck */
        if(rx > h->cappx) rx = (int)h->cappx;
        if(ry > h->cappx) ry = (int)h->cappx;
    }
    h->maxx = rx; h->maxy = ry;
}

/* Read one packet's per-axis gyro mean into rate[3]; returns 1 on a usable
 * sensor packet, 0 otherwise. */
static int packet_rate(const unsigned char *buf, int n, double rate[3]){
    if(n < PKT_MIN || buf[0] != 0x01) return 0;
    for(int a = 0; a < 3; a++){
        long s = 0; const unsigned char *g = buf + GYRO_OFF + a*GYRO_N*2;
        for(int j = 0; j < GYRO_N; j++) s += rd16(g + j*2);
        rate[a] = s / (double)GYRO_N;
    }
    return 1;
}

void vr_head_poll(vr_head *h){
    if(!h) return;
    unsigned char buf[1024];
    double sum[3] = {0,0,0}; int count = 0; double rate[3];

    for(;;){
        int n = read(h->fd, buf, sizeof buf);
        if(n <= 0) break;                       /* EAGAIN / no more packets */
        if(!packet_rate(buf, n, rate)) continue;
        for(int a = 0; a < 3; a++) sum[a] += rate[a];
        count++;
    }
    if(count == 0) return;

    double avg[3]; for(int a = 0; a < 3; a++) avg[a] = sum[a] / count;
    double t = now_s();

    if(!h->biased){
        for(int a = 0; a < 3; a++) h->bsum[a] += avg[a];
        h->bcount++;
        if(t - h->t_open >= BIAS_SECS && h->bcount >= BIAS_MINPK){
            for(int a = 0; a < 3; a++) h->bias[a] = h->bsum[a] / h->bcount;
            h->biased = 1;
            vrlog("[head] bias zeroed: %.1f %.1f %.1f\n",
                    h->bias[0], h->bias[1], h->bias[2]);
        }
        h->t_last = t;
        return;                                 /* no output until settled */
    }

    double dt = t - h->t_last; h->t_last = t;
    if(dt <= 0) dt = 1e-4;                          /* guard against stalls / clock jumps */
    if(dt > 0.2) dt = 0.2;

    double yaw_rate   = (avg[h->yaw_axis]   - h->bias[h->yaw_axis])   / h->cpd; /* deg/s */
    double pitch_rate = (avg[h->pitch_axis] - h->bias[h->pitch_axis]) / h->cpd;

    /* integrate head angle -- the view HOLDS where you look, no drift-back */
    h->yaw_deg   += h->yaw_sign   * yaw_rate   * dt;
    h->pitch_deg += h->pitch_sign * pitch_rate * dt;

    /* clamp each ANGLE to its axis's visible reach so it never winds up past the
     * edge: look further and the image just stays pinned at the edge (it "comes
     * with you"), and turning back tracks immediately. The reach is per-axis and
     * set live from the window aspect (vr_head_set_range), so a wide window pans
     * horizontally, a tall one vertically, updating as it is resized. */
    double limx = (h->gain > 1e-6) ? h->maxx / h->gain : 1e9;   /* max view angle, deg */
    double limy = (h->gain > 1e-6) ? h->maxy / h->gain : 1e9;
    if(h->yaw_deg   >  limx) h->yaw_deg   =  limx; else if(h->yaw_deg   < -limx) h->yaw_deg   = -limx;
    if(h->pitch_deg >  limy) h->pitch_deg =  limy; else if(h->pitch_deg < -limy) h->pitch_deg = -limy;

    /* optional slow recentre (VRMIRROR_HEAD_RECENTER seconds); off by default */
    if(h->tau > 0){ double decay = exp(-dt / h->tau); h->yaw_deg *= decay; h->pitch_deg *= decay; }

    if(h->debug && (++h->dbg % 30) == 0){
        int xo, yo; vr_head_offset(h, &xo, &yo);
        vrlog("[head] rate x=%.1f y=%.1f z=%.1f  yaw=%.1fdeg pitch=%.1fdeg  off=(%d,%d)\n",
                avg[0]-h->bias[0], avg[1]-h->bias[1], avg[2]-h->bias[2],
                h->yaw_deg, h->pitch_deg, xo, yo);
    }
}

void vr_head_offset(const vr_head *h, int *xoff, int *yoff){
    double x = 0, y = 0;
    if(h && h->biased){
        x = h->yaw_deg   * h->gain;
        y = h->pitch_deg * h->gain;
        if(x >  h->maxx) x =  h->maxx; else if(x < -h->maxx) x = -h->maxx;
        if(y >  h->maxy) y =  h->maxy; else if(y < -h->maxy) y = -h->maxy;
    }
    if(xoff) *xoff = (int)lround(x);
    if(yoff) *yoff = (int)lround(y);
}
