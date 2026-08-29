/* present.c -- libmpv (software render) -> double-buffered KMS present loop.
 *
 * "Flat now, distortion later": the frame SOURCE (libmpv producing one eye
 * image) and the PRESENTER (blit + duplicate + flip) are separated. Per-eye
 * lens distortion later replaces only present_frame() -- render the eye image
 * as a texture through a distortion mesh via GBM/EGL -- the source is untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include "present.h"

struct fbuf { uint32_t handle, pitch, fb_id; uint64_t size; uint8_t *map; };

static int make_fb(int fd, int W, int H, struct fbuf *b) {
    uint64_t offset = 0;
    memset(b, 0, sizeof *b);
    if (drmModeCreateDumbBuffer(fd, W, H, 32, 0, &b->handle, &b->pitch, &b->size)) return -1;
    if (drmModeAddFB(fd, W, H, 24, 32, b->pitch, b->handle, &b->fb_id)) return -1;
    if (drmModeMapDumbBuffer(fd, b->handle, &offset)) return -1;
    b->map = mmap(0, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (b->map == MAP_FAILED) return -1;
    memset(b->map, 0, b->size);
    return 0;
}
static void free_fb(int fd, struct fbuf *b) {
    if (b->map && b->map != MAP_FAILED) munmap(b->map, b->size);
    if (b->fb_id) drmModeRmFB(fd, b->fb_id);
    if (b->handle) drmModeDestroyDumbBuffer(fd, b->handle);
}

/* --- THE SEAM: today a straight duplicate (flat mono, no lens correction).
 * Later, replace with a per-eye distortion pass. --------------------------- */
static void present_frame(struct fbuf *b, int W, int H,
                          const uint8_t *eye, size_t eye_stride) {
    int half = W / 2;               /* eye width */
    size_t bytes = (size_t)half * 4;
    for (int y = 0; y < H; y++) {
        const uint8_t *src = eye + (size_t)y * eye_stride;
        uint8_t *dstL = b->map + (size_t)y * b->pitch;
        memcpy(dstL, src, bytes);          /* left eye  */
        memcpy(dstL + bytes, src, bytes);  /* right eye */
    }
}

static int g_flip_pending = 0;
static void on_flip(int fd, unsigned seq, unsigned s, unsigned us, void *u) {
    (void)fd;(void)seq;(void)s;(void)us;(void)u; g_flip_pending = 0;
}
static volatile int g_mpv_redraw = 1;
static void on_mpv_update(void *ctx) { (void)ctx; g_mpv_redraw = 1; }

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static void die_mpv(const char *what, int err) {
    fprintf(stderr, "ERROR: %s: %s\n", what, mpv_error_string(err));
    exit(6);
}

