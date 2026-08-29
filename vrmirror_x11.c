/* vrmirror_x11.c -- the X11 frontend of vrmirror: a single-binary system-tray
 * app that mirrors a desktop window to the WMR headset. Same app as vrmirror_wl,
 * adapted to the X11 desktop (RandR lease + XComposite capture) instead of
 * Wayland. XApp status icon (Cinnamon-native) + GTK3 menu, with the
 * DRM-lease / XComposite-capture / KMS-scanout engine folded into one process.
 *
 * Menu:
 *   Enable headset   grab the DRM lease, light the panel (one desktop flash)
 *   Disable headset  stop mirroring + release the lease (one desktop flash)
 *   Mirror a window  crosshair-click to pick a top-level window to mirror
 *   Stop mirroring   blank the panel, keep the lease (no flash)
 *   Exit             release the lease and quit
 *
 * Launching the app does nothing to the display until you Enable; quitting
 * (Exit, window-manager kill, or SIGTERM) always releases the lease.
 *
 * build: gcc vrmirror_x11.c -o vrmirror-x11 \
 *   $(pkg-config --cflags --libs gtk+-3.0 xapp xcb xcb-randr libdrm x11 xcomposite)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <libxapp/xapp-status-icon.h>
#include "vrpresent.h"
#include "vrhead.h"

/* -------------------------- engine state (single process) --------------- */
static int         g_drmfd  = -1;      /* leased DRM fd; owned here, borrowed by presenter */
static vr_present *g_present = NULL;    /* shared scanout/compositor (vrpresent.c) */
static guint       g_drm_src = 0;       /* GLib watch that reaps flip-complete events */
static vr_head    *g_head = NULL;       /* headset gyro -> per-eye offset (vrhead.c) */
static guint       g_head_src = 0;      /* GLib watch that drains IMU packets */
static int         g_base_xoff = 0, g_base_yoff = 0;  /* static centring offset (head adds to it) */

static xcb_connection_t *g_xcb = NULL;
static xcb_randr_lease_t g_lease = 0;

static Display *g_dpy = NULL;          /* dedicated capture connection to :0 */
static Window   g_root, g_target = 0;
static Pixmap   g_pix = 0;
static int      g_pw = 0, g_ph = 0;

static uint8_t *g_frame = NULL;         /* packed BGRX capture, handed to the presenter */
static size_t   g_framesz = 0;

static const char *g_output = "DP-3-2";
static XAppStatusIcon *g_icon = NULL;

/* ignore X errors from capturing windows that vanish/resize under us */
static int xerr(Display *d, XErrorEvent *e){ (void)d;(void)e; return 0; }

/* ------------------------------ lease grab ------------------------------ */
static int lease_acquire(void){
    int sp=0; g_xcb=xcb_connect(NULL,&sp);
    if(xcb_connection_has_error(g_xcb)) return -1;
    xcb_screen_iterator_t it=xcb_setup_roots_iterator(xcb_get_setup(g_xcb));
    for(int i=0;i<sp;i++) xcb_screen_next(&it);
    xcb_window_t root=it.data->root;
    xcb_randr_get_screen_resources_reply_t *res=
        xcb_randr_get_screen_resources_reply(g_xcb,xcb_randr_get_screen_resources(g_xcb,root),NULL);
    if(!res) return -1;
    xcb_randr_output_t *outs=xcb_randr_get_screen_resources_outputs(res);
    int nouts=xcb_randr_get_screen_resources_outputs_length(res);
    xcb_randr_output_t out=0; xcb_randr_crtc_t *poss=NULL; int nposs=0;
    for(int i=0;i<nouts;i++){
        xcb_randr_get_output_info_reply_t *oi=
            xcb_randr_get_output_info_reply(g_xcb,xcb_randr_get_output_info(g_xcb,outs[i],res->config_timestamp),NULL);
        if(!oi) continue;
        int len=xcb_randr_get_output_info_name_length(oi); char nm[128]; int l=len<127?len:127;
        memcpy(nm,xcb_randr_get_output_info_name(oi),l); nm[l]=0;
        if(!strcmp(nm,g_output)){ out=outs[i]; nposs=xcb_randr_get_output_info_crtcs_length(oi);
            poss=malloc(sizeof(xcb_randr_crtc_t)*nposs);
            memcpy(poss,xcb_randr_get_output_info_crtcs(oi),sizeof(xcb_randr_crtc_t)*nposs);
            free(oi); break; }
        free(oi);
    }
    if(!out){ free(res); return -1; }
    xcb_randr_crtc_t crtc=0;
    for(int i=0;i<nposs && !crtc;i++){
        xcb_randr_get_crtc_info_reply_t *ci=
            xcb_randr_get_crtc_info_reply(g_xcb,xcb_randr_get_crtc_info(g_xcb,poss[i],res->config_timestamp),NULL);
        if(!ci) continue;
        if(xcb_randr_get_crtc_info_outputs_length(ci)==0) crtc=poss[i];
        free(ci);
    }
    free(poss); free(res);
    if(!crtc) return -1;
    g_lease=xcb_generate_id(g_xcb);
    xcb_generic_error_t *err=NULL;
    xcb_randr_create_lease_reply_t *lr=
        xcb_randr_create_lease_reply(g_xcb,xcb_randr_create_lease(g_xcb,root,g_lease,1,1,&crtc,&out),&err);
    if(!lr||err) return -1;
    int nfd=lr->nfd; int *fds=xcb_randr_create_lease_reply_fds(g_xcb,lr);
    if(nfd<1) return -1;
    g_drmfd=fds[0];
    return 0;
}
static int lease_on(void){ return g_present!=NULL; }   /* headset lit */

