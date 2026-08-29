/* vrpresent.c -- shared VR presenter for vrmirror. See vrpresent.h.
 *
 * Extracted from the atomic-KMS + stereo-detection path that grew up inside the
 * Wayland frontend, so both frontends share one scanout/compositor. The cursor
 * overlay is disabled on the leased CRTC here (that was the real cause of the
 * early flashing -- the flip mechanism was never at fault).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "vrpresent.h"

#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_PRIMARY 1
#endif

/* stereo detector tuning (verbatim from the Wayland frontend) */
#define DET_W 256
#define DET_H 144
#define DET_DISP 5
#define DETECT_EVERY 15     /* run the check every Nth presented frame (~4x/s) */
#define SWITCH_AFTER 4      /* consecutive agreeing checks before switching (~1s) */

struct fbuf { uint32_t handle, pitch, fb_id; uint64_t size; uint8_t *map; };

struct vr_present {
    int drmfd;                              /* borrowed, not owned */
    uint32_t conn_id, crtc_id, plane_id, mode_blob;
    drmModeModeInfo mode;
    struct fbuf fb[2];
    int front, flip_pending;
    /* atomic property ids */
    uint32_t P_fb,P_pcrtc,P_sx,P_sy,P_sw,P_sh,P_cx,P_cy,P_cw,P_ch,P_mode,P_active,P_conn_crtc;
    /* presentation controls */
    int fit; double zoom; int xoff, yoff; int stereo_force;
    /* per-axis head-tracking reach (px each way) from the last scaled frame: the
     * shift that brings a content edge to the viewport centre (= half the scaled
     * image). Tracks source-vs-eye aspect, so it follows window resizes. */
    int pan_x, pan_y;
    /* detection state */
    int smode, scand, sagree, detframe;
    uint8_t gray[DET_W*DET_H];
    /* scratch eye buffers */
    uint8_t *eyeL, *eyeR; size_t eyesz;
};

/* ============================= KMS bring-up ============================= */
static uint32_t prop_get(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val){
    drmModeObjectProperties *pr = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0; if(!pr) return 0;
    for(uint32_t i=0;i<pr->count_props;i++){
        drmModePropertyRes *p = drmModeGetProperty(fd, pr->props[i]);
        if(p){ if(!strcmp(p->name,name)){ id=p->prop_id; if(val)*val=pr->prop_values[i]; } drmModeFreeProperty(p); }
    }
    drmModeFreeObjectProperties(pr); return id;
}
static int mk_fb(int fd, int W, int H, struct fbuf *b){
    uint64_t off=0; memset(b,0,sizeof *b);
    if(drmModeCreateDumbBuffer(fd,W,H,32,0,&b->handle,&b->pitch,&b->size)) return -1;
    if(drmModeAddFB(fd,W,H,24,32,b->pitch,b->handle,&b->fb_id)) return -1;
    if(drmModeMapDumbBuffer(fd,b->handle,&off)) return -1;
    b->map = mmap(0,b->size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,off);
    if(b->map==MAP_FAILED) return -1;
    memset(b->map,0,b->size); return 0;
}
static int find_primary_plane(int fd, uint32_t *out){
    drmModePlaneRes *plr = drmModeGetPlaneResources(fd);
    if(!plr) return -1;
    uint32_t found=0;
    for(uint32_t i=0;i<plr->count_planes && !found;i++){
        drmModePlane *pl = drmModeGetPlane(fd, plr->planes[i]);
        if(!pl) continue;
        int on_crtc = (pl->possible_crtcs & 1u); uint32_t pid = pl->plane_id; drmModeFreePlane(pl);
        if(!on_crtc) continue;
        uint64_t type=0; prop_get(fd,pid,DRM_MODE_OBJECT_PLANE,"type",&type);
        if(type==DRM_PLANE_TYPE_PRIMARY) found=pid;
    }
    drmModeFreePlaneResources(plr);
    *out=found; return found?0:-1;
}
static void plane_full(vr_present *p, drmModeAtomicReq *req, uint32_t fb, int W, int H){
    drmModeAtomicAddProperty(req,p->plane_id,p->P_fb,fb);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_pcrtc,p->crtc_id);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_sx,0);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_sy,0);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_sw,(uint64_t)W<<16);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_sh,(uint64_t)H<<16);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_cx,0);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_cy,0);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_cw,W);
    drmModeAtomicAddProperty(req,p->plane_id,p->P_ch,H);
}
static int present_flip(vr_present *p, int back){
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req,p->plane_id,p->P_fb,p->fb[back].fb_id);
    int r = drmModeAtomicCommit(p->drmfd,req,DRM_MODE_ATOMIC_NONBLOCK|DRM_MODE_PAGE_FLIP_EVENT,p);
    drmModeAtomicFree(req);
    return r;
}
static void on_flip(int fd,unsigned seq,unsigned tv,unsigned tu,void *u){
    (void)fd;(void)seq;(void)tv;(void)tu;
    if(u) ((vr_present*)u)->flip_pending = 0;
}