int play_video(int drmfd, const char *target, const char *video) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    drmModeRes *dr = drmModeGetResources(drmfd);
    if (!dr || dr->count_connectors < 1 || dr->count_crtcs < 1) {
        fprintf(stderr, "ERROR: leased fd exposes no usable connector/crtc\n"); return 3;
    }
    uint32_t conn_id = dr->connectors[0];
    uint32_t crtc_id = dr->crtcs[0];
    drmModeConnector *c = drmModeGetConnector(drmfd, conn_id);
    if (!c || c->count_modes < 1) {
        fprintf(stderr, "ERROR: connector has no modes (panel asleep? wear the headset)\n"); return 3;
    }
    /* largest-area mode, highest refresh on ties (native 2880x1440@90) */
    int best = 0;
    for (int m = 1; m < c->count_modes; m++) {
        long a  = (long)c->modes[m].hdisplay    * c->modes[m].vdisplay;
        long ab = (long)c->modes[best].hdisplay * c->modes[best].vdisplay;
        if (a > ab || (a == ab && c->modes[m].vrefresh > c->modes[best].vrefresh)) best = m;
    }
    drmModeModeInfo mode = c->modes[best];
    int W = mode.hdisplay, H = mode.vdisplay, half = W / 2;
    printf("panel mode: %dx%d @ %dHz  (eye %dx%d)\n", W, H, mode.vrefresh, half, H);

    struct fbuf fb[2];
    if (make_fb(drmfd, W, H, &fb[0]) || make_fb(drmfd, W, H, &fb[1])) {
        fprintf(stderr, "ERROR: framebuffer allocation failed\n"); return 4;
    }
    size_t eye_stride = (size_t)half * 4;
    uint8_t *eye = malloc(eye_stride * H);
    if (!eye) { fprintf(stderr, "ERROR: eye buffer alloc\n"); return 4; }

    mpv_handle *mpv = mpv_create();
    if (!mpv) { fprintf(stderr, "ERROR: mpv_create\n"); return 6; }
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "no");
    mpv_set_option_string(mpv, "keep-open", "no");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", "all=warn");
    int e;
    if ((e = mpv_initialize(mpv)) < 0) die_mpv("mpv_initialize", e);

    mpv_render_context *rc = NULL;
    int adv = 0;
    mpv_render_param cparams[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &adv },
        { 0 },
    };
    if ((e = mpv_render_context_create(&rc, mpv, cparams)) < 0) die_mpv("render_context_create", e);
    mpv_render_context_set_update_callback(rc, on_mpv_update, NULL);

    const char *cmd[] = { "loadfile", video, NULL };
    if ((e = mpv_command(mpv, cmd)) < 0) die_mpv("loadfile", e);

    if (drmModeSetCrtc(drmfd, crtc_id, fb[0].fb_id, 0, 0, &conn_id, 1, &mode)) {
        fprintf(stderr, "ERROR: initial SetCrtc failed: %s\n", strerror(errno)); return 5;
    }
    printf("SUCCESS: scanning out on %s. >>> LOOK IN THE HEADSET <<<\n", target);
    fflush(stdout);

    drmEventContext evctx = { .version = 2, .page_flip_handler = on_flip };
    int front = 0;
    struct pollfd pfd = { .fd = drmfd, .events = POLLIN };
    int done = 0;

    while (!done && !g_stop) {
        for (;;) {
            mpv_event *ev = mpv_wait_event(mpv, 0);
            if (ev->event_id == MPV_EVENT_NONE) break;
            if (ev->event_id == MPV_EVENT_END_FILE || ev->event_id == MPV_EVENT_SHUTDOWN) done = 1;
        }
        uint64_t upd = mpv_render_context_update(rc);
        if (!(upd & MPV_RENDER_UPDATE_FRAME) || g_flip_pending) {
            if (g_flip_pending) { if (poll(&pfd, 1, 100) > 0) drmHandleEvent(drmfd, &evctx); }
            else usleep(1000);
            continue;
        }
        int sw_size[2] = { half, H };
        size_t stride = eye_stride;
        mpv_render_param rp[] = {
            { MPV_RENDER_PARAM_SW_SIZE,    sw_size },
            { MPV_RENDER_PARAM_SW_FORMAT,  (void *)"bgr0" },  /* XRGB8888 in memory */
            { MPV_RENDER_PARAM_SW_STRIDE,  &stride },
            { MPV_RENDER_PARAM_SW_POINTER, eye },
            { 0 },
        };
        if ((e = mpv_render_context_render(rc, rp)) < 0) {
            fprintf(stderr, "render: %s\n", mpv_error_string(e)); break;
        }
        int back = front ^ 1;
        present_frame(&fb[back], W, H, eye, eye_stride);

        if (drmModePageFlip(drmfd, crtc_id, fb[back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
            g_flip_pending = 1;
            while (g_flip_pending && !g_stop) {
                if (poll(&pfd, 1, 100) > 0) drmHandleEvent(drmfd, &evctx);
                else break;
            }
        } else {
            drmModeSetCrtc(drmfd, crtc_id, fb[back].fb_id, 0, 0, &conn_id, 1, &mode);
        }
        front = back;
        mpv_render_context_report_swap(rc);
    }

    printf("playback finished (%s).\n", g_stop ? "interrupted" : "end of file");
    mpv_render_context_free(rc);
    mpv_destroy(mpv);
    free(eye);
    free_fb(drmfd, &fb[0]);
    free_fb(drmfd, &fb[1]);
    drmModeFreeConnector(c);
    drmModeFreeResources(dr);
    return 0;
}