/* reap flip-completion events from the presenter's DRM fd (clears flip-pending) */
static gboolean drm_cb(GIOChannel *ch, GIOCondition c, gpointer u){
    (void)ch;(void)c;(void)u;
    if(g_present) vr_present_dispatch(g_present);
    return G_SOURCE_CONTINUE;
}
/* apply the shared VRMIRROR_* presentation knobs (same env as the Wayland frontend) */
static void apply_present_env(void){
    if(!g_present) return;
    g_base_xoff = getenv("VRMIRROR_XOFF")?atoi(getenv("VRMIRROR_XOFF")):0;
    g_base_yoff = getenv("VRMIRROR_YOFF")?atoi(getenv("VRMIRROR_YOFF")):0;
    vr_present_set_offset(g_present, g_base_xoff, g_base_yoff);
    vr_present_set_fit(g_present,
        getenv("VRMIRROR_FIT")?VR_FIT_LETTERBOX:VR_FIT_COVER,
        getenv("VRMIRROR_ZOOM")?atof(getenv("VRMIRROR_ZOOM")):1.0);
    const char *sm=getenv("VRMIRROR_STEREO");
    if(sm){ int m=VR_STEREO_AUTO;
        if(!strcmp(sm,"sbs"))m=VR_STEREO_SBS; else if(!strcmp(sm,"ou"))m=VR_STEREO_OU;
        else if(!strcmp(sm,"mono")||!strcmp(sm,"2d"))m=VR_STEREO_MONO;
        vr_present_set_stereo(g_present,m); }
}
static void lease_release(void){
    if(g_drmfd>=0){
        if(g_xcb&&g_lease) xcb_randr_free_lease(g_xcb,g_lease,1);
        if(g_xcb) xcb_flush(g_xcb);
        close(g_drmfd); g_drmfd=-1;
    }
    if(g_xcb){ xcb_disconnect(g_xcb); g_xcb=NULL; }
    g_lease=0;
}
/* ---------------------------- window capture ---------------------------- */
/* Capture the target window into a packed BGRX frame (g_frame); the presenter
 * does the scaling, stereo split, and flip. XImage may be 24- or 32-bpp, so we
 * repack to a tight 4-byte-per-pixel buffer here. */
static int capture_frame(int *ow, int *oh){
    if(!g_target) return -1;
    XWindowAttributes wa;
    if(!XGetWindowAttributes(g_dpy,g_target,&wa)||wa.map_state!=IsViewable) return -1;
    int sw=wa.width, sh=wa.height; if(sw<1||sh<1) return -1;
    if(!g_pix||sw!=g_pw||sh!=g_ph){
        if(g_pix){ XFreePixmap(g_dpy,g_pix); g_pix=0; }
        g_pix=XCompositeNameWindowPixmap(g_dpy,g_target); g_pw=sw; g_ph=sh; XSync(g_dpy,False);
    }
    Drawable src=g_pix?g_pix:g_target;
    XImage *img=XGetImage(g_dpy,src,0,0,sw,sh,AllPlanes,ZPixmap);
    if(!img&&g_pix){ XFreePixmap(g_dpy,g_pix); g_pix=0; img=XGetImage(g_dpy,g_target,0,0,sw,sh,AllPlanes,ZPixmap); }
    if(!img) return -1;
    int Bpp=img->bits_per_pixel/8;
    size_t need=(size_t)sw*sh*4;
    if(g_framesz<need){ free(g_frame); g_frame=malloc(need); g_framesz=g_frame?need:0; }
    if(!g_frame){ XDestroyImage(img); return -1; }
    for(int y=0;y<sh;y++){
        uint8_t *drow=g_frame+(size_t)y*sw*4;
        uint8_t *srow=(uint8_t*)img->data+(size_t)y*img->bytes_per_line;
        for(int x=0;x<sw;x++){
            uint8_t *sp=srow+(size_t)x*Bpp;
            drow[0]=sp[0]; drow[1]=sp[1]; drow[2]=sp[2]; drow[3]=0;  /* BGRX */
            drow+=4;
        }
    }
    XDestroyImage(img);
    *ow=sw; *oh=sh;
    return 0;
}
static void present_target(void){
    if(!g_present||!g_target) return;
    int w,h;
    if(capture_frame(&w,&h)!=0) return;
    vr_present_frame(g_present, g_frame, w, h, (size_t)w*4);
}

