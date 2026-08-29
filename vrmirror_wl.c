/* vrmirror_wl.c -- the Wayland/GNOME frontend of vrmirror: mirror a desktop
 * window to the WMR headset. Same app as vrmirror_x11, adapted to Wayland
 * (wp_drm_lease + portal/PipeWire capture). Launched from a desktop icon (no
 * CLI, no window).
 *
 * Flow, entirely click-driven:
 *   launch -> GNOME ScreenCast portal asks which window/screen (one click)
 *          -> lease the headset via wp_drm_lease, light the panel
 *          -> stream the picked source via PipeWire and scan it out (both eyes)
 *   stop   -> use GNOME's top-bar screen-sharing indicator; the portal session
 *             closes, we catch it, release the lease, and exit.
 * Pick a different window by launching again and choosing it.
 *
 * The lease is only taken AFTER a successful pick, so cancelling the portal
 * dialog causes no desktop flash.
 *
 * build: see Makefile target `wayland-mirror`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <glib-unix.h>
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <wayland-client.h>
#include <xf86drmMode.h>          /* wl_lease_acquire validates the leased fd */
#include "drm-lease-v1-client-protocol.h"
#include "vrpresent.h"
#include "vrhead.h"

static GMainLoop *g_loop;
static const char *g_output = "DP-7";

/* =============================== Wayland lease ========================== */
struct conn_info {
    struct wp_drm_lease_connector_v1 *proxy;
    struct wp_drm_lease_device_v1 *dev;
    char name[128]; uint32_t connector_id; struct conn_info *next;
};
static struct conn_info *g_conns = NULL;
static struct wl_display *g_wl = NULL;
static struct wp_drm_lease_v1 *g_lease_obj = NULL;
static int g_lease_fd = -1, g_lease_finished = 0;

static void c_name(void*d,struct wp_drm_lease_connector_v1*p,const char*n){struct conn_info*c=d;(void)p;snprintf(c->name,sizeof c->name,"%s",n);}
static void c_desc(void*d,struct wp_drm_lease_connector_v1*p,const char*s){(void)d;(void)p;(void)s;}
static void c_id(void*d,struct wp_drm_lease_connector_v1*p,uint32_t i){struct conn_info*c=d;(void)p;c->connector_id=i;}
static void c_withdrawn(void*d,struct wp_drm_lease_connector_v1*p){struct conn_info*c=d;(void)p;c->name[0]=0;}
static void c_done(void*d,struct wp_drm_lease_connector_v1*p){(void)d;(void)p;}
static const struct wp_drm_lease_connector_v1_listener conn_listener={.name=c_name,.description=c_desc,.connector_id=c_id,.withdrawn=c_withdrawn,.done=c_done};

static void d_drm_fd(void*d,struct wp_drm_lease_device_v1*p,int fd){(void)d;(void)p;close(fd);}
static void d_connector(void*d,struct wp_drm_lease_device_v1*dev,struct wp_drm_lease_connector_v1*conn){
    (void)d; struct conn_info*c=calloc(1,sizeof*c); c->proxy=conn; c->dev=dev; c->next=g_conns; g_conns=c;
    wp_drm_lease_connector_v1_add_listener(conn,&conn_listener,c);
}
static void d_released(void*d,struct wp_drm_lease_device_v1*p){(void)d;(void)p;}
static void d_done(void*d,struct wp_drm_lease_device_v1*p){(void)d;(void)p;}
static const struct wp_drm_lease_device_v1_listener dev_listener={.drm_fd=d_drm_fd,.connector=d_connector,.released=d_released,.done=d_done};

static void reg_global(void*d,struct wl_registry*r,uint32_t name,const char*iface,uint32_t ver){
    (void)d;(void)ver;
    if(!strcmp(iface,wp_drm_lease_device_v1_interface.name)){
        struct wp_drm_lease_device_v1*dev=wl_registry_bind(r,name,&wp_drm_lease_device_v1_interface,1);
        wp_drm_lease_device_v1_add_listener(dev,&dev_listener,NULL);
    }
}
static void reg_remove(void*d,struct wl_registry*r,uint32_t n){(void)d;(void)r;(void)n;}
static const struct wl_registry_listener reg_listener={.global=reg_global,.global_remove=reg_remove};

