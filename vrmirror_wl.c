/* vrmirror_wl.c -- Wayland/GNOME counterpart of vrtray: mirror a desktop window
 * to the WMR headset. Launched from a desktop icon (no CLI, no window).
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
#include <poll.h>
#include <sys/mman.h>
#include <glib-unix.h>
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <math.h>
#include <wayland-client.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "drm-lease-v1-client-protocol.h"

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

/* ============================= KMS (atomic) ============================ */
/* Atomic modeset + vblank-synced page flips. Legacy drmModePageFlip is rejected
 * on mutter's leased CRTC (that drove the flashing/tearing); atomic commits work
 * and flip in sync with vblank, so no tearing. */
#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_PRIMARY 1
#endif
struct fbuf{uint32_t handle,pitch,fb_id;uint64_t size;uint8_t*map;};
static int g_drmfd=-1; static uint32_t g_conn_id,g_crtc_id; static drmModeModeInfo g_mode;
static struct fbuf g_fb[2];
static int g_xoff=0, g_yoff=0;   /* per-eye image shift (px); tune via env */
static int g_fill=1;             /* 1=cover(fill+crop), 0=fit(letterbox) */
static double g_zoom=1.0;        /* extra scale multiplier */
static int g_test=0;             /* draw a centered crosshair instead of capture */
static uint32_t g_plane_id, g_mode_blob;
static int g_kfront=0; static volatile int g_flip_pending=0;
static uint32_t P_fb,P_pcrtc,P_sx,P_sy,P_sw,P_sh,P_cx,P_cy,P_cw,P_ch,P_mode,P_active,P_conn_crtc;

static gboolean frame_cb(gpointer);   /* fwd: present newest frame */
static void on_flip(int f,unsigned s,unsigned tv,unsigned tu,void*u){(void)f;(void)s;(void)tv;(void)tu;(void)u; g_flip_pending=0;}

