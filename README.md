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

| Frontend        | Source file      | Desktop | Lease          | Capture     |
|-----------------|------------------|---------|----------------|-------------|
| **vrmirror-x11** | `vrmirror_x11.c` | X11     | RandR lease    | XComposite  |
| **vrmirror-wl**  | `vrmirror_wl.c`  | Wayland/GNOME | `wp_drm_lease` | portal + PipeWire |

(A single launcher that picks the right frontend for the running session is a likely
future addition.)

Common to both:

- Holds the DRM lease for the whole session, so the desktop only re-probes (a brief
  one-time flash) on **Enable** and **Disable** — never when videos or windows change.
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

### Notes / current limits

- **Output name.** The X11 frontend defaults to output `DP-3-2`; pass another as
  `./vrmirror-x11 <OUTPUT>`.
- **Stereo.** vrmirror-wl auto-detects side-by-side / over-under / mono from the frame
  content; vrmirror-x11 is still flat/mono. Unifying the presenter (so both get stereo,
  and eventually per-eye lens distortion and a little head rotation) is the current work.

## Dependencies (Fedora)

    sudo dnf install gtk3-devel xapps-devel libayatana-appindicator-gtk3-devel \
                     libxcb-devel libX11-devel libXcomposite-devel libdrm-devel
    # vrmirror-wl only:
    sudo dnf install libportal-devel pipewire-devel wayland-devel wayland-protocols-devel