/* ----------------------- click-to-pick a window ------------------------- */
static Window client_window(Window w){
    Atom wm_state=XInternAtom(g_dpy,"WM_STATE",True);
    if(wm_state==None) return w;
    Atom type; int fmt; unsigned long n,after; unsigned char *prop=NULL;
    if(XGetWindowProperty(g_dpy,w,wm_state,0,0,False,AnyPropertyType,&type,&fmt,&n,&after,&prop)==Success){
        if(prop) XFree(prop);
        if(type!=None) return w;
    }
    Window root,parent,*kids=NULL; unsigned nk=0;
    if(XQueryTree(g_dpy,w,&root,&parent,&kids,&nk)){
        Window found=0;
        for(unsigned i=0;i<nk&&!found;i++) found=client_window(kids[i]);
        if(kids) XFree(kids);
        if(found) return found;
    }
    return w;
}
static Window pick_window(void){
    Cursor cur=XCreateFontCursor(g_dpy,XC_crosshair);
    if(XGrabPointer(g_dpy,g_root,False,ButtonPressMask,GrabModeAsync,GrabModeAsync,g_root,cur,CurrentTime)!=GrabSuccess){
        XFreeCursor(g_dpy,cur); return 0; }
    Window picked=0;
    for(;;){ XEvent e; XNextEvent(g_dpy,&e);
        if(e.type==ButtonPress){ picked=e.xbutton.subwindow?e.xbutton.subwindow:e.xbutton.window; break; } }
    XUngrabPointer(g_dpy,CurrentTime); XFreeCursor(g_dpy,cur); XFlush(g_dpy);
    if(picked&&picked!=g_root) picked=client_window(picked);
    return picked;
}
static void set_target(Window w){
    if(g_pix){ XFreePixmap(g_dpy,g_pix); g_pix=0; }
    g_pw=g_ph=0; g_target=w;
    if(w) XCompositeRedirectWindow(g_dpy,w,CompositeRedirectAutomatic);
    XSync(g_dpy,False);
}

/* ------------------------------- UI glue -------------------------------- */
/* drain IMU packets whenever the hidraw fd is readable (advances head angle) */
static gboolean head_cb(GIOChannel *ch, GIOCondition c, gpointer u){
    (void)ch;(void)c;(void)u;
    if(g_head) vr_head_poll(g_head);
    return G_SOURCE_CONTINUE;
}
static guint g_frame_src = 0;
static gboolean frame_cb(gpointer u){ (void)u;
    if(g_present&&g_target&&!vr_present_flip_pending(g_present)){
        if(g_head){ int rx,ry; vr_present_pan_range(g_present,&rx,&ry); vr_head_set_range(g_head,rx,ry);
            int hx,hy; vr_head_offset(g_head,&hx,&hy);
            vr_present_set_offset(g_present, g_base_xoff+hx, g_base_yoff+hy); }
        present_target();
    }
    return G_SOURCE_CONTINUE; }