static void l_lease_fd(void*d,struct wp_drm_lease_v1*p,int fd){(void)d;(void)p;g_lease_fd=fd;}
static void l_finished(void*d,struct wp_drm_lease_v1*p){(void)d;(void)p;g_lease_finished=1;}
static const struct wp_drm_lease_v1_listener lease_listener={.lease_fd=l_lease_fd,.finished=l_finished};

/* returns leased master-capable DRM fd, or -1 */
static int wl_lease_acquire(void){
    g_wl=wl_display_connect(NULL);
    if(!g_wl){ fprintf(stderr,"[wl] cannot connect\n"); return -1; }
    struct wl_registry*reg=wl_display_get_registry(g_wl);
    wl_registry_add_listener(reg,&reg_listener,NULL);
    wl_display_roundtrip(g_wl); wl_display_roundtrip(g_wl); wl_display_roundtrip(g_wl);
    for(struct conn_info*c=g_conns;c;c=c->next){
        if(strcmp(c->name,g_output)!=0) continue;
        fprintf(stderr,"[wl] trying '%s' on lease-device %p\n",c->name,(void*)c->dev);
        struct wp_drm_lease_request_v1*req=wp_drm_lease_device_v1_create_lease_request(c->dev);
        wp_drm_lease_request_v1_request_connector(req,c->proxy);
        struct wp_drm_lease_v1*lease=wp_drm_lease_request_v1_submit(req);
        wp_drm_lease_v1_add_listener(lease,&lease_listener,NULL);
        g_lease_fd=-1; g_lease_finished=0;
        while(g_lease_fd<0 && !g_lease_finished) if(wl_display_roundtrip(g_wl)<0) break;
        if(g_lease_fd<0){ fprintf(stderr,"[wl] denied on this device\n"); wp_drm_lease_v1_destroy(lease); continue; }
        /* validate: a grant from the wrong GPU enumerates no KMS resources */
        drmModeRes*r=drmModeGetResources(g_lease_fd);
        if(r&&r->count_connectors>=1&&r->count_crtcs>=1){
            fprintf(stderr,"[wl] leased %s (usable: %d conn / %d crtc)\n",g_output,r->count_connectors,r->count_crtcs);
            drmModeFreeResources(r); g_lease_obj=lease; return g_lease_fd;
        }
        fprintf(stderr,"[wl] grant unusable (%s); trying next device\n", r?"no conn/crtc":strerror(errno));
        if(r) drmModeFreeResources(r);
        close(g_lease_fd); g_lease_fd=-1;
        wp_drm_lease_v1_destroy(lease);
    }
    fprintf(stderr,"[wl] no usable leasable connector '%s'\n",g_output);
    return -1;
}

/* ========================= KMS (shared presenter) ===================== */
/* Mode pick, framebuffers, cursor-overlay disable, stereo split and flip all
 * live in vrpresent.c now (shared with the X11 frontend). Here we only hold the
 * leased fd and hand captured frames in. */
static int g_drmfd = -1;                  /* leased DRM fd; borrowed by the presenter */
static vr_present *g_present = NULL;       /* shared scanout/compositor (vrpresent.c) */
static int g_test = 0;                     /* draw a centered crosshair instead of capturing */
static vr_head *g_head = NULL;             /* headset gyro -> per-eye offset (vrhead.c) */
static int g_base_xoff = 0, g_base_yoff = 0;  /* static centring offset (head adds to it) */

static gboolean frame_cb(gpointer);        /* fwd: present newest frame */

