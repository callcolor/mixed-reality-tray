/* vrhead.h -- light head-tracking for vrmirror from the WMR IMU.
 *
 * Not full 6DoF head tracking: this reads the headset gyroscope (the "Microsoft
 * HoloLens Sensors" HID interface on WMR gen1, e.g. the Acer AH101), integrates
 * a little yaw/pitch, and turns it into the per-eye pixel offset the presenter
 * already applies (vr_present_set_offset). The intent is "turn your head a bit
 * to look at the corners of the mirrored window": the view HOLDS wherever you
 * look. Turn too far and the angle pins at the visible edge -- the image stays
 * put as you keep turning (it comes with you) and tracks again the moment you
 * turn back. No auto-recentre by default (that felt like the world drifting out
 * from under you); an optional slow recentre is available via env.
 *
 * Everything is a small integrator over raw gyro counts; the gyro bias is
 * auto-zeroed from the first ~0.6 s at rest. Gain, clamp, recentre and the
 * yaw/pitch axis+sign are env-tunable (VRMIRROR_HEAD_*), because which raw gyro
 * axis is yaw vs pitch depends on the headset and is easiest to confirm live.
 */
#ifndef VRMIRROR_VRHEAD_H
#define VRMIRROR_VRHEAD_H

typedef struct vr_head vr_head;

/* Open the WMR IMU and start it streaming. `path` is a hidraw node
 * ("/dev/hidraw6"); NULL auto-detects the "HoloLens Sensors" interface. Returns
 * NULL when tracking is disabled (VRMIRROR_HEAD=0), no device is found, or open
 * fails -- the caller then simply runs without head tracking. */
vr_head *vr_head_open(const char *path);
void vr_head_close(vr_head *h);

/* fd to watch for readability in the main loop (-1 if none). */
int vr_head_fd(const vr_head *h);

/* Drain all pending IMU packets and advance the integrated view angle and its
 * decay-to-centre. Call whenever vr_head_fd() is readable. */
void vr_head_poll(vr_head *h);

/* Current head-driven per-eye pixel offset (a delta to add to the static
 * centring offset before vr_present_set_offset). Zero until bias is settled. */
void vr_head_offset(const vr_head *h, int *xoff, int *yoff);

/* Set the per-axis pan reach in pixels (how far the view may shift each way),
 * normally fed each frame from vr_present_pan_range() so the reach follows the
 * mirrored window's aspect and resizes. An axis with 0 reach does not pan. */
void vr_head_set_range(vr_head *h, int rx, int ry);

/* Make the current head pose the new centre. */
void vr_head_recenter(vr_head *h);

#endif /* VRMIRROR_VRHEAD_H */