vr_present *vr_present_create(int drmfd){
    vr_present *p = calloc(1,sizeof *p);
    if(!p) return NULL;
    p->drmfd = drmfd; p->fit = VR_FIT_COVER; p->zoom = 1.0; p->stereo_force = VR_STEREO_AUTO;
    p->smode = VR_STEREO_MONO; p->scand = VR_STEREO_MONO; p->sagree = SWITCH_AFTER;

    drmSetClientCap(drmfd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1);
    if(drmSetClientCap(drmfd,DRM_CLIENT_CAP_ATOMIC,1)){ fprintf(stderr,"[present] atomic cap unsupported\n"); goto fail; }
    drmModeRes *dr = drmModeGetResources(drmfd);
    if(!dr){ fprintf(stderr,"[present] GetResources NULL\n"); goto fail; }
    if(dr->count_connectors<1 || dr->count_crtcs<1){ drmModeFreeResources(dr); goto fail; }
    p->conn_id = dr->connectors[0]; p->crtc_id = dr->crtcs[0];
    drmModeConnector *c = drmModeGetConnector(drmfd,p->conn_id);
    if(!c || c->count_modes<1){ if(c)drmModeFreeConnector(c); drmModeFreeResources(dr); fprintf(stderr,"[present] no modes (panel asleep? wear the headset)\n"); goto fail; }
    int best=0;   /* largest area, highest refresh on ties (native 2880x1440@90) */
    for(int m=1;m<c->count_modes;m++){
        long a=(long)c->modes[m].hdisplay*c->modes[m].vdisplay, ab=(long)c->modes[best].hdisplay*c->modes[best].vdisplay;
        if(a>ab || (a==ab && c->modes[m].vrefresh>c->modes[best].vrefresh)) best=m;
    }
    p->mode = c->modes[best]; drmModeFreeConnector(c); drmModeFreeResources(dr);
    int W=p->mode.hdisplay, H=p->mode.vdisplay;
    if(mk_fb(drmfd,W,H,&p->fb[0]) || mk_fb(drmfd,W,H,&p->fb[1])){ fprintf(stderr,"[present] fb alloc failed\n"); goto fail; }
    if(find_primary_plane(drmfd,&p->plane_id)){ fprintf(stderr,"[present] no primary plane for crtc\n"); goto fail; }
    p->P_fb=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"FB_ID",0);
    p->P_pcrtc=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_ID",0);
    p->P_sx=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"SRC_X",0);
    p->P_sy=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"SRC_Y",0);
    p->P_sw=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"SRC_W",0);
    p->P_sh=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"SRC_H",0);
    p->P_cx=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_X",0);
    p->P_cy=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_Y",0);
    p->P_cw=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_W",0);
    p->P_ch=prop_get(drmfd,p->plane_id,DRM_MODE_OBJECT_PLANE,"CRTC_H",0);
    p->P_mode=prop_get(drmfd,p->crtc_id,DRM_MODE_OBJECT_CRTC,"MODE_ID",0);
    p->P_active=prop_get(drmfd,p->crtc_id,DRM_MODE_OBJECT_CRTC,"ACTIVE",0);
    p->P_conn_crtc=prop_get(drmfd,p->conn_id,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",0);
    if(!p->P_fb||!p->P_pcrtc||!p->P_mode||!p->P_active||!p->P_conn_crtc){ fprintf(stderr,"[present] missing atomic props\n"); goto fail; }
    drmModeCreatePropertyBlob(drmfd,&p->mode,sizeof p->mode,&p->mode_blob);
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req,p->conn_id,p->P_conn_crtc,p->crtc_id);
    drmModeAtomicAddProperty(req,p->crtc_id,p->P_mode,p->mode_blob);
    drmModeAtomicAddProperty(req,p->crtc_id,p->P_active,1);
    plane_full(p,req,p->fb[0].fb_id,W,H);
    int r = drmModeAtomicCommit(drmfd,req,DRM_MODE_ATOMIC_ALLOW_MODESET,NULL);
    drmModeAtomicFree(req);
    if(r){ fprintf(stderr,"[present] atomic modeset failed: %s\n",strerror(errno)); goto fail; }

    /* THE fix for the early flashing: kill any stray cursor overlay left on the
     * leased CRTC. Nothing else programs a cursor plane, so this stays off. */
    drmModeSetCursor(drmfd,p->crtc_id,0,0,0);

    p->front=0; p->flip_pending=0;
    printf("[present] panel %dx%d@%dHz lit (atomic, plane %u)\n",W,H,p->mode.vrefresh,p->plane_id); fflush(stdout);
    return p;