static uint32_t prop_get(uint32_t obj,uint32_t type,const char*name,uint64_t*val){
    drmModeObjectProperties*pr=drmModeObjectGetProperties(g_drmfd,obj,type);
    uint32_t id=0; if(!pr)return 0;
    for(uint32_t i=0;i<pr->count_props;i++){
        drmModePropertyRes*p=drmModeGetProperty(g_drmfd,pr->props[i]);
        if(p){ if(!strcmp(p->name,name)){ id=p->prop_id; if(val)*val=pr->prop_values[i]; } drmModeFreeProperty(p);} }
    drmModeFreeObjectProperties(pr); return id;
}
static int mk_fb(int W,int H,struct fbuf*b){
    uint64_t off=0; memset(b,0,sizeof*b);
    if(drmModeCreateDumbBuffer(g_drmfd,W,H,32,0,&b->handle,&b->pitch,&b->size))return -1;
    if(drmModeAddFB(g_drmfd,W,H,24,32,b->pitch,b->handle,&b->fb_id))return -1;
    if(drmModeMapDumbBuffer(g_drmfd,b->handle,&off))return -1;
    b->map=mmap(0,b->size,PROT_READ|PROT_WRITE,MAP_SHARED,g_drmfd,off);
    if(b->map==MAP_FAILED)return -1;
    memset(b->map,0,b->size); return 0;
}
static int find_primary_plane(void){
    drmModePlaneRes*plr=drmModeGetPlaneResources(g_drmfd);
    if(!plr)return -1;
    for(uint32_t i=0;i<plr->count_planes && !g_plane_id;i++){
        drmModePlane*pl=drmModeGetPlane(g_drmfd,plr->planes[i]);
        if(!pl)continue;
        int on_crtc=(pl->possible_crtcs & 1u); uint32_t pid=pl->plane_id; drmModeFreePlane(pl);
        if(!on_crtc)continue;
        uint64_t type=0; prop_get(pid,DRM_MODE_OBJECT_PLANE,"type",&type);
        if(type==DRM_PLANE_TYPE_PRIMARY) g_plane_id=pid;
    }
    drmModeFreePlaneResources(plr);
    return g_plane_id?0:-1;
}
static void plane_full(drmModeAtomicReq*req,uint32_t fb,int W,int H){
    drmModeAtomicAddProperty(req,g_plane_id,P_fb,fb);
    drmModeAtomicAddProperty(req,g_plane_id,P_pcrtc,g_crtc_id);
    drmModeAtomicAddProperty(req,g_plane_id,P_sx,0);
    drmModeAtomicAddProperty(req,g_plane_id,P_sy,0);
    drmModeAtomicAddProperty(req,g_plane_id,P_sw,(uint64_t)W<<16);
    drmModeAtomicAddProperty(req,g_plane_id,P_sh,(uint64_t)H<<16);
    drmModeAtomicAddProperty(req,g_plane_id,P_cx,0);
    drmModeAtomicAddProperty(req,g_plane_id,P_cy,0);
    drmModeAtomicAddProperty(req,g_plane_id,P_cw,W);
    drmModeAtomicAddProperty(req,g_plane_id,P_ch,H);
}
static int kms_up(void){
    drmSetClientCap(g_drmfd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1);
    if(drmSetClientCap(g_drmfd,DRM_CLIENT_CAP_ATOMIC,1)){ fprintf(stderr,"[kms] atomic cap unsupported\n"); return -1; }
    drmModeRes*dr=drmModeGetResources(g_drmfd);
    if(!dr){ fprintf(stderr,"[kms] GetResources NULL\n"); return -1; }
    if(dr->count_connectors<1||dr->count_crtcs<1){ drmModeFreeResources(dr); return -1; }
    g_conn_id=dr->connectors[0]; g_crtc_id=dr->crtcs[0];
    drmModeConnector*c=drmModeGetConnector(g_drmfd,g_conn_id);
    if(!c||c->count_modes<1){ if(c)drmModeFreeConnector(c); drmModeFreeResources(dr); fprintf(stderr,"[kms] no modes\n"); return -1; }
    int best=0;
    for(int m=1;m<c->count_modes;m++){
        long a=(long)c->modes[m].hdisplay*c->modes[m].vdisplay, ab=(long)c->modes[best].hdisplay*c->modes[best].vdisplay;
        if(a>ab||(a==ab&&c->modes[m].vrefresh>c->modes[best].vrefresh))best=m;
    }
    g_mode=c->modes[best]; drmModeFreeConnector(c); drmModeFreeResources(dr);
    int W=g_mode.hdisplay,H=g_mode.vdisplay;
    if(mk_fb(W,H,&g_fb[0])||mk_fb(W,H,&g_fb[1])){ fprintf(stderr,"[kms] fb alloc failed\n"); return -1; }
    if(find_primary_plane()){ fprintf(stderr,"[kms] no primary plane for crtc\n"); return -1; }
    P_fb=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"FB_ID",0);
    P_pcrtc=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_ID",0);
    P_sx=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"SRC_X",0);
    P_sy=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"SRC_Y",0);
    P_sw=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"SRC_W",0);
    P_sh=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"SRC_H",0);
    P_cx=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_X",0);
    P_cy=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_Y",0);
    P_cw=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_W",0);
    P_ch=prop_get(g_plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_H",0);
    P_mode=prop_get(g_crtc_id,DRM_MODE_OBJECT_CRTC,"MODE_ID",0);
    P_active=prop_get(g_crtc_id,DRM_MODE_OBJECT_CRTC,"ACTIVE",0);
    P_conn_crtc=prop_get(g_conn_id,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",0);
    if(!P_fb||!P_pcrtc||!P_mode||!P_active||!P_conn_crtc){ fprintf(stderr,"[kms] missing atomic props\n"); return -1; }
    drmModeCreatePropertyBlob(g_drmfd,&g_mode,sizeof g_mode,&g_mode_blob);
    drmModeAtomicReq*req=drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req,g_conn_id,P_conn_crtc,g_crtc_id);
    drmModeAtomicAddProperty(req,g_crtc_id,P_mode,g_mode_blob);
    drmModeAtomicAddProperty(req,g_crtc_id,P_active,1);
    plane_full(req,g_fb[0].fb_id,W,H);
    int r=drmModeAtomicCommit(g_drmfd,req,DRM_MODE_ATOMIC_ALLOW_MODESET,NULL);
    drmModeAtomicFree(req);
    if(r){ fprintf(stderr,"[kms] atomic modeset failed: %s\n",strerror(errno)); return -1; }
    g_kfront=0; g_flip_pending=0;
    printf("[kms] panel %dx%d@%dHz lit (atomic, plane %u)\n",W,H,g_mode.vrefresh,g_plane_id); fflush(stdout);
    return 0;
}
static int present_flip(int back){
    drmModeAtomicReq*req=drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req,g_plane_id,P_fb,g_fb[back].fb_id);
    int r=drmModeAtomicCommit(g_drmfd,req,DRM_MODE_ATOMIC_NONBLOCK|DRM_MODE_PAGE_FLIP_EVENT,NULL);
    drmModeAtomicFree(req);
    return r;
}
static void kms_down(void){
    if(g_mode_blob){ drmModeDestroyPropertyBlob(g_drmfd,g_mode_blob); g_mode_blob=0; }
    for(int i=0;i<2;i++){
        if(g_fb[i].map&&g_fb[i].map!=MAP_FAILED)munmap(g_fb[i].map,g_fb[i].size);
        if(g_fb[i].fb_id)drmModeRmFB(g_drmfd,g_fb[i].fb_id);
        if(g_fb[i].handle)drmModeDestroyDumbBuffer(g_drmfd,g_fb[i].handle);
        memset(&g_fb[i],0,sizeof g_fb[i]);
    }
}
/* dispatch page-flip completion events (registered on the GLib loop) */
static gboolean drm_ev_cb(GIOChannel*ch,GIOCondition cond,gpointer u){
    (void)ch;(void)cond;(void)u;
    drmEventContext ev={.version=2,.page_flip_handler=on_flip};
    drmHandleEvent(g_drmfd,&ev);   /* clears g_flip_pending */
    frame_cb(NULL);                /* immediately present newest at this vblank */
    return G_SOURCE_CONTINUE;
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
static void scale_bgrx(uint8_t*src,int sw,int sh,int sstride,uint8_t*dst,int dw,int dh,int dstride){
    double fx=(double)dw/sw, fy=(double)dh/sh;
    double sc = g_fill ? (fx>fy?fx:fy) : (fx<fy?fx:fy);   /* cover vs fit */
    sc *= g_zoom;
    int ow=(int)(sw*sc),oh=(int)(sh*sc); if(ow<1)ow=1; if(oh<1)oh=1;
    int offx=(dw-ow)/2+g_xoff, offy=(dh-oh)/2+g_yoff;   /* env-tunable shift */
    memset(dst,0,(size_t)dstride*dh);
    for(int y=0;y<oh;y++){
        int dy=offy+y; if(dy<0||dy>=dh) continue;
        int sy=(int)(y/sc); if(sy>=sh)sy=sh-1;
        uint8_t*sr=src+(size_t)sy*sstride;
        for(int x=0;x<ow;x++){
            int dx=offx+x; if(dx<0||dx>=dw) continue;
            int sx=(int)(x/sc); if(sx>=sw)sx=sw-1; uint8_t*sp=sr+(size_t)sx*4;
            uint8_t*dr=dst+(size_t)dy*dstride+(size_t)dx*4;
            dr[0]=sp[0];dr[1]=sp[1];dr[2]=sp[2];dr[3]=0;
        }
    }
}
/* =================== auto stereo-layout detection ====================== */
/* Decide mono / side-by-side / over-under from the frame itself: the two halves
 * of a stereo pair match (at a small parallax shift) but the image is NOT self-
 * repeating; a grid or text page also has matching halves but IS self-repeating,
 * so the periodicity check rejects it. No metadata, no manual toggle. */
enum { STEREO_MONO=0, STEREO_SBS=1, STEREO_OU=2 };
#define DET_W 256
#define DET_H 144
#define DET_DISP 5
#define DETECT_EVERY 15     /* run the check every Nth presented frame (~4x/s) */
#define SWITCH_AFTER 4      /* consecutive agreeing checks before switching (~1s) */
static int g_smode=STEREO_MONO, g_scand=STEREO_MONO, g_sagree=SWITCH_AFTER, g_detframe=0;
static int g_force=-1;                 /* -1 auto; else forced mode */
static uint8_t g_gray[DET_W*DET_H];

static void downscale_gray(uint8_t*src,int sw,int sh,int sstride,uint8_t*dst,int dw,int dh){
    for(int y=0;y<dh;y++){ int sy=y*sh/dh; uint8_t*sr=src+(size_t)sy*sstride;
        for(int x=0;x<dw;x++){ int sx=x*sw/dw; uint8_t*p=sr+(size_t)sx*4;
            dst[y*dw+x]=(uint8_t)((p[0]+p[1]+p[2])/3); } }
}
/* normalized cross-correlation of two equal horizontal spans, row-wise */
static double corr_span(const uint8_t*g,int w,int y0,int y1,int ax0,int bx0,int cols){
    double sa=0,sb=0,saa=0,sbb=0,sab=0; long n=0;
    for(int y=y0;y<y1;y++){ const uint8_t*r=g+(size_t)y*w;
        for(int i=0;i<cols;i++){ double a=r[ax0+i],b=r[bx0+i]; sa+=a;sb+=b;saa+=a*a;sbb+=b*b;sab+=a*b;n++; } }
    if(n==0) return 0; double ma=sa/n,mb=sb/n,va=saa/n-ma*ma,vb=sbb/n-mb*mb,cov=sab/n-ma*mb;
    if(va<1e-6||vb<1e-6) return 0; return cov/sqrt(va*vb);
}
static double match_lr(const uint8_t*g,int w,int y0,int y1){
    int hw=w/2, cols=hw-DET_DISP; if(cols<=0) return 0; double best=-1;
    for(int d=-DET_DISP; d<=DET_DISP; d++){ double c=corr_span(g,w,y0,y1,0,hw+d,cols); if(c>best)best=c; }
    return best;
}
static double match_tb(const uint8_t*g,int w,int y0,int y1){
    int mid=(y0+y1)/2, rows=mid-y0; if(rows<=0) return 0;
    double sa=0,sb=0,saa=0,sbb=0,sab=0; long n=0;
    for(int i=0;i<rows;i++){ const uint8_t*ra=g+(size_t)(y0+i)*w,*rb=g+(size_t)(mid+i)*w;
        for(int x=0;x<w;x++){ double a=ra[x],b=rb[x]; sa+=a;sb+=b;saa+=a*a;sbb+=b*b;sab+=a*b;n++; } }
    if(n==0) return 0; double ma=sa/n,mb=sb/n,va=saa/n-ma*ma,vb=sbb/n-mb*mb,cov=sab/n-ma*mb;
    if(va<1e-6||vb<1e-6) return 0; return cov/sqrt(va*vb);
}
static double self_rep(const uint8_t*g,int w,int y0,int y1){
    int hw=w/2; double best=0;
    for(int s=hw/4; s<hw/2; s+=2){ int cols=hw-s; if(cols<=0)continue;
        double c=corr_span(g,w,y0,y1,0,s,cols); if(c>best)best=c; }
    return best;
}
static void detect_scores(const uint8_t*g,int w,int h,double*lr,double*tb,double*rep){
    int y0=(int)(h*0.12), y1=h-y0;                 /* drop chrome/letterbox bands */
    *lr=match_lr(g,w,y0,y1); *tb=match_tb(g,w,y0,y1); *rep=self_rep(g,w,y0,y1);
}
/* hysteretic decision: high bar to ENTER a stereo mode, low bar to LEAVE it, so
 * scores hovering near the line don't cause flip-flopping. Periodicity (grids,
 * text) always forces mono. `cur` is the currently-committed mode. */
static int decide_mode(int cur,double lr,double tb,double rep){
    if(cur==STEREO_SBS) return (rep>0.75 || lr<0.55) ? STEREO_MONO : STEREO_SBS;
    if(cur==STEREO_OU)  return (rep>0.75 || tb<0.55) ? STEREO_MONO : STEREO_OU;
    if(rep<0.6 && lr>0.72 && lr>tb+0.10) return STEREO_SBS;   /* enter from mono */
    if(rep<0.6 && tb>0.72 && tb>lr+0.10) return STEREO_OU;
    return STEREO_MONO;
}

/* dead-center crosshair + border, to separate our centering from lens optics */
static void draw_test(uint8_t*eye,int dw,int dh,size_t stride){
    memset(eye,0,stride*dh);
    int cx=dw/2+g_xoff, cy=dh/2+g_yoff;
    for(int y=0;y<dh;y++){ uint8_t*row=eye+(size_t)y*stride;
        for(int x=0;x<dw;x++){
            int w = (abs(x-cx)<3)||(abs(y-cy)<3)||(x<4||x>=dw-4||y<4||y>=dh-4);
            uint8_t*p=row+(size_t)x*4; uint8_t v=w?255:0; p[0]=v;p[1]=v;p[2]=v;p[3]=0;
        }
    }
}
static gboolean frame_cb(gpointer u){
    (void)u;
    if(g_drmfd<0||g_flip_pending) return G_SOURCE_CONTINUE;  /* wait for last flip */
    int W=g_mode.hdisplay,H=g_mode.vdisplay,half=W/2; size_t es=(size_t)half*4;
    static uint8_t*eyeL=NULL,*eyeR=NULL; static size_t esz=0;
    if(esz<es*H){ free(eyeL);free(eyeR); eyeL=malloc(es*H); eyeR=malloc(es*H); esz=es*H; }
    uint8_t*R=eyeL;                       /* right eye = left unless stereo */
    int do_detect=0;
    if(g_test){
        draw_test(eyeL,half,H,es);
    } else {
        g_mutex_lock(&g_lock);
        if(!g_have){ g_mutex_unlock(&g_lock); return G_SOURCE_CONTINUE; }
        do_detect = (g_force<0) && ((++g_detframe % DETECT_EVERY)==0);
        if(do_detect) downscale_gray(g_latest,g_lw,g_lh,g_lw*4,g_gray,DET_W,DET_H);
        int m = (g_force>=0)? g_force : g_smode;
        if(m==STEREO_SBS){                /* left half -> left eye, right half -> right eye */
            int lw=g_lw/2;
            scale_bgrx(g_latest, lw, g_lh, g_lw*4, eyeL, half,H,(int)es);
            scale_bgrx(g_latest+(size_t)lw*4, g_lw-lw, g_lh, g_lw*4, eyeR, half,H,(int)es);
            R=eyeR;
        } else if(m==STEREO_OU){          /* top half -> left eye, bottom half -> right eye */
            int th=g_lh/2;
            scale_bgrx(g_latest, g_lw, th, g_lw*4, eyeL, half,H,(int)es);
            scale_bgrx(g_latest+(size_t)th*g_lw*4, g_lw, g_lh-th, g_lw*4, eyeR, half,H,(int)es);
            R=eyeR;
        } else {                          /* mono: same image to both eyes */
            scale_bgrx(g_latest,g_lw,g_lh,g_lw*4,eyeL,half,H,(int)es);
        }
        g_mutex_unlock(&g_lock);
    }
    /* draw into the BACK buffer, then atomically flip it in at vblank (tear-free) */
    int back=g_kfront^1;
    uint8_t*base=g_fb[back].map; uint32_t pitch=g_fb[back].pitch;
    for(int y=0;y<H;y++){ uint8_t*dd=base+(size_t)y*pitch;
        memcpy(dd,eyeL+(size_t)y*es,es); memcpy(dd+es,R+(size_t)y*es,es); }
    if(present_flip(back)==0){ g_flip_pending=1; g_kfront=back; }
    else { static int warned=0; if(!warned){warned=1; fprintf(stderr,"[kms] atomic flip failed: %s\n",strerror(errno));} }
    /* run detection outside the lock; switch mode after SWITCH_AFTER agreeing checks */
    if(do_detect){
        double lr,tb,rep; detect_scores(g_gray,DET_W,DET_H,&lr,&tb,&rep);
        int m=decide_mode(g_smode,lr,tb,rep);
        if(m==g_scand) g_sagree++; else { g_scand=m; g_sagree=1; }
        if(g_sagree>=SWITCH_AFTER && g_smode!=g_scand){
            g_smode=g_scand;
            fprintf(stderr,"[stereo] -> %s (lr=%.2f tb=%.2f rep=%.2f)\n",
                g_smode==STEREO_SBS?"SBS":g_smode==STEREO_OU?"OU":"2D",lr,tb,rep); fflush(stderr);
        }
    }
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
    if(g_drmfd<0||kms_up()<0){ fprintf(stderr,"[wl] lease/kms failed (headset asleep?)\n"); cleanup_and_quit(); return; }
    GIOChannel*drmch=g_io_channel_unix_new(g_drmfd);   /* deliver flip-complete events */
    g_io_add_watch(drmch,G_IO_IN,drm_ev_cb,NULL);
    g_io_channel_unref(drmch);
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
    if(getenv("VRMIRROR_XOFF")) g_xoff=atoi(getenv("VRMIRROR_XOFF"));
    if(getenv("VRMIRROR_YOFF")) g_yoff=atoi(getenv("VRMIRROR_YOFF"));
    if(getenv("VRMIRROR_FIT"))  g_fill=0;
    if(getenv("VRMIRROR_ZOOM")) g_zoom=atof(getenv("VRMIRROR_ZOOM"));
    if(getenv("VRMIRROR_TEST")) g_test=1;
    { const char*sm=getenv("VRMIRROR_STEREO");   /* force layout; default auto */
      if(sm){ if(!strcmp(sm,"sbs"))g_force=STEREO_SBS; else if(!strcmp(sm,"ou"))g_force=STEREO_OU;
              else if(!strcmp(sm,"mono")||!strcmp(sm,"2d"))g_force=STEREO_MONO; } }
    g_mutex_init(&g_lock);
    g_loop=g_main_loop_new(NULL,FALSE);
    g_unix_signal_add(SIGINT,on_signal,NULL);
    g_unix_signal_add(SIGTERM,on_signal,NULL);
    if(g_test){
        /* diagnostic: no portal, just lease + draw the centered crosshair */
        g_drmfd=wl_lease_acquire();
        if(g_drmfd<0||kms_up()<0){ fprintf(stderr,"[test] lease/kms failed\n"); return 1; }
        GIOChannel*drmch=g_io_channel_unix_new(g_drmfd);
        g_io_add_watch(drmch,G_IO_IN,drm_ev_cb,NULL); g_io_channel_unref(drmch);
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
    if(g_drmfd>=0){ kms_down(); }
    if(g_lease_obj) wp_drm_lease_v1_destroy(g_lease_obj);
    if(g_wl){ wl_display_flush(g_wl); wl_display_disconnect(g_wl); }
    printf("[vrmirror] exit (lease released)\n");
    return 0;
}