/* dispatch page-flip completion events (registered on the GLib loop) */
static gboolean drm_ev_cb(GIOChannel*ch,GIOCondition cond,gpointer u){
    (void)ch;(void)cond;(void)u;
    if(g_present) vr_present_dispatch(g_present);   /* clears flip-pending */
    frame_cb(NULL);                                 /* present newest at this vblank */
    return G_SOURCE_CONTINUE;
}
/* apply the shared VRMIRROR_* presentation knobs (same env as the X11 frontend) */
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
/* drain IMU packets whenever the hidraw fd is readable (advances head angle) */
static gboolean head_cb(GIOChannel*ch,GIOCondition cond,gpointer u){
    (void)ch;(void)cond;(void)u;
    if(g_head) vr_head_poll(g_head);
    return G_SOURCE_CONTINUE;
}
/* create the presenter on the leased fd and wire flip-event delivery */
static int present_up(void){
    g_present = vr_present_create(g_drmfd);
    if(!g_present) return -1;
    apply_present_env();
    GIOChannel *drmch = g_io_channel_unix_new(vr_present_fd(g_present));
    g_io_add_watch(drmch,G_IO_IN,drm_ev_cb,NULL);
    g_io_channel_unref(drmch);
    g_head = vr_head_open(NULL);                     /* headset gyro (optional) */
    if(g_head){ GIOChannel *hc = g_io_channel_unix_new(vr_head_fd(g_head));
        g_io_add_watch(hc,G_IO_IN,head_cb,NULL); g_io_channel_unref(hc); }
    return 0;
}

/* ============================ capture (PipeWire) ======================== */
static struct pw_thread_loop *g_pw; static struct pw_stream *g_stream;
static struct spa_video_info_raw g_vinfo;
static GMutex g_lock; static uint8_t *g_latest=NULL; static int g_lw=0,g_lh=0,g_have=0;

static void on_param_changed(void*u,uint32_t id,const struct spa_pod*param){
    (void)u; if(!param||id!=SPA_PARAM_Format)return;
    uint32_t mt,mst; if(spa_format_parse(param,&mt,&mst)<0)return;
    if(mt!=SPA_MEDIA_TYPE_video||mst!=SPA_MEDIA_SUBTYPE_raw)return;
    spa_format_video_raw_parse(param,&g_vinfo);
    printf("[cap] %ux%u\n",g_vinfo.size.width,g_vinfo.size.height); fflush(stdout);
    uint8_t b[1024]; struct spa_pod_builder pb=SPA_POD_BUILDER_INIT(b,sizeof b);
    const struct spa_pod*ps[2];
    ps[0]=spa_pod_builder_add_object(&pb,
        SPA_TYPE_OBJECT_ParamBuffers,SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,SPA_POD_CHOICE_RANGE_Int(4,2,16),
        SPA_PARAM_BUFFERS_dataType,SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemFd)|(1<<SPA_DATA_MemPtr)));
    /* advertise VideoCrop meta support so mutter tells us the window's real
     * sub-region within its fixed padded buffer */
    ps[1]=spa_pod_builder_add_object(&pb,
        SPA_TYPE_OBJECT_ParamMeta,SPA_PARAM_Meta,
        SPA_PARAM_META_type,SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size,SPA_POD_Int(sizeof(struct spa_meta_region)));
    pw_stream_update_params(g_stream,ps,2);
}
static void on_process(void*u){
    (void)u;
    /* drain the whole queue, keep ONLY the newest buffer -- otherwise a backlog
     * is consumed oldest-first and momentarily rewinds g_latest (rubber-banding) */
    struct pw_buffer*pb,*last=NULL;
    while((pb=pw_stream_dequeue_buffer(g_stream))){
        if(last) pw_stream_queue_buffer(g_stream,last);
        last=pb;
    }
    if(!last) return;
    struct spa_buffer*buf=last->buffer;
    struct spa_data*d=&buf->datas[0];
    int w=g_vinfo.size.width,h=g_vinfo.size.height;
    int sstride=d->chunk?d->chunk->stride:w*4;
    int cx=0,cy=0;
    /* window content is a top-left sub-region of a fixed padded buffer; VideoCrop
     * meta gives the valid area. Without it, the right/bottom padding counts as
     * content and a shrunk window drifts left. */
    struct spa_meta_region*cr=spa_buffer_find_meta_data(buf,SPA_META_VideoCrop,sizeof(*cr));
    if(cr && spa_meta_region_is_valid(cr)){
        cx=cr->region.position.x; cy=cr->region.position.y;
        w=cr->region.size.width;  h=cr->region.size.height;
    }
    uint8_t*src=(uint8_t*)d->data + (size_t)cy*sstride + (size_t)cx*4;
    if(d->data&&w>0&&h>0){
        g_mutex_lock(&g_lock);
        if(g_lw!=w||g_lh!=h){ free(g_latest); g_latest=malloc((size_t)w*h*4); g_lw=w; g_lh=h; }
        for(int y=0;y<h;y++) memcpy(g_latest+(size_t)y*w*4, src+(size_t)y*sstride, (size_t)w*4);
        g_have=1; g_mutex_unlock(&g_lock);
    }
    pw_stream_queue_buffer(g_stream,last);
}
static const struct pw_stream_events stream_events={PW_VERSION_STREAM_EVENTS,.param_changed=on_param_changed,.process=on_process};