fail:
    vr_present_destroy(p);
    return NULL;
}

void vr_present_destroy(vr_present *p){
    if(!p) return;
    if(p->mode_blob) drmModeDestroyPropertyBlob(p->drmfd,p->mode_blob);
    for(int i=0;i<2;i++){
        if(p->fb[i].map && p->fb[i].map!=MAP_FAILED) munmap(p->fb[i].map,p->fb[i].size);
        if(p->fb[i].fb_id) drmModeRmFB(p->drmfd,p->fb[i].fb_id);
        if(p->fb[i].handle) drmModeDestroyDumbBuffer(p->drmfd,p->fb[i].handle);
    }
    free(p->eyeL); free(p->eyeR);
    free(p);
}

void vr_present_panel_size(const vr_present *p, int *w, int *h){ if(w)*w=p->mode.hdisplay; if(h)*h=p->mode.vdisplay; }
int  vr_present_refresh(const vr_present *p){ return p->mode.vrefresh; }
int  vr_present_fd(const vr_present *p){ return p->drmfd; }
int  vr_present_flip_pending(const vr_present *p){ return p->flip_pending; }
void vr_present_dispatch(vr_present *p){
    drmEventContext ev = { .version=2, .page_flip_handler=on_flip };
    drmHandleEvent(p->drmfd,&ev);
}
void vr_present_set_stereo(vr_present *p, int mode){ p->stereo_force = mode; }
void vr_present_set_fit(vr_present *p, int fit, double zoom){ p->fit=fit; p->zoom = zoom>0?zoom:1.0; }
void vr_present_set_offset(vr_present *p, int xoff, int yoff){ p->xoff=xoff; p->yoff=yoff; }
void vr_present_pan_range(const vr_present *p, int *rx, int *ry){ if(rx)*rx=p->pan_x; if(ry)*ry=p->pan_y; }

/* =============================== scaling ================================ */
static void scale_bgrx(vr_present *p, const uint8_t *src, int sw, int sh, int sstride,
                       uint8_t *dst, int dw, int dh, int dstride){
    double fx=(double)dw/sw, fy=(double)dh/sh;
    double sc = (p->fit==VR_FIT_COVER) ? (fx>fy?fx:fy) : (fx<fy?fx:fy);
    sc *= p->zoom;
    int ow=(int)(sw*sc), oh=(int)(sh*sc); if(ow<1)ow=1; if(oh<1)oh=1;
    /* head-tracking reach: how far to shift so a content edge lands at the
     * viewport centre. The image is centred at zero offset, its edges are half
     * its size away, so that shift is exactly half the scaled image on each axis
     * (= the aspect crop plus half the eye). Follows the window aspect + resize. */
    p->pan_x = ow/2;
    p->pan_y = oh/2;
    int offx=(dw-ow)/2+p->xoff, offy=(dh-oh)/2+p->yoff;
    memset(dst,0,(size_t)dstride*dh);
    for(int y=0;y<oh;y++){
        int dy=offy+y; if(dy<0||dy>=dh) continue;
        int sy=(int)(y/sc); if(sy>=sh)sy=sh-1;
        const uint8_t *sr=src+(size_t)sy*sstride;
        for(int x=0;x<ow;x++){
            int dx=offx+x; if(dx<0||dx>=dw) continue;
            int sx=(int)(x/sc); if(sx>=sw)sx=sw-1; const uint8_t *sp=sr+(size_t)sx*4;
            uint8_t *dr=dst+(size_t)dy*dstride+(size_t)dx*4;
            dr[0]=sp[0];dr[1]=sp[1];dr[2]=sp[2];dr[3]=0;
        }
    }
}