static void icon_state(void){
    const char *ic = !lease_on() ? "video-display-symbolic"
                    : (g_target ? "media-playback-start-symbolic" : "video-display-symbolic");
    const char *tt = !lease_on() ? "Headset: off"
                    : (g_target ? "Headset: mirroring" : "Headset: on (idle)");
    if(g_icon){ xapp_status_icon_set_icon_name(g_icon,ic); xapp_status_icon_set_tooltip_text(g_icon,tt); }
}
static int ensure_enabled(void){
    if(g_present) return 0;
    if(lease_acquire()!=0){ lease_release(); return -1; }
    g_present=vr_present_create(g_drmfd);
    if(!g_present){ lease_release(); return -1; }
    apply_present_env();
    GIOChannel *ch=g_io_channel_unix_new(vr_present_fd(g_present));   /* reap flip events */
    g_drm_src=g_io_add_watch(ch,G_IO_IN,drm_cb,NULL);
    g_io_channel_unref(ch);
    if(!g_head){                                                     /* headset gyro (optional) */
        g_head=vr_head_open(NULL);
        if(g_head){ GIOChannel *hc=g_io_channel_unix_new(vr_head_fd(g_head));
            g_head_src=g_io_add_watch(hc,G_IO_IN,head_cb,NULL); g_io_channel_unref(hc); }
    }
    if(!g_frame_src) g_frame_src=g_timeout_add(16,frame_cb,NULL);   /* ~60fps */
    icon_state(); return 0;
}
static void do_enable(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    if(ensure_enabled()!=0) g_warning("enable failed (panel asleep? wear the headset)"); }
static void do_disable(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    g_target=0; if(g_pix){XFreePixmap(g_dpy,g_pix);g_pix=0;}
    if(g_drm_src){ g_source_remove(g_drm_src); g_drm_src=0; }
    if(g_head_src){ g_source_remove(g_head_src); g_head_src=0; }
    if(g_head){ vr_head_close(g_head); g_head=NULL; }
    if(g_present){ vr_present_destroy(g_present); g_present=NULL; }
    if(g_drmfd>=0) lease_release();
    if(g_frame_src){ g_source_remove(g_frame_src); g_frame_src=0; }
    icon_state(); }
static void do_pick(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    if(ensure_enabled()!=0){ g_warning("cannot enable headset"); return; }
    Window w=pick_window();
    if(w){ set_target(w); printf("[x11] mirroring 0x%lx\n",w); }
    icon_state(); }
static void do_stop(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    g_target=0; if(g_pix){XFreePixmap(g_dpy,g_pix);g_pix=0;}
    if(g_present) vr_present_blank(g_present);
    icon_state(); }
static void do_quit(GtkMenuItem*m,gpointer u){ (void)m;(void)u; gtk_main_quit(); }

static GtkWidget* build_menu(void){
    GtkWidget *menu=gtk_menu_new();
    struct { const char*label; GCallback cb; } items[] = {
        {"Enable headset", G_CALLBACK(do_enable)},
        {"Disable headset",G_CALLBACK(do_disable)},
        {NULL,NULL},
        {"Mirror a window…", G_CALLBACK(do_pick)},
        {"Stop mirroring", G_CALLBACK(do_stop)},
        {NULL,NULL},
        {"Exit", G_CALLBACK(do_quit)},
    };
    for(unsigned i=0;i<sizeof(items)/sizeof(items[0]);i++){
        GtkWidget *it = items[i].label ? gtk_menu_item_new_with_label(items[i].label)
                                       : gtk_separator_menu_item_new();
        if(items[i].label) g_signal_connect(it,"activate",items[i].cb,NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),it);
    }
    gtk_widget_show_all(menu);
    return menu;
}
static gboolean on_signal(gpointer u){ (void)u; gtk_main_quit(); return G_SOURCE_REMOVE; }

int main(int argc,char**argv){
    gtk_init(&argc,&argv);
    if(argc>1) g_output=argv[1];
    XSetErrorHandler(xerr);
    g_dpy=XOpenDisplay(NULL);
    if(!g_dpy){ fprintf(stderr,"cannot open X display\n"); return 1; }
    g_root=DefaultRootWindow(g_dpy);
    int evb,erb; if(!XCompositeQueryExtension(g_dpy,&evb,&erb)){ fprintf(stderr,"no XComposite\n"); return 1; }

    g_icon=xapp_status_icon_new();
    xapp_status_icon_set_name(g_icon,"vrmirror-x11");
    GtkWidget *menu=build_menu();
    xapp_status_icon_set_primary_menu(g_icon,GTK_MENU(menu));
    xapp_status_icon_set_secondary_menu(g_icon,GTK_MENU(menu));
    icon_state();

    g_unix_signal_add(SIGINT,on_signal,NULL);
    g_unix_signal_add(SIGTERM,on_signal,NULL);

    printf("[x11] running (output %s). Use the tray menu.\n",g_output); fflush(stdout);
    gtk_main();

    /* teardown: always release the lease */
    g_target=0; if(g_pix) XFreePixmap(g_dpy,g_pix);
    if(g_frame_src) g_source_remove(g_frame_src);
    if(g_drm_src) g_source_remove(g_drm_src);
    if(g_head_src) g_source_remove(g_head_src);
    if(g_head){ vr_head_close(g_head); g_head=NULL; }
    if(g_present){ vr_present_destroy(g_present); g_present=NULL; }
    if(g_drmfd>=0) lease_release();
    XCloseDisplay(g_dpy);
    printf("[x11] exit (lease released)\n");
    return 0;
}
