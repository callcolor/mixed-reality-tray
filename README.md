# mixed-reality-sucks

Drive an old Windows Mixed Reality headset (Acer AH101) as a plain content-viewing
display on Linux — no Monado, no SteamVR. The headset panel is leased directly from
the running desktop via a DRM lease and driven with our own KMS scanout, so the rest
of the desktop is untouched.

## vrtray — the app

A system-tray app that **mirrors any desktop window onto the headset**. Run a video in
VLC (or a browser, or anything) on the desktop; `vrtray` captures that window and scans
it out to both eyes. You watch on the monitor and in the headset at the same time, and
seeking/pausing in the source app just follows.

- Holds the DRM lease for the whole session, so the desktop only re-probes (a brief
  one-time flash) on **Enable** and **Disable** — never when videos or windows change.
- Window capture uses XComposite, so the mirrored window can sit in the background
  (just don't minimize it).

### Build & run

    make            # builds ./vrtray
    ./vrtray        # launch from a terminal inside your X session

Then use the tray icon:

| Menu item        | Effect |
|------------------|--------|
| Enable headset   | Grab the lease, light the panel (one desktop flash). Put the headset on first. |
| Mirror a window… | Crosshair-click the window to mirror. |
| Stop mirroring   | Blank the panel, keep the lease (no flash). |
| Disable headset  | Release the lease (one flash). |
| Exit             | Release the lease and quit. |

### Notes / current limits

- **X11 only.** Uses an X RandR lease + XComposite capture. The output name is `DP-3-2`
  by default; pass another as `./vrtray <OUTPUT>`.
- **Flat / mono** — the same image goes to both eyes, with no lens-distortion
  correction yet, so it warps through the optics. Per-eye distortion and side-by-side
  3D are the planned next steps.

## vrplayer — optional Wayland file player

A standalone libmpv player that decodes a video file straight to the headset under
**Wayland** (using the `wp_drm_lease` protocol instead of X). Kept as the reference
implementation for leasing + KMS scanout on Wayland; not needed for `vrtray`.

    make wayland    # builds ./vrplayer (needs libmpv + wayland-scanner)
    XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ./vrplayer video.mp4

## Dependencies (Fedora)

    sudo dnf install gtk3-devel xapps-devel libayatana-appindicator-gtk3-devel \
                     libxcb-devel libX11-devel libXcomposite-devel libdrm-devel
    # vrplayer only:
    sudo dnf install mpv-libs-devel wayland-devel wayland-protocols-devel
