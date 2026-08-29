/* vrpresent.h -- shared VR presenter for vrmirror (both desktop frontends).
 *
 * Given a master-capable DRM fd for one leased headset connector, this module
 * owns everything below capture: pick the panel mode, allocate double-buffered
 * dumb framebuffers, disable the cursor overlay on the leased CRTC (the actual
 * cause of the early flashing -- not the flip path), then composite each
 * captured source frame into both eye-halves and flip at vblank. It does
 * automatic side-by-side / over-under / mono stereo detection and applies a
 * per-eye offset (the seam where head rotation will later drive the image).
 *
 * The frontends (vrmirror_x11 = RandR lease + XComposite, vrmirror_wl =
 * wp_drm_lease + portal/PipeWire) differ only in how they obtain the lease and
 * capture frames. They push raw frames in here; everything else is shared.
 *
 * Loop-agnostic: vr_present_frame() issues a non-blocking flip and returns.
 * The frontend watches vr_present_fd() for readability and calls
 * vr_present_dispatch() to reap flip-completion events (clears "flip pending").
 */
#ifndef VRMIRROR_VRPRESENT_H
#define VRMIRROR_VRPRESENT_H
#include <stdint.h>
#include <stddef.h>

typedef struct vr_present vr_present;

/* Stereo source layout. AUTO detects it from frame content. */
enum {
    VR_STEREO_AUTO = -1,
    VR_STEREO_MONO =  0,
    VR_STEREO_SBS  =  1,   /* left half -> left eye, right half -> right eye */
    VR_STEREO_OU   =  2,   /* top half  -> left eye, bottom half -> right eye */
};

/* Scale mode for fitting a source frame into one eye. */
enum {
    VR_FIT_COVER = 0,      /* fill the eye, cropping overflow (default) */
    VR_FIT_LETTERBOX = 1,  /* fit the whole frame, black bars */
};

/* Bring up scanout on a leased, master-capable DRM fd: choose the largest /
 * highest-refresh mode, allocate two framebuffers, disable the cursor overlay,
 * and do the initial atomic modeset. Returns NULL on failure (and leaves the
 * fd owned by the caller). The presenter does not close drmfd. */
vr_present *vr_present_create(int drmfd);

/* Tear down framebuffers and presenter state. Does not close drmfd. */
void vr_present_destroy(vr_present *p);

/* Chosen panel mode dimensions (full panel; one eye is half the width). */
void vr_present_panel_size(const vr_present *p, int *w, int *h);
int  vr_present_refresh(const vr_present *p);   /* vrefresh (Hz) */

/* The DRM fd to poll for flip-completion events, and the reaper for them.
 * dispatch() is safe to call whenever the fd is readable. */
int  vr_present_fd(const vr_present *p);
void vr_present_dispatch(vr_present *p);

/* True if a page flip issued by vr_present_frame() has not completed yet.
 * vr_present_frame() is a no-op while this holds, so the frontend can just call
 * it every tick without tracking state itself. */
int  vr_present_flip_pending(const vr_present *p);

/* Composite one captured source frame (packed BGRX, 4 bytes/px, `stride` bytes
 * per row) into both eyes and issue a non-blocking flip. Runs stereo detection
 * (unless a layout is forced) and applies the current per-eye offset. */
void vr_present_frame(vr_present *p, const uint8_t *bgrx, int w, int h, size_t stride);

/* Draw a centered crosshair/border test pattern into both eyes and flip. Used
 * to separate our centering from the lens optics (no capture needed). */
void vr_present_test_pattern(vr_present *p);

/* Blank both eyes to black and flip. Keeps the lease, mode, and framebuffers. */
void vr_present_blank(vr_present *p);

/* --- presentation controls (env-tunable in the frontends today) --- */
void vr_present_set_stereo(vr_present *p, int mode);      /* VR_STEREO_* */
void vr_present_set_fit(vr_present *p, int fit, double zoom);
void vr_present_set_offset(vr_present *p, int xoff, int yoff);  /* per-eye px shift */

/* Per-axis head-tracking reach (px each way) of the last presented frame: the
 * shift that brings a content edge to the viewport centre (= half the scaled
 * image on that axis). Tracks the source window's aspect, so it updates as the
 * window is resized. Head tracking uses this instead of a fixed limit. */
void vr_present_pan_range(const vr_present *p, int *rx, int *ry);

#endif /* VRMIRROR_VRPRESENT_H */
