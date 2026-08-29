/* vrtray.c -- single-binary system-tray app that mirrors a desktop window to
 * the WMR headset. XApp status icon (Cinnamon-native) + GTK3 menu, with the
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
 * build: gcc vrtray.c -o vrtray \
 *   $(pkg-config --cflags --libs gtk+-3.0 xapp xcb xcb-randr libdrm x11 xcomposite)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <libxapp/xapp-status-icon.h>

/* -------------------------- engine state (single process) --------------- */
struct fbuf { uint32_t handle, pitch, fb_id; uint64_t size; uint8_t *map; };
static int      g_drmfd = -1;
static uint32_t g_conn_id, g_crtc_id;
static drmModeModeInfo g_mode;
static struct fbuf g_fb[2];
static int      g_front = 0, g_flip_pending = 0;

static xcb_connection_t *g_xcb = NULL;
static xcb_randr_lease_t g_lease = 0;

static Display *g_dpy = NULL;          /* dedicated capture connection to :0 */
static Window   g_root, g_target = 0;
static Pixmap   g_pix = 0;
static int      g_pw = 0, g_ph = 0;

static const char *g_output = "DP-3-2";
static XAppStatusIcon *g_icon = NULL;

static void on_flip(int f,unsigned a,unsigned b,unsigned c,void*u){(void)f;(void)a;(void)b;(void)c;(void)u; g_flip_pending=0;}
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
static int lease_on(void){ return g_drmfd>=0; }

