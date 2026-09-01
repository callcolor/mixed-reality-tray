CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2

# Stamped into the log header so a submitted log identifies its build. CI passes
# its release version in; a git checkout describes itself; a source tarball with
# no git falls back to "unknown".
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
CPPFLAGS += -DVRMIRROR_VERSION=\"$(VERSION)\"

PREFIX ?= $(HOME)/.local

# vrmirror -- one app (mirror a desktop window to the WMR headset), two desktop
# frontends. The X11 frontend is built by default; the Wayland one on request.

# --- vrmirror-x11: X11 frontend (RandR lease + XComposite capture) ---
VRMIRROR_X11_PKGS = gtk+-3.0 xapp xcb xcb-randr libdrm x11 xcomposite

# --- vrmirror-wl: Wayland/GNOME frontend (wp_drm_lease + portal/PipeWire) ---
VRMIRROR_WL_PKGS  = libportal libpipewire-0.3 gio-2.0 wayland-client libdrm
WL_PROTO          = /usr/share/wayland-protocols/staging/drm-lease/drm-lease-v1.xml

.PHONY: all wayland-mirror install uninstall clean
all: vrmirror-x11

# vrpresent.c is the shared scanout/compositor, vrhead.c the shared gyro
# head-tracking and vrlog.c the shared logger, used by both frontends (-lm for
# the head-tracking maths).
vrmirror-x11: vrmirror_x11.c vrpresent.c vrhead.c vrlog.c vrpresent.h vrhead.h vrlog.h
	$(CC) $(CFLAGS) $(CPPFLAGS) vrmirror_x11.c vrpresent.c vrhead.c vrlog.c -o $@ \
		$$(pkg-config --cflags --libs $(VRMIRROR_X11_PKGS)) -lm

wayland-mirror: vrmirror-wl
vrmirror-wl: vrmirror_wl.c vrpresent.c vrhead.c vrlog.c vrpresent.h vrhead.h vrlog.h drm-lease-v1-protocol.c drm-lease-v1-client-protocol.h
	$(CC) $(CFLAGS) $(CPPFLAGS) vrmirror_wl.c vrpresent.c vrhead.c vrlog.c drm-lease-v1-protocol.c -o $@ \
		$$(pkg-config --cflags --libs $(VRMIRROR_WL_PKGS)) -lm

# generated Wayland protocol bindings (not committed; regenerated here)
drm-lease-v1-client-protocol.h:
	wayland-scanner client-header $(WL_PROTO) $@
drm-lease-v1-protocol.c:
	wayland-scanner private-code $(WL_PROTO) $@

# Put the binaries on PATH and the launchers where the desktop can see them, so
# the desktop-icon launch path the Wayland frontend assumes actually exists.
# Whichever frontend was not built is skipped.
install: $(PREFIX)/bin $(PREFIX)/share/applications
	@for f in vrmirror-x11 vrmirror-wl; do \
		if [ -x "$$f" ]; then \
			install -m755 "$$f" "$(PREFIX)/bin/$$f"; \
			install -m644 "desktop/$$f.desktop" "$(PREFIX)/share/applications/$$f.desktop"; \
			echo "installed $$f"; \
		else echo "skipped $$f (not built)"; fi; \
	done
	@update-desktop-database "$(PREFIX)/share/applications" 2>/dev/null || true

$(PREFIX)/bin $(PREFIX)/share/applications:
	mkdir -p $@

uninstall:
	rm -f $(PREFIX)/bin/vrmirror-x11 $(PREFIX)/bin/vrmirror-wl
	rm -f $(PREFIX)/share/applications/vrmirror-x11.desktop
	rm -f $(PREFIX)/share/applications/vrmirror-wl.desktop

clean:
	rm -f vrmirror-x11 vrmirror-wl drm-lease-v1-client-protocol.h drm-lease-v1-protocol.c