static void start_pipewire(int fd,uint32_t node){
    pw_init(NULL,NULL);
    g_pw=pw_thread_loop_new("vrmirror",NULL);
    struct pw_context*ctx=pw_context_new(pw_thread_loop_get_loop(g_pw),NULL,0);
    pw_thread_loop_start(g_pw); pw_thread_loop_lock(g_pw);
    struct pw_core*core=pw_context_connect_fd(ctx,fd,NULL,0);
    g_stream=pw_stream_new(core,"vrmirror",
        pw_properties_new(PW_KEY_MEDIA_TYPE,"Video",PW_KEY_MEDIA_CATEGORY,"Capture",PW_KEY_MEDIA_ROLE,"Screen",NULL));
    static struct spa_hook h; pw_stream_add_listener(g_stream,&h,&stream_events,NULL);
    uint8_t b[1024]; struct spa_pod_builder pb=SPA_POD_BUILDER_INIT(b,sizeof b);
    const struct spa_pod*params[1];
    params[0]=spa_pod_builder_add_object(&pb,
        SPA_TYPE_OBJECT_Format,SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype,SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,SPA_POD_CHOICE_ENUM_Id(3,SPA_VIDEO_FORMAT_BGRx,SPA_VIDEO_FORMAT_BGRx,SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size,SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1280,720),&SPA_RECTANGLE(1,1),&SPA_RECTANGLE(8192,8192)),
        SPA_FORMAT_VIDEO_framerate,SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(30,1),&SPA_FRACTION(0,1),&SPA_FRACTION(120,1)));
    pw_stream_connect(g_stream,PW_DIRECTION_INPUT,node,PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS,params,1);
    pw_thread_loop_unlock(g_pw);
}

/* =============================== present =============================== */
/* Pull the newest captured frame and hand it to the shared presenter, which
 * does the stereo split, scaling, and vblank flip (see vrpresent.c). The lock
 * is held across the present because the presenter reads g_latest directly; the
 * non-blocking flip commit inside it returns at once, so the hold is brief. */
static gboolean frame_cb(gpointer u){
    (void)u;
    if(!g_present || vr_present_flip_pending(g_present)) return G_SOURCE_CONTINUE;
    if(g_head){ int rx,ry; vr_present_pan_range(g_present,&rx,&ry); vr_head_set_range(g_head,rx,ry);
        int hx,hy; vr_head_offset(g_head,&hx,&hy);
        vr_present_set_offset(g_present, g_base_xoff+hx, g_base_yoff+hy); }
    if(g_test){ vr_present_test_pattern(g_present); return G_SOURCE_CONTINUE; }
    g_mutex_lock(&g_lock);
    if(g_have) vr_present_frame(g_present, g_latest, g_lw, g_lh, (size_t)g_lw*4);
    g_mutex_unlock(&g_lock);
    return G_SOURCE_CONTINUE;
}

/* =============================== portal ================================ */
static void cleanup_and_quit(void){ g_main_loop_quit(g_loop); }
static void on_closed(XdpSession*s,gpointer u){ (void)s;(void)u; printf("[portal] session closed (stopped from top bar)\n"); cleanup_and_quit(); }