static int mk_fb(int W,int H,struct fbuf*b){
    uint64_t off=0; memset(b,0,sizeof*b);
    if(drmModeCreateDumbBuffer(g_drmfd,W,H,32,0,&b->handle,&b->pitch,&b->size))return -1;
    if(drmModeAddFB(g_drmfd,W,H,24,32,b->pitch,b->handle,&b->fb_id))return -1;
    if(drmModeMapDumbBuffer(g_drmfd,b->handle,&off))return -1;
    b->map=mmap(0,b->size,PROT_READ|PROT_WRITE,MAP_SHARED,g_drmfd,off);
    if(b->map==MAP_FAILED)return -1;
    memset(b->map,0,b->size); return 0;
}
static int kms_up(void){
    drmModeRes *dr=drmModeGetResources(g_drmfd);
    if(!dr||dr->count_connectors<1||dr->count_crtcs<1) return -1;
    g_conn_id=dr->connectors[0]; g_crtc_id=dr->crtcs[0];
    drmModeConnector *c=drmModeGetConnector(g_drmfd,g_conn_id);
    if(!c||c->count_modes<1){ drmModeFreeResources(dr); return -1; }
    int best=0;
    for(int m=1;m<c->count_modes;m++){
        long a=(long)c->modes[m].hdisplay*c->modes[m].vdisplay;
        long ab=(long)c->modes[best].hdisplay*c->modes[best].vdisplay;
        if(a>ab||(a==ab&&c->modes[m].vrefresh>c->modes[best].vrefresh)) best=m;
    }
    g_mode=c->modes[best]; drmModeFreeConnector(c); drmModeFreeResources(dr);
    int W=g_mode.hdisplay,H=g_mode.vdisplay;
    if(mk_fb(W,H,&g_fb[0])||mk_fb(W,H,&g_fb[1])) return -1;
    g_front=0; g_flip_pending=0;
    if(drmModeSetCrtc(g_drmfd,g_crtc_id,g_fb[0].fb_id,0,0,&g_conn_id,1,&g_mode)) return -1;
    printf("[vrtray] panel %dx%d@%dHz lit\n",W,H,g_mode.vrefresh); fflush(stdout);
    return 0;
}
static void kms_down(void){
    for(int i=0;i<2;i++){
        if(g_fb[i].map&&g_fb[i].map!=MAP_FAILED) munmap(g_fb[i].map,g_fb[i].size);
        if(g_fb[i].fb_id) drmModeRmFB(g_drmfd,g_fb[i].fb_id);
        if(g_fb[i].handle) drmModeDestroyDumbBuffer(g_drmfd,g_fb[i].handle);
        memset(&g_fb[i],0,sizeof g_fb[i]);
    }
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
static void flip_back(void){
    int back=g_front^1;
    struct pollfd pfd={.fd=g_drmfd,.events=POLLIN};
    drmEventContext ev={.version=2,.page_flip_handler=on_flip};
    if(drmModePageFlip(g_drmfd,g_crtc_id,g_fb[back].fb_id,DRM_MODE_PAGE_FLIP_EVENT,NULL)==0){
        g_flip_pending=1; int guard=0;
        while(g_flip_pending && guard++<10){ if(poll(&pfd,1,50)>0) drmHandleEvent(g_drmfd,&ev); else break; }
    } else {
        drmModeSetCrtc(g_drmfd,g_crtc_id,g_fb[back].fb_id,0,0,&g_conn_id,1,&g_mode);
    }
    g_front=back;
}
static void blank_panel(void){
    if(!lease_on()) return;
    int back=g_front^1; memset(g_fb[back].map,0,g_fb[back].size); flip_back();
}

/* ---------------------------- window capture ---------------------------- */
static int capture_into_eye(uint8_t *eye,int dw,int dh,size_t estride){
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
    double scale=(double)dw/sw; if((double)dh/sh<scale) scale=(double)dh/sh;
    int ow=(int)(sw*scale), oh=(int)(sh*scale);
    if(ow<1) ow=1;
    if(oh<1) oh=1;
    int offx=(dw-ow)/2, offy=(dh-oh)/2;
    memset(eye,0,estride*dh);
    for(int y=0;y<oh;y++){
        int sy=(int)(y/scale); if(sy>=sh) sy=sh-1;
        uint8_t *drow=eye+(size_t)(offy+y)*estride+(size_t)offx*4;
        uint8_t *srow=(uint8_t*)img->data+(size_t)sy*img->bytes_per_line;
        for(int x=0;x<ow;x++){
            int sx=(int)(x/scale); if(sx>=sw) sx=sw-1;
            uint8_t *sp=srow+(size_t)sx*Bpp;
            drow[0]=sp[0]; drow[1]=sp[1]; drow[2]=sp[2]; drow[3]=0;  /* BGRX */
            drow+=4;
        }
    }
    XDestroyImage(img);
    return 0;
}
static void present_target(void){
    if(!lease_on()||!g_target) return;
    int W=g_mode.hdisplay,H=g_mode.vdisplay,half=W/2;
    size_t estride=(size_t)half*4;
    static uint8_t *eye=NULL; static size_t eyesz=0;
    if(eyesz<estride*H){ free(eye); eye=malloc(estride*H); eyesz=estride*H; }
    if(capture_into_eye(eye,half,H,estride)!=0) return;
    int back=g_front^1;
    for(int y=0;y<H;y++){
        uint8_t *d=g_fb[back].map+(size_t)y*g_fb[back].pitch;
        memcpy(d,eye+(size_t)y*estride,estride);
        memcpy(d+estride,eye+(size_t)y*estride,estride);
    }
    flip_back();
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
static guint g_frame_src = 0;
static gboolean frame_cb(gpointer u){ (void)u; if(lease_on()&&g_target) present_target(); return G_SOURCE_CONTINUE; }
static void icon_state(void){
    const char *ic = !lease_on() ? "video-display-symbolic"
                    : (g_target ? "media-playback-start-symbolic" : "video-display-symbolic");
    const char *tt = !lease_on() ? "Headset: off"
                    : (g_target ? "Headset: mirroring" : "Headset: on (idle)");
    if(g_icon){ xapp_status_icon_set_icon_name(g_icon,ic); xapp_status_icon_set_tooltip_text(g_icon,tt); }
}
static int ensure_enabled(void){
    if(lease_on()) return 0;
    if(lease_acquire()!=0||kms_up()!=0){ lease_release(); return -1; }
    if(!g_frame_src) g_frame_src=g_timeout_add(16,frame_cb,NULL);   /* ~60fps */
    icon_state(); return 0;
}
static void do_enable(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    if(ensure_enabled()!=0) g_warning("enable failed (panel asleep? wear the headset)"); }
static void do_disable(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    g_target=0; if(g_pix){XFreePixmap(g_dpy,g_pix);g_pix=0;}
    if(lease_on()){ kms_down(); lease_release(); }
    if(g_frame_src){ g_source_remove(g_frame_src); g_frame_src=0; }
    icon_state(); }
static void do_pick(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    if(ensure_enabled()!=0){ g_warning("cannot enable headset"); return; }
    Window w=pick_window();
    if(w){ set_target(w); printf("[vrtray] mirroring 0x%lx\n",w); }
    icon_state(); }
static void do_stop(GtkMenuItem*m,gpointer u){ (void)m;(void)u;
    g_target=0; if(g_pix){XFreePixmap(g_dpy,g_pix);g_pix=0;} blank_panel(); icon_state(); }
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
    xapp_status_icon_set_name(g_icon,"vrtray");
    GtkWidget *menu=build_menu();
    xapp_status_icon_set_primary_menu(g_icon,GTK_MENU(menu));
    xapp_status_icon_set_secondary_menu(g_icon,GTK_MENU(menu));
    icon_state();

    g_unix_signal_add(SIGINT,on_signal,NULL);
    g_unix_signal_add(SIGTERM,on_signal,NULL);

    printf("[vrtray] running (output %s). Use the tray menu.\n",g_output); fflush(stdout);
    gtk_main();

    /* teardown: always release the lease */
    g_target=0; if(g_pix) XFreePixmap(g_dpy,g_pix);
    if(g_frame_src) g_source_remove(g_frame_src);
    if(lease_on()){ kms_down(); lease_release(); }
    XCloseDisplay(g_dpy);
    printf("[vrtray] exit (lease released)\n");
    return 0;
}
