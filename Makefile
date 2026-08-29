CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2

# --- vrtray: the product (X11 tray app that mirrors a window to the headset) ---
VRTRAY_PKGS = gtk+-3.0 xapp xcb xcb-randr libdrm x11 xcomposite

# --- vrmirror-wl: the Wayland/GNOME counterpart (portal + PipeWire capture) ---
VRMIRROR_PKGS = libportal libpipewire-0.3 gio-2.0 wayland-client libdrm

# --- vrplayer: optional Wayland file-player (kept for Wayland lease/scanout) ---
VRPLAYER_PKGS = mpv wayland-client libdrm
WL_PROTO      = /usr/share/wayland-protocols/staging/drm-lease/drm-lease-v1.xml

.PHONY: all wayland wayland-mirror clean
all: vrtray

vrtray: vrtray.c
	$(CC) $(CFLAGS) $< -o $@ $$(pkg-config --cflags --libs $(VRTRAY_PKGS))

wayland-mirror: vrmirror-wl
vrmirror-wl: vrmirror_wl.c drm-lease-v1-protocol.c drm-lease-v1-client-protocol.h
	$(CC) $(CFLAGS) vrmirror_wl.c drm-lease-v1-protocol.c -o $@ \
		$$(pkg-config --cflags --libs $(VRMIRROR_PKGS)) -lm

wayland: vrplayer

# generated Wayland protocol bindings (not committed; regenerated here)
drm-lease-v1-client-protocol.h:
	wayland-scanner client-header $(WL_PROTO) $@
drm-lease-v1-protocol.c:
	wayland-scanner private-code $(WL_PROTO) $@

vrplayer: vrplayer.c present.c present.h drm-lease-v1-protocol.c drm-lease-v1-client-protocol.h
	$(CC) $(CFLAGS) vrplayer.c present.c drm-lease-v1-protocol.c -o $@ \
		$$(pkg-config --cflags --libs $(VRPLAYER_PKGS))

clean:
	rm -f vrtray vrplayer vrmirror-wl drm-lease-v1-client-protocol.h drm-lease-v1-protocol.c