/* ===================== auto stereo-layout detection ==================== */
/* Two halves of a real stereo pair match (at a small parallax shift) but the
 * image is NOT self-repeating; a grid/text page also has matching halves but IS
 * self-repeating, so the periodicity check rejects it. No metadata, no toggle. */
static void downscale_gray(const uint8_t *src, int sw, int sh, int sstride, uint8_t *dst, int dw, int dh){
    for(int y=0;y<dh;y++){ int sy=y*sh/dh; const uint8_t *sr=src+(size_t)sy*sstride;
        for(int x=0;x<dw;x++){ int sx=x*sw/dw; const uint8_t *q=sr+(size_t)sx*4;
            dst[y*dw+x]=(uint8_t)((q[0]+q[1]+q[2])/3); } }
}
static double corr_span(const uint8_t *g,int w,int y0,int y1,int ax0,int bx0,int cols){
    double sa=0,sb=0,saa=0,sbb=0,sab=0; long n=0;
    for(int y=y0;y<y1;y++){ const uint8_t *r=g+(size_t)y*w;
        for(int i=0;i<cols;i++){ double a=r[ax0+i],b=r[bx0+i]; sa+=a;sb+=b;saa+=a*a;sbb+=b*b;sab+=a*b;n++; } }
    if(n==0) return 0;
    double ma=sa/n,mb=sb/n,va=saa/n-ma*ma,vb=sbb/n-mb*mb,cov=sab/n-ma*mb;
    if(va<1e-6||vb<1e-6) return 0;
    return cov/sqrt(va*vb);
}
static double match_lr(const uint8_t *g,int w,int y0,int y1){
    int hw=w/2, cols=hw-DET_DISP; if(cols<=0) return 0; double best=-1;
    for(int d=-DET_DISP; d<=DET_DISP; d++){ double c=corr_span(g,w,y0,y1,0,hw+d,cols); if(c>best)best=c; }
    return best;
}
static double match_tb(const uint8_t *g,int w,int y0,int y1){
    int mid=(y0+y1)/2, rows=mid-y0; if(rows<=0) return 0;
    double sa=0,sb=0,saa=0,sbb=0,sab=0; long n=0;
    for(int i=0;i<rows;i++){ const uint8_t *ra=g+(size_t)(y0+i)*w,*rb=g+(size_t)(mid+i)*w;
        for(int x=0;x<w;x++){ double a=ra[x],b=rb[x]; sa+=a;sb+=b;saa+=a*a;sbb+=b*b;sab+=a*b;n++; } }
    if(n==0) return 0;
    double ma=sa/n,mb=sb/n,va=saa/n-ma*ma,vb=sbb/n-mb*mb,cov=sab/n-ma*mb;
    if(va<1e-6||vb<1e-6) return 0;
    return cov/sqrt(va*vb);
}
static double self_rep(const uint8_t *g,int w,int y0,int y1){
    int hw=w/2; double best=0;
    for(int s=hw/4; s<hw/2; s+=2){ int cols=hw-s; if(cols<=0)continue;
        double c=corr_span(g,w,y0,y1,0,s,cols); if(c>best)best=c; }
    return best;
}
static void detect_scores(const uint8_t *g,int w,int h,double *lr,double *tb,double *rep){
    int y0=(int)(h*0.12), y1=h-y0;                 /* drop chrome/letterbox bands */
    *lr=match_lr(g,w,y0,y1); *tb=match_tb(g,w,y0,y1); *rep=self_rep(g,w,y0,y1);
}
/* hysteretic: high bar to ENTER a stereo mode, low bar to LEAVE it, so scores
 * hovering near the line don't flip-flop. Periodicity forces mono. */
static int decide_mode(int cur,double lr,double tb,double rep){
    if(cur==VR_STEREO_SBS) return (rep>0.75 || lr<0.55) ? VR_STEREO_MONO : VR_STEREO_SBS;
    if(cur==VR_STEREO_OU)  return (rep>0.75 || tb<0.55) ? VR_STEREO_MONO : VR_STEREO_OU;
    if(rep<0.6 && lr>0.72 && lr>tb+0.10) return VR_STEREO_SBS;
    if(rep<0.6 && tb>0.72 && tb>lr+0.10) return VR_STEREO_OU;
    return VR_STEREO_MONO;
}

