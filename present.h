/* present.h -- display-server-agnostic VR presenter.
 * Given a master-capable DRM fd for one leased connector, decode `video` with
 * libmpv and scan it out (flat, duplicated to both eye-halves) until EOF or
 * SIGINT. The lease is obtained by a front-end (X11 or Wayland) and passed in.
 */
#ifndef VR_PRESENT_H
#define VR_PRESENT_H
int play_video(int drmfd, const char *target, const char *video);
#endif