static void on_started(GObject*src,GAsyncResult*res,gpointer u){
    (void)u; XdpSession*session=XDP_SESSION(src); GError*err=NULL;
    if(!xdp_session_start_finish(session,res,&err)){ fprintf(stderr,"[portal] start failed/cancelled: %s\n",err?err->message:"?"); cleanup_and_quit(); return; }
    GVariant*streams=xdp_session_get_streams(session);
    GVariantIter it; guint32 node=0; GVariant*props=NULL; g_variant_iter_init(&it,streams);
    if(!g_variant_iter_next(&it,"(u@a{sv})",&node,&props)){ fprintf(stderr,"[portal] no stream\n"); cleanup_and_quit(); return; }
    if(props)g_variant_unref(props);
    /* pick succeeded -> now take the lease (so cancelling above caused no flash) */
    g_drmfd=wl_lease_acquire();
    if(g_drmfd<0||present_up()<0){ fprintf(stderr,"[wl] lease/present failed (headset asleep?)\n"); cleanup_and_quit(); return; }
    int fd=xdp_session_open_pipewire_remote(session);
    start_pipewire(fd,node);
    g_signal_connect(session,"closed",G_CALLBACK(on_closed),NULL);
    g_timeout_add(16,frame_cb,NULL);
    printf("[vrmirror] mirroring. Stop from GNOME's top-bar share indicator.\n"); fflush(stdout);
}
static void on_created(GObject*src,GAsyncResult*res,gpointer u){
    (void)u; XdpPortal*portal=XDP_PORTAL(src); GError*err=NULL;
    XdpSession*session=xdp_portal_create_screencast_session_finish(portal,res,&err);
    if(!session){ fprintf(stderr,"[portal] create failed: %s\n",err?err->message:"?"); cleanup_and_quit(); return; }
    xdp_session_start(session,NULL,NULL,on_started,NULL);
}
static gboolean on_signal(gpointer u){ (void)u; cleanup_and_quit(); return G_SOURCE_REMOVE; }

int main(int argc,char**argv){
    if(argc>1) g_output=argv[1];
    if(getenv("VRMIRROR_TEST")) g_test=1;   /* other VRMIRROR_* knobs: apply_present_env() */
    g_mutex_init(&g_lock);
    g_loop=g_main_loop_new(NULL,FALSE);
    g_unix_signal_add(SIGINT,on_signal,NULL);
    g_unix_signal_add(SIGTERM,on_signal,NULL);
    if(g_test){
        /* diagnostic: no portal, just lease + draw the centered crosshair */
        g_drmfd=wl_lease_acquire();
        if(g_drmfd<0||present_up()<0){ fprintf(stderr,"[test] lease/present failed\n"); return 1; }
        g_timeout_add(16,frame_cb,NULL);
        printf("[test] crosshair on panel. Ctrl-C to stop.\n"); fflush(stdout);
    } else {
        XdpPortal*portal=xdp_portal_new();
        xdp_portal_create_screencast_session(portal,
            XDP_OUTPUT_MONITOR|XDP_OUTPUT_WINDOW,XDP_SCREENCAST_FLAG_NONE,
            XDP_CURSOR_MODE_HIDDEN,XDP_PERSIST_MODE_NONE,NULL,NULL,on_created,NULL);
    }
    g_main_loop_run(g_loop);

    /* teardown: always release the lease */
    if(g_pw){ pw_thread_loop_stop(g_pw); }
    if(g_head){ vr_head_close(g_head); g_head=NULL; }
    if(g_present){ vr_present_destroy(g_present); g_present=NULL; }
    if(g_drmfd>=0){ close(g_drmfd); g_drmfd=-1; }
    if(g_lease_obj) wp_drm_lease_v1_destroy(g_lease_obj);
    if(g_wl){ wl_display_flush(g_wl); wl_display_disconnect(g_wl); }
    printf("[vrmirror] exit (lease released)\n");
    return 0;
}
