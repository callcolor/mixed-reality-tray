/* vrplayer.c
 * Flat VR video player for the Acer AH101 WMR headset on Linux/Wayland.
 *
 * Pipeline:
 *   [libmpv, software render] --frame--> [KMS presenter on a leased connector]
 *
 * The headset is not a desktop display; we lease its non-desktop connector from
 * mutter via wp_drm_lease_device_v1 (yielding a master-capable DRM fd), exactly
 * as vr-lease-wl.c does, then run our own KMS present loop on it. libmpv decodes
 * and renders each frame in software into a temp "eye" buffer sized to one half
 * of the panel; we duplicate that into the left and right eye-halves of a
 * double-buffered dumb framebuffer and page-flip at vblank.
 *
 * "Flat now, distortion later": the frame SOURCE (libmpv producing one eye image)
 * and the PRESENTER (blit + duplicate + flip) are separated. Per-eye lens
 * distortion later replaces only the presenter (render the eye image as a texture
 * through a distortion mesh via GBM/EGL) -- the source is untouched. The single
 * seam to change is present_frame() below.
 *
 * build:
 *   wayland-scanner client-header .../drm-lease-v1.xml drm-lease-v1-client-protocol.h
 *   wayland-scanner private-code  .../drm-lease-v1.xml drm-lease-v1-protocol.c
 *   gcc vrplayer.c drm-lease-v1-protocol.c -o vrplayer \
 *     $(pkg-config --cflags --libs mpv wayland-client libdrm)
 * run:
 *   XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
 *     ./vrplayer /path/to/video.mp4 [CONNECTOR_NAME]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <wayland-client.h>
#include "drm-lease-v1-client-protocol.h"
#include "present.h"

/* ================================================================== */
/* Wayland lease front-end (verbatim from vr-lease-wl.c): yields a     */
/* master-capable DRM fd for one non-desktop connector.                */
/* ================================================================== */
struct conn_info {
    struct wp_drm_lease_connector_v1 *proxy;
    struct wp_drm_lease_device_v1 *dev;
    char name[128];
    uint32_t connector_id;
    struct conn_info *next;
};
static struct conn_info *g_conns = NULL;

static void c_name(void *data, struct wp_drm_lease_connector_v1 *p, const char *name) {
    struct conn_info *ci = data; (void)p;
    snprintf(ci->name, sizeof ci->name, "%s", name);
}
static void c_desc(void *d, struct wp_drm_lease_connector_v1 *p, const char *s) { (void)d;(void)p;(void)s; }
static void c_id(void *data, struct wp_drm_lease_connector_v1 *p, uint32_t id) {
    struct conn_info *ci = data; (void)p; ci->connector_id = id;
}
static void c_withdrawn(void *data, struct wp_drm_lease_connector_v1 *p) {
    struct conn_info *ci = data; (void)p; ci->name[0] = 0;
}
static void c_done(void *d, struct wp_drm_lease_connector_v1 *p) { (void)d;(void)p; }
static const struct wp_drm_lease_connector_v1_listener conn_listener = {
    .name = c_name, .description = c_desc, .connector_id = c_id,
    .withdrawn = c_withdrawn, .done = c_done,
};

static void d_drm_fd(void *d, struct wp_drm_lease_device_v1 *p, int fd) {
    (void)d; (void)p; close(fd);
}
static void d_connector(void *data, struct wp_drm_lease_device_v1 *dev,
                        struct wp_drm_lease_connector_v1 *conn) {
    (void)data;
    struct conn_info *ci = calloc(1, sizeof *ci);
    ci->proxy = conn; ci->dev = dev;
    ci->next = g_conns; g_conns = ci;
    wp_drm_lease_connector_v1_add_listener(conn, &conn_listener, ci);
}
static void d_released(void *d, struct wp_drm_lease_device_v1 *p) { (void)d;(void)p; }
static void d_done(void *d, struct wp_drm_lease_device_v1 *p) { (void)d;(void)p; }
static const struct wp_drm_lease_device_v1_listener dev_listener = {
    .drm_fd = d_drm_fd, .connector = d_connector,
    .released = d_released, .done = d_done,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)data; (void)version;
    if (strcmp(iface, wp_drm_lease_device_v1_interface.name) == 0) {
        struct wp_drm_lease_device_v1 *dev =
            wl_registry_bind(reg, name, &wp_drm_lease_device_v1_interface, 1);
        wp_drm_lease_device_v1_add_listener(dev, &dev_listener, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_remove,
};

static int g_lease_fd = -1;
static int g_lease_finished = 0;
static void l_lease_fd(void *d, struct wp_drm_lease_v1 *p, int fd) { (void)d;(void)p; g_lease_fd = fd; }
static void l_finished(void *d, struct wp_drm_lease_v1 *p) { (void)d;(void)p; g_lease_finished = 1; }
static const struct wp_drm_lease_v1_listener lease_listener = {
    .lease_fd = l_lease_fd, .finished = l_finished,
};

/* KMS presenter + libmpv source live in present.c. */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <video> [connector=DP-7]\n", argv[0]);
        return 1;
    }
    const char *video  = argv[1];
    const char *target = (argc > 2) ? argv[2] : "DP-7";

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "ERROR: cannot connect to Wayland (WAYLAND_DISPLAY?)\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    for (struct conn_info *ci = g_conns; ci; ci = ci->next) {
        if (strcmp(ci->name, target) != 0) continue;
        printf("requesting lease of '%s' (connector_id=%u)...\n", ci->name, ci->connector_id);
        struct wp_drm_lease_request_v1 *req = wp_drm_lease_device_v1_create_lease_request(ci->dev);
        wp_drm_lease_request_v1_request_connector(req, ci->proxy);
        struct wp_drm_lease_v1 *lease = wp_drm_lease_request_v1_submit(req);
        wp_drm_lease_v1_add_listener(lease, &lease_listener, NULL);

        g_lease_fd = -1; g_lease_finished = 0;
        while (g_lease_fd < 0 && !g_lease_finished)
            if (wl_display_roundtrip(dpy) < 0) break;

        if (g_lease_fd >= 0) {
            printf("LEASE GRANTED: drm fd = %d\n", g_lease_fd);
            int rc = play_video(g_lease_fd, target, video);
            wp_drm_lease_v1_destroy(lease);
            wl_display_roundtrip(dpy);
            wl_display_disconnect(dpy);
            return rc;
        }
        printf("  lease denied on this instance, trying next...\n");
        wp_drm_lease_v1_destroy(lease);
    }
    fprintf(stderr, "ERROR: no leasable connector named '%s' was granted.\n", target);
    wl_display_disconnect(dpy);
    return 2;
}
