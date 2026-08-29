CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2

# vrmirror -- one app (mirror a desktop window to the WMR headset), two desktop
# frontends. The X11 frontend is built by default; the Wayland one on request.

# --- vrmirror-x11: X11 frontend (RandR lease + XComposite capture) ---
VRMIRROR_X11_PKGS = gtk+-3.0 xapp xcb xcb-randr libdrm x11 xcomposite

# --- vrmirror-wl: Wayland/GNOME frontend (wp_drm_lease + portal/PipeWire) ---
VRMIRROR_WL_PKGS  = libportal libpipewire-0.3 gio-2.0 wayland-client libdrm
WL_PROTO          = /usr/share/wayland-protocols/staging/drm-lease/drm-lease-v1.xml

.PHONY: all wayland-mirror clean
all: vrmirror-x11

# vrpresent.c is the shared scanout/compositor and vrhead.c the shared gyro
# head-tracking, used by both frontends (both need -lm).
vrmirror-x11: vrmirror_x11.c vrpresent.c vrhead.c vrpresent.h vrhead.h
	$(CC) $(CFLAGS) vrmirror_x11.c vrpresent.c vrhead.c -o $@ \
		$$(pkg-config --cflags --libs $(VRMIRROR_X11_PKGS)) -lm

wayland-mirror: vrmirror-wl
vrmirror-wl: vrmirror_wl.c vrpresent.c vrhead.c vrpresent.h vrhead.h drm-lease-v1-protocol.c drm-lease-v1-client-protocol.h
	$(CC) $(CFLAGS) vrmirror_wl.c vrpresent.c vrhead.c drm-lease-v1-protocol.c -o $@ \
		$$(pkg-config --cflags --libs $(VRMIRROR_WL_PKGS)) -lm

# generated Wayland protocol bindings (not committed; regenerated here)
drm-lease-v1-client-protocol.h:
	wayland-scanner client-header $(WL_PROTO) $@
drm-lease-v1-protocol.c:
	wayland-scanner private-code $(WL_PROTO) $@

clean:
	rm -f vrmirror-x11 vrmirror-wl drm-lease-v1-client-protocol.h drm-lease-v1-protocol.c