/* =============================== compositing =========================== */
static int ensure_eyes(vr_present *p, size_t es, int H){
    if(p->eyesz >= es*(size_t)H) return 0;
    free(p->eyeL); free(p->eyeR);
    p->eyeL=malloc(es*H); p->eyeR=malloc(es*H); p->eyesz=es*H;
    return (p->eyeL && p->eyeR) ? 0 : -1;
}
/* blit prepared eye buffers into the back framebuffer and flip at vblank */
static void composite_and_flip(vr_present *p, const uint8_t *L, const uint8_t *R, int H, size_t es){
    int back = p->front ^ 1;
    uint8_t *base = p->fb[back].map; uint32_t pitch = p->fb[back].pitch;
    for(int y=0;y<H;y++){ uint8_t *d=base+(size_t)y*pitch;
        memcpy(d, L+(size_t)y*es, es); memcpy(d+es, R+(size_t)y*es, es); }
    if(present_flip(p,back)==0){ p->flip_pending=1; p->front=back; }
    else { static int warned=0; if(!warned){warned=1; fprintf(stderr,"[present] atomic flip failed: %s\n",strerror(errno)); } }
}

void vr_present_frame(vr_present *p, const uint8_t *bgrx, int w, int h, size_t stride){
    if(!p || p->flip_pending || !bgrx || w<1 || h<1) return;
    int W=p->mode.hdisplay, H=p->mode.vdisplay, half=W/2; size_t es=(size_t)half*4;
    if(ensure_eyes(p,es,H)) return;

    int do_detect = (p->stereo_force==VR_STEREO_AUTO) && ((++p->detframe % DETECT_EVERY)==0);
    if(do_detect) downscale_gray(bgrx,w,h,(int)stride,p->gray,DET_W,DET_H);

    int m = (p->stereo_force>=0) ? p->stereo_force : p->smode;
    const uint8_t *R = p->eyeL;             /* right eye = left unless stereo */
    if(m==VR_STEREO_SBS){
        int lw=w/2;
        scale_bgrx(p, bgrx,               lw,   h, (int)stride, p->eyeL, half,H,(int)es);
        scale_bgrx(p, bgrx+(size_t)lw*4,  w-lw, h, (int)stride, p->eyeR, half,H,(int)es);
        R=p->eyeR;
    } else if(m==VR_STEREO_OU){
        int th=h/2;
        scale_bgrx(p, bgrx,                    w, th,   (int)stride, p->eyeL, half,H,(int)es);
        scale_bgrx(p, bgrx+(size_t)th*stride,  w, h-th, (int)stride, p->eyeR, half,H,(int)es);
        R=p->eyeR;
    } else {
        scale_bgrx(p, bgrx, w, h, (int)stride, p->eyeL, half,H,(int)es);
    }
    composite_and_flip(p, p->eyeL, R, H, es);

    if(do_detect){
        double lr,tb,rep; detect_scores(p->gray,DET_W,DET_H,&lr,&tb,&rep);
        int nm = decide_mode(p->smode,lr,tb,rep);
        if(nm==p->scand) p->sagree++; else { p->scand=nm; p->sagree=1; }
        if(p->sagree>=SWITCH_AFTER && p->smode!=p->scand){
            p->smode=p->scand;
            fprintf(stderr,"[stereo] -> %s (lr=%.2f tb=%.2f rep=%.2f)\n",
                p->smode==VR_STEREO_SBS?"SBS":p->smode==VR_STEREO_OU?"OU":"2D",lr,tb,rep); fflush(stderr);
        }
    }
}

void vr_present_test_pattern(vr_present *p){
    if(!p || p->flip_pending) return;
    int W=p->mode.hdisplay, H=p->mode.vdisplay, half=W/2; size_t es=(size_t)half*4;
    if(ensure_eyes(p,es,H)) return;
    int cx=half/2+p->xoff, cy=H/2+p->yoff;
    for(int y=0;y<H;y++){ uint8_t *row=p->eyeL+(size_t)y*es;
        for(int x=0;x<half;x++){
            int on = (abs(x-cx)<3)||(abs(y-cy)<3)||(x<4||x>=half-4||y<4||y>=H-4);
            uint8_t *q=row+(size_t)x*4; uint8_t v=on?255:0; q[0]=v;q[1]=v;q[2]=v;q[3]=0;
        }
    }
    composite_and_flip(p, p->eyeL, p->eyeL, H, es);
}

void vr_present_blank(vr_present *p){
    if(!p || p->flip_pending) return;
    int back = p->front ^ 1;
    memset(p->fb[back].map, 0, p->fb[back].size);
    if(present_flip(p,back)==0){ p->flip_pending=1; p->front=back; }
}
