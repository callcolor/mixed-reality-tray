# mixed-reality-tray

Drive an old Windows Mixed Reality headset (Acer AH101) as a plain content-viewing
display on Linux — no Monado, no SteamVR. The headset panel is leased directly from
the running desktop via a DRM lease and driven with our own KMS scanout, so the rest
of the desktop is untouched.

## vrmirror — the app

A system-tray app that **mirrors any desktop window onto the headset**. Run a video in
VLC (or a browser, or anything) on the desktop; vrmirror captures that window and scans
it out to both eyes. You watch on the monitor and in the headset at the same time, and
seeking/pausing in the source app just follows.

It's one app with a frontend per desktop — same lease → capture → KMS-scanout engine,
adapted to how each display server exposes leases and window capture:

| Frontend         | Source file      | Desktop       | Lease          | Capture           |
|------------------|------------------|---------------|----------------|-------------------|
| **vrmirror-x11** | `vrmirror_x11.c` | X11           | RandR lease    | XComposite        |
| **vrmirror-wl**  | `vrmirror_wl.c`  | Wayland/GNOME | `wp_drm_lease` | portal + PipeWire |

Both share one presenter (`vrpresent.c`): mode pick, double-buffered KMS scanout,
stereo split, and the vblank flip. (A single launcher that picks the right frontend for
the running session is a likely future addition.)

Common to both:

- Holds the DRM lease for the whole session, so the desktop only re-probes (a brief
  one-time flash) on **Enable** and **Disable** — never when videos or windows change.
- **Auto stereo detection** — side-by-side / over-under / mono is detected from the
  frame content, no toggle.
- **Head tracking** — turn your head to look around the mirrored window (see below).
- The mirrored window can sit in the background (just don't minimize it).

### Build & run

    make                   # builds ./vrmirror-x11 (the X11 frontend)
    ./vrmirror-x11         # launch from a terminal inside your X session

    make wayland-mirror    # builds ./vrmirror-wl (needs wayland-scanner)

Then use the tray icon:

| Menu item        | Effect |
|------------------|--------|
| Enable headset   | Grab the lease, light the panel (one desktop flash). Put the headset on first. |
| Mirror a window… | Crosshair-click the window to mirror. |
| Stop mirroring   | Blank the panel, keep the lease (no flash). |
| Disable headset  | Release the lease (one flash). |
| Exit             | Release the lease and quit. |

## Head tracking

Light gyro-only head tracking (`vrhead.c`) — not full 6DoF, just enough to **turn your
head and look around the mirrored window**. It reads the headset's built-in gyroscope
(the "Microsoft HoloLens Sensors" HID interface, auto-detected) directly — no Monado, no
external tracking.

- The view **holds wherever you look** (no auto-recenter tug).
- The pan **reach follows the window's aspect** and updates live as it's resized: a wide
  window pans left/right, a tall one up/down. Reach is set so any content **edge can be
  brought to the centre of your view**, in every direction.
- Bias is auto-zeroed from the first ~0.6 s at rest, so hold still briefly on Enable.

It needs read access to the HoloLens Sensors hidraw node (normally granted to the active
seat's user automatically). If head tracking is silent, check that access; set
`VRMIRROR_HEAD=0` to disable it entirely.

Tunables (environment variables):

| Variable                 | Default | Meaning |
|--------------------------|---------|---------|
| `VRMIRROR_HEAD`          | on      | `0`/`off` disables head tracking |
| `VRMIRROR_HEAD_GAIN`     | `40`    | pixels of pan per degree of head turn (sensitivity) |
| `VRMIRROR_HEAD_MARGIN`   | `0`     | extra px to pan past an edge-at-centre |
| `VRMIRROR_HEAD_MAX`      | `0`     | optional cap on reach in px (`0` = uncapped) |
| `VRMIRROR_HEAD_RECENTER` | `0`     | seconds; `>0` re-enables a slow drift-back to centre |
| `VRMIRROR_HEAD_DEBUG`    | off     | print per-axis rate and offset to stderr |

Axis mapping and signs (`VRMIRROR_HEAD_YAW_AXIS` / `_PITCH_AXIS` / `_YAW_SIGN` /
`_PITCH_SIGN`) default to the measured Acer AH101 layout; override if your headset's
gyro is oriented differently.

## Notes / current limits

- **Output name.** The X11 frontend defaults to output `DP-3-2`; pass another as
  `./vrmirror-x11 <OUTPUT>`. (The Wayland frontend defaults to `DP-7`.)
- **No lens distortion yet.** The image is scanned out flat to each eye; per-eye lens
  distortion is the next step (the presenter already isolates the per-eye seam for it).
- **Gyro drift.** With recenter off (the default), "straight ahead" can wander a few
  degrees over a long session; a recenter control is a planned addition.

## Dependencies (Fedora)

    sudo dnf install gtk3-devel xapps-devel libayatana-appindicator-gtk3-devel \
                     libxcb-devel libX11-devel libXcomposite-devel libdrm-devel
    # vrmirror-wl only:
    sudo dnf install libportal-devel pipewire-devel wayland-devel wayland-protocols-devel

## License

MIT — see [LICENSE](LICENSE).
