#ifdef __PS2__

#undef client
#include <gsKit.h>
#include <gsInline.h>

// ps2.yaml remaps the final full-canvas sprite call to the modern-UI compositor. This file also
// needs the real gsKit sprite primitive for the dedicated viewport-overlay texture.
#ifdef gsKit_prim_sprite_texture_3d
#undef gsKit_prim_sprite_texture_3d
#endif
extern void gsKit_prim_sprite_texture_3d(GSGLOBAL *gsGlobal, const GSTEXTURE *Texture,
                                         float x1, float y1, int iz1, float u1, float v1,
                                         float x2, float y2, int iz2, float u2, float v2,
                                         u64 color);

#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "pix3d.h"
#include "ps2_gs_raster.h"

#define PS2_GS_RUNTIME_CODE __attribute__((section(".ps2_runtime_text"), noinline))
#define PS2_GS_RUNTIME_DATA __attribute__((section(".ps2_runtime_data"), used, aligned(16)))

#define PS2_GS_TRI_CAPACITY 4096
#define PS2_GS_STATE_MAGIC 0x47535231u
#define PS2_GS_TEXTURE_COUNT 50
#define PS2_GS_TEXTURE_SIZE 64
#define PS2_GS_TEXTURE_BATCH_TRIANGLES 32
#define PS2_GS_TEXTURE_BATCH_VERTICES (PS2_GS_TEXTURE_BATCH_TRIANGLES * 3)
#define PS2_GS_UNTEXTURED_BATCH_TRIANGLES 32
#define PS2_GS_UNTEXTURED_BATCH_VERTICES (PS2_GS_UNTEXTURED_BATCH_TRIANGLES * 3)
#define PS2_GS_TRI_FLAT 0
#define PS2_GS_TRI_GOURAUD 1
#define PS2_GS_TRI_TEXTURED 2

typedef struct {
    int16_t x1, y1, x2, y2, x3, y3;
    uint8_t kind, alpha, texture_id, shade1, shade2, shade3;
    uint16_t reserved;
    uint32_t color1, color2, color3;
    float s1, t1, q1, s2, t2, q2, s3, t3, q3;
} Ps2GsTriangle;

typedef struct {
    uint32_t magic;
    Ps2GsTriangle *triangles;
    GSTEXTURE *textures;
    uint16_t *texture_upload;
    GSPRIMSTQPOINT *texture_batch;
    GSPRIMPOINT *untextured_batch;
#if PS2_GS_DIRECT_VIEWPORT_TEST
    GSTEXTURE *viewport_overlay_texture;
    uint16_t *viewport_overlay_upload;
    bool viewport_overlay_dirty;
#endif
    uint64_t texture_allocated_mask;
    uint64_t texture_uploaded_mask;
    int count;
    int overflow;
} Ps2GsRasterState;

static Ps2GsRasterState ps2_gs_state PS2_GS_RUNTIME_DATA = {
    .magic = PS2_GS_STATE_MAGIC
};

extern Pix3D _Pix3D;

PS2_GS_RUNTIME_CODE
static int16_t ps2_gs_coord(int value) {
    if(value < -8192)return -8192;
    if(value > 8191)return 8191;
    return (int16_t)value;
}

PS2_GS_RUNTIME_CODE
static uint32_t ps2_gs_palette_rgb(int color) {
    if(color<0)color=0;
    if(color>65535)color=65535;
    return (uint32_t)_Pix3D.palette[color]&0x00ffffffU;
}

PS2_GS_RUNTIME_CODE
static void ps2_gs_reset_state(void) {
    ps2_gs_state.magic=PS2_GS_STATE_MAGIC;
    ps2_gs_state.triangles=NULL;
    ps2_gs_state.textures=NULL;
    ps2_gs_state.texture_upload=NULL;
    ps2_gs_state.texture_batch=NULL;
    ps2_gs_state.untextured_batch=NULL;
#if PS2_GS_DIRECT_VIEWPORT_TEST
    ps2_gs_state.viewport_overlay_texture=NULL;
    ps2_gs_state.viewport_overlay_upload=NULL;
    ps2_gs_state.viewport_overlay_dirty=false;
#endif
    ps2_gs_state.texture_allocated_mask=0;
    ps2_gs_state.texture_uploaded_mask=0;
    ps2_gs_state.count=0;
    ps2_gs_state.overflow=0;
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_ensure_queue(void) {
    if(ps2_gs_state.magic!=PS2_GS_STATE_MAGIC)ps2_gs_reset_state();
    if(!ps2_gs_state.triangles) {
        ps2_gs_state.triangles=malloc(sizeof(Ps2GsTriangle)*PS2_GS_TRI_CAPACITY);
        if(!ps2_gs_state.triangles)return false;
    }
    return true;
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_ensure_texture_cache(void) {
    if(ps2_gs_state.magic!=PS2_GS_STATE_MAGIC)ps2_gs_reset_state();
    if(!ps2_gs_state.textures) {
        ps2_gs_state.textures=calloc(PS2_GS_TEXTURE_COUNT,sizeof(GSTEXTURE));
        if(!ps2_gs_state.textures)return false;
    }
    if(!ps2_gs_state.texture_upload) {
        ps2_gs_state.texture_upload=memalign(128,gsKit_texture_size(PS2_GS_TEXTURE_SIZE,PS2_GS_TEXTURE_SIZE,GS_PSM_CT16));
        if(!ps2_gs_state.texture_upload)return false;
    }
    return true;
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_ensure_texture_batch(void) {
#if PS2_GS_TEXTURE_BATCH_TEST
    if (ps2_gs_state.magic != PS2_GS_STATE_MAGIC) ps2_gs_reset_state();
    if (!ps2_gs_state.texture_batch) {
        ps2_gs_state.texture_batch = memalign(
            16, sizeof(GSPRIMSTQPOINT) * PS2_GS_TEXTURE_BATCH_VERTICES);
        if (!ps2_gs_state.texture_batch) return false;
    }
    return true;
#else
    return false;
#endif
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_ensure_untextured_batch(void) {
#if PS2_GS_UNTEXTURED_BATCH_TEST
    if (ps2_gs_state.magic != PS2_GS_STATE_MAGIC) ps2_gs_reset_state();
    if (!ps2_gs_state.untextured_batch) {
        ps2_gs_state.untextured_batch = memalign(
            16, sizeof(GSPRIMPOINT) * PS2_GS_UNTEXTURED_BATCH_VERTICES);
        if (!ps2_gs_state.untextured_batch) return false;
    }
    return true;
#else
    return false;
#endif
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_push(int x1,int y1,int x2,int y2,int x3,int y3,
                        uint32_t c1,uint32_t c2,uint32_t c3,int alpha,int kind) {
    int area=(x2-x1)*(y3-y1)-(y2-y1)*(x3-x1);
    if(area==0)return true;
    if(!ps2_gs_ensure_queue())return false;
    if(ps2_gs_state.count>=PS2_GS_TRI_CAPACITY){ps2_gs_state.overflow++;return false;}

    Ps2GsTriangle *tri=&ps2_gs_state.triangles[ps2_gs_state.count++];
    memset(tri,0,sizeof(*tri));
    tri->x1=ps2_gs_coord(x1);tri->y1=ps2_gs_coord(y1);
    tri->x2=ps2_gs_coord(x2);tri->y2=ps2_gs_coord(y2);
    tri->x3=ps2_gs_coord(x3);tri->y3=ps2_gs_coord(y3);
    tri->kind=(uint8_t)kind;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    tri->alpha=(uint8_t)alpha;
    tri->texture_id=0xff;
    tri->color1=c1&0x00ffffffU;tri->color2=c2&0x00ffffffU;tri->color3=c3&0x00ffffffU;
    return true;
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_queue_flat(int x1,int y1,int x2,int y2,int x3,int y3,int rgb,int alpha) {
    uint32_t c=(uint32_t)rgb&0x00ffffffU;
    return ps2_gs_push(x1,y1,x2,y2,x3,y3,c,c,c,alpha,PS2_GS_TRI_FLAT);
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_queue_gouraud(int x1,int y1,int x2,int y2,int x3,int y3,
                                 int c1,int c2,int c3,int alpha) {
    return ps2_gs_push(x1,y1,x2,y2,x3,y3,ps2_gs_palette_rgb(c1),ps2_gs_palette_rgb(c2),
                       ps2_gs_palette_rgb(c3),alpha,PS2_GS_TRI_GOURAUD);
}

PS2_GS_RUNTIME_CODE
static int32_t ps2_gs_plane_at(int base,int stride,int step_y,int x,int y,int cx,int cy) {
    uint32_t value=(uint32_t)base;
    value+=(uint32_t)(stride>>3)*(uint32_t)(x-cx);
    value+=(uint32_t)step_y*(uint32_t)(y-cy);
    return (int32_t)value;
}

PS2_GS_RUNTIME_CODE
static uint8_t ps2_gs_clamp_shade(int shade) {
    if (shade < 0) return 0;
    if (shade > 127) return 127;
    return (uint8_t)shade;
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_queue_textured(
    int x1,int y1,int x2,int y2,int x3,int y3,
    int shade1,int shade2,int shade3,
    int u,int u_stride,int u_step_y,
    int v,int v_stride,int v_step_y,
    int w,int w_stride,int w_step_y,
    int center_x,int center_y,
    int texture_id,int fallback_rgb,int alpha) {
    if(texture_id<0||texture_id>=PS2_GS_TEXTURE_COUNT)return false;
    int area=(x2-x1)*(y3-y1)-(y2-y1)*(x3-x1);
    if(area==0)return true;
    if(!ps2_gs_ensure_queue())return false;
    if(ps2_gs_state.count>=PS2_GS_TRI_CAPACITY){ps2_gs_state.overflow++;return false;}

    int32_t up1=ps2_gs_plane_at(u,u_stride,u_step_y,x1,y1,center_x,center_y);
    int32_t vp1=ps2_gs_plane_at(v,v_stride,v_step_y,x1,y1,center_x,center_y);
    int32_t wp1=ps2_gs_plane_at(w,w_stride,w_step_y,x1,y1,center_x,center_y);
    int32_t up2=ps2_gs_plane_at(u,u_stride,u_step_y,x2,y2,center_x,center_y);
    int32_t vp2=ps2_gs_plane_at(v,v_stride,v_step_y,x2,y2,center_x,center_y);
    int32_t wp2=ps2_gs_plane_at(w,w_stride,w_step_y,x2,y2,center_x,center_y);
    int32_t up3=ps2_gs_plane_at(u,u_stride,u_step_y,x3,y3,center_x,center_y);
    int32_t vp3=ps2_gs_plane_at(v,v_stride,v_step_y,x3,y3,center_x,center_y);
    int32_t wp3=ps2_gs_plane_at(w,w_stride,w_step_y,x3,y3,center_x,center_y);

    // In low-memory textureRaster the normalized coordinates are U/W and V/W: the later *64 is
    // only the conversion from normalized coordinate to a 64x64 texel index. GS ST expects that
    // normalized coordinate directly and RGBAQ.Q supplies the homogeneous divisor.
    float s1=(float)up1,t1=(float)vp1,q1=(float)wp1;
    float s2=(float)up2,t2=(float)vp2,q2=(float)wp2;
    float s3=(float)up3,t3=(float)vp3,q3=(float)wp3;

    bool qp=q1>0.0f&&q2>0.0f&&q3>0.0f;
    bool qn=q1<0.0f&&q2<0.0f&&q3<0.0f;
    if(!qp&&!qn)return false;
    if(qn){s1=-s1;t1=-t1;q1=-q1;s2=-s2;t2=-t2;q2=-q2;s3=-s3;t3=-t3;q3=-q3;}

    float m=fabsf(s1);
    if (fabsf(t1) > m) m = fabsf(t1);
    if (fabsf(q1) > m) m = fabsf(q1);
    if (fabsf(s2) > m) m = fabsf(s2);
    if (fabsf(t2) > m) m = fabsf(t2);
    if (fabsf(q2) > m) m = fabsf(q2);
    if (fabsf(s3) > m) m = fabsf(s3);
    if (fabsf(t3) > m) m = fabsf(t3);
    if (fabsf(q3) > m) m = fabsf(q3);
    if(m<1.0f)return false;
    float scale=1.0f/m;

    Ps2GsTriangle *tri=&ps2_gs_state.triangles[ps2_gs_state.count++];
    memset(tri,0,sizeof(*tri));
    tri->x1=ps2_gs_coord(x1);tri->y1=ps2_gs_coord(y1);
    tri->x2=ps2_gs_coord(x2);tri->y2=ps2_gs_coord(y2);
    tri->x3=ps2_gs_coord(x3);tri->y3=ps2_gs_coord(y3);
    tri->kind=PS2_GS_TRI_TEXTURED;
    if(alpha<0)alpha=0;if(alpha>255)alpha=255;
    tri->alpha=(uint8_t)alpha;tri->texture_id=(uint8_t)texture_id;
    tri->shade1=ps2_gs_clamp_shade(shade1);tri->shade2=ps2_gs_clamp_shade(shade2);tri->shade3=ps2_gs_clamp_shade(shade3);
    tri->color1=(uint32_t)fallback_rgb&0x00ffffffU;tri->color2=tri->color1;tri->color3=tri->color1;
    tri->s1=s1*scale;tri->t1=t1*scale;tri->q1=q1*scale;
    tri->s2=s2*scale;tri->t2=t2*scale;tri->q2=q2*scale;
    tri->s3=s3*scale;tri->t3=t3*scale;tri->q3=q3*scale;
    return true;
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_invalidate_texture(int texture_id) {
    if(texture_id<0||texture_id>=PS2_GS_TEXTURE_COUNT||ps2_gs_state.magic!=PS2_GS_STATE_MAGIC)return;
    ps2_gs_state.texture_uploaded_mask&=~(1ULL<<texture_id);
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_has_pending(void) {
    return ps2_gs_state.magic==PS2_GS_STATE_MAGIC&&ps2_gs_state.count>0;
}

PS2_GS_RUNTIME_CODE
static uint8_t ps2_gs_alpha_value(uint8_t rs_alpha) {
    int opacity=256-(int)rs_alpha;
    int a=(opacity*0x80+127)>>8;
    if (a < 0) a = 0;
    if (a > 0x80) a = 0x80;
    return (uint8_t)a;
}

PS2_GS_RUNTIME_CODE
static u64 ps2_gs_color(uint32_t rgb,uint8_t alpha) {
    return GS_SETREG_RGBAQ((rgb>>16)&0xff,(rgb>>8)&0xff,rgb&0xff,ps2_gs_alpha_value(alpha),0x00);
}

PS2_GS_RUNTIME_CODE
static uint8_t ps2_gs_texture_light(uint8_t shade) {
    int band=shade>>4;
    if(band<4)return (uint8_t)(128-band*16);
    return (uint8_t)(64-(band-4)*8);
}

PS2_GS_RUNTIME_CODE
static GSTEXTURE *ps2_gs_prepare_texture(GSGLOBAL *gs,int id) {
    if(!gs||id<0||id>=PS2_GS_TEXTURE_COUNT||id>=_Pix3D.textureCount||
       !_Pix3D.textures||!_Pix3D.texturePalettes||!_Pix3D.textures[id]||!_Pix3D.texturePalettes[id])return NULL;
    Pix8 *src=_Pix3D.textures[id];
    if(src->width!=PS2_GS_TEXTURE_SIZE||src->height!=PS2_GS_TEXTURE_SIZE)return NULL;
    if(!ps2_gs_ensure_texture_cache())return NULL;

    uint64_t bit=1ULL<<id;
    GSTEXTURE *tex=&ps2_gs_state.textures[id];
    if(!(ps2_gs_state.texture_allocated_mask&bit)) {
        memset(tex,0,sizeof(*tex));
        tex->Width=src->width;tex->Height=src->height;tex->PSM=GS_PSM_CT16;
        tex->Filter=GS_FILTER_NEAREST;tex->Delayed=0;
        tex->Vram=gsKit_vram_alloc(gs,gsKit_texture_size(tex->Width,tex->Height,tex->PSM),GSKIT_ALLOC_USERBUFFER);
        if(tex->Vram==GSKIT_ALLOC_ERROR){memset(tex,0,sizeof(*tex));return NULL;}
        ps2_gs_state.texture_allocated_mask|=bit;
    }

    if(!(ps2_gs_state.texture_uploaded_mask&bit)) {
        int *palette=_Pix3D.texturePalettes[id];
        bool transparent=false;
        for(int i=0;i<PS2_GS_TEXTURE_SIZE*PS2_GS_TEXTURE_SIZE;i++) {
            int rgb=palette[(uint8_t)src->pixels[i]]&0x00f8f8ff;
            uint32_t r5=((uint32_t)rgb>>19)&0x1f,g5=((uint32_t)rgb>>11)&0x1f,b5=((uint32_t)rgb>>3)&0x1f;
            uint16_t a1=rgb==0?0:0x8000;
            if(rgb==0)transparent=true;
            ps2_gs_state.texture_upload[i]=(uint16_t)(a1|(b5<<10)|(g5<<5)|r5);
        }
        _Pix3D.textureHasTransparency[id]=transparent;
        tex->Mem=(u32 *)ps2_gs_state.texture_upload;
        gsKit_texture_upload(gs,tex);
        ps2_gs_state.texture_uploaded_mask|=bit;
    }
    return tex;
}

PS2_GS_RUNTIME_CODE
static void ps2_gs_set_alpha_mode(GSGLOBAL *gs,u64 mode) {
    if(gs->PrimAlpha!=mode||gs->PABE!=0)gsKit_set_primalpha(gs,mode,0);
}

#if PS2_GS_DIRECT_VIEWPORT_TEST
PS2_GS_RUNTIME_CODE
static bool ps2_gs_ensure_viewport_overlay(void) {
    if (ps2_gs_state.magic != PS2_GS_STATE_MAGIC) ps2_gs_reset_state();

    if (!ps2_gs_state.viewport_overlay_texture) {
        ps2_gs_state.viewport_overlay_texture = calloc(1, sizeof(GSTEXTURE));
        if (!ps2_gs_state.viewport_overlay_texture) return false;
        ps2_gs_state.viewport_overlay_texture->Width = PS2_VIEWPORT_LOGICAL_WIDTH;
        ps2_gs_state.viewport_overlay_texture->Height = PS2_VIEWPORT_LOGICAL_HEIGHT;
        ps2_gs_state.viewport_overlay_texture->PSM = GS_PSM_CT16;
        ps2_gs_state.viewport_overlay_texture->Filter = GS_FILTER_NEAREST;
        ps2_gs_state.viewport_overlay_texture->Delayed = 0;
        ps2_gs_state.viewport_overlay_texture->Vram = GSKIT_ALLOC_ERROR;
    }

    if (!ps2_gs_state.viewport_overlay_upload) {
        size_t bytes = gsKit_texture_size(
            PS2_VIEWPORT_LOGICAL_WIDTH, PS2_VIEWPORT_LOGICAL_HEIGHT, GS_PSM_CT16);
        ps2_gs_state.viewport_overlay_upload = memalign(128, bytes);
        if (!ps2_gs_state.viewport_overlay_upload) return false;
        ps2_gs_state.viewport_overlay_texture->Mem =
            (u32 *)ps2_gs_state.viewport_overlay_upload;
    }

    return true;
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_capture_viewport_overlay(const uint32_t *pixels, int width, int height) {
    if (!pixels || width != PS2_VIEWPORT_LOGICAL_WIDTH ||
        height != PS2_VIEWPORT_LOGICAL_HEIGHT ||
        !ps2_gs_ensure_viewport_overlay()) {
        return false;
    }

    uint16_t *dst = ps2_gs_state.viewport_overlay_upload;
    const int count = width * height;
    memset(dst, 0, (size_t)count * sizeof(uint16_t));

    // The GS already owns every 3D triangle. The CPU surface is now only a sparse overlay:
    // hitmarks, overhead text, hints, performance text, menus/cursor, etc. The 0xffffffff key is
    // produced with one fast memset before those overlays are drawn, so keyed pixels need no
    // RGB->CT16 math or write here.
    for (int i = 0; i < count; i++) {
        uint32_t rgb = pixels[i];
        if (rgb == PS2_VIEWPORT_OVERLAY_KEY) continue;

        uint32_t r5 = ((rgb >> 16) & 0xff) >> 3;
        uint32_t g5 = ((rgb >> 8) & 0xff) >> 3;
        uint32_t b5 = (rgb & 0xff) >> 3;
        dst[i] = (uint16_t)(0x8000 | (b5 << 10) | (g5 << 5) | r5);
    }

    ps2_gs_state.viewport_overlay_dirty = true;
    return true;
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_draw_viewport_overlay(void *gs_global,
                                         float view_x, float view_y,
                                         float view_w, float view_h) {
    if (!gs_global || !ps2_gs_state.viewport_overlay_dirty ||
        !ps2_gs_state.viewport_overlay_texture ||
        !ps2_gs_state.viewport_overlay_upload) {
        return;
    }

    GSGLOBAL *gs = (GSGLOBAL *)gs_global;
    GSTEXTURE *overlay = ps2_gs_state.viewport_overlay_texture;
    if (overlay->Vram == GSKIT_ALLOC_ERROR) {
        overlay->Vram = gsKit_vram_alloc(
            gs, gsKit_texture_size(overlay->Width, overlay->Height, overlay->PSM),
            GSKIT_ALLOC_USERBUFFER);
        if (overlay->Vram == GSKIT_ALLOC_ERROR) return;
    }

    int saved_alpha_enable = gs->PrimAlphaEnable;
    u64 saved_alpha_mode = gs->PrimAlpha;
    u8 saved_pabe = gs->PABE;
    u8 saved_ate = gs->Test->ATE;
    u8 saved_atst = gs->Test->ATST;
    u8 saved_aref = gs->Test->AREF;
    u8 saved_afail = gs->Test->AFAIL;

    gsKit_texture_upload(gs, overlay);

    // CT16 A1=0 is the sparse-overlay key. Reject it and source-replace every A1=1 pixel so the
    // overlay never blends/darkens the already-rendered GS world.
    gs->Test->ATE = GS_SETTING_ON;
    gs->Test->ATST = 6; // GREATER
    gs->Test->AREF = 0;
    gs->Test->AFAIL = 0; // KEEP
    gsKit_set_test(gs, 0);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 2, 0x80), 0);
    gs->PrimAlphaEnable = GS_SETTING_ON;

    gsKit_prim_sprite_texture_3d(
        gs, overlay,
        view_x, view_y, 0, 0.0f, 0.0f,
        view_x + view_w, view_y + view_h, 0,
        (float)PS2_VIEWPORT_LOGICAL_WIDTH, (float)PS2_VIEWPORT_LOGICAL_HEIGHT,
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));

    gs->PrimAlphaEnable = saved_alpha_enable;
    gs->Test->ATE = saved_ate;
    gs->Test->ATST = saved_atst;
    gs->Test->AREF = saved_aref;
    gs->Test->AFAIL = saved_afail;
    gsKit_set_test(gs, 0);
    gsKit_set_primalpha(gs, saved_alpha_mode, saved_pabe);

    ps2_gs_state.viewport_overlay_dirty = false;
}
#endif

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_flush(void *gs_global,float view_x,float view_y,float view_w,float view_h) {
    if(!gs_global||!ps2_gs_raster_has_pending()||view_w<=0.0f||view_h<=0.0f)return;
    GSGLOBAL *gs=(GSGLOBAL *)gs_global;
    const float sx=view_w/(float)PS2_3D_RENDER_WIDTH,sy=view_h/(float)PS2_3D_RENDER_HEIGHT;
    const float vx1=view_x+view_w,vy1=view_y+view_h;

    int cx0=(int)view_x,cy0=(int)view_y,cx1=(int)vx1-1,cy1=(int)vy1-1;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= gs->Width) cx1 = gs->Width - 1;
    if (cy1 >= gs->Height) cy1 = gs->Height - 1;

    int saved_alpha_enable=gs->PrimAlphaEnable;
    u64 saved_alpha_mode=gs->PrimAlpha;
    u8 saved_pabe=gs->PABE;
    u8 saved_ate=gs->Test->ATE,saved_atst=gs->Test->ATST,saved_aref=gs->Test->AREF,saved_afail=gs->Test->AFAIL;

    gsKit_set_scissor(gs,GS_SETREG_SCISSOR(cx0,cx1,cy0,cy1));
    gsKit_set_primalpha(gs,GS_BLEND_BACK2FRONT,0);
    gs->Test->ATE=GS_SETTING_ON;gs->Test->ATST=6;gs->Test->AREF=0;gs->Test->AFAIL=0;
    gsKit_set_test(gs,0);
    gs->PrimAlphaEnable=GS_SETTING_OFF;
    gsKit_prim_sprite(gs,view_x,view_y,vx1,vy1,1,
        GS_SETREG_RGBAQ((PS2_VIEWPORT_SKY_RGB>>16)&0xff,(PS2_VIEWPORT_SKY_RGB>>8)&0xff,PS2_VIEWPORT_SKY_RGB&0xff,0x80,0));

    for (int i = 0; i < ps2_gs_state.count;) {
        Ps2GsTriangle *tri = &ps2_gs_state.triangles[i];
        float x1 = view_x + (float)tri->x1 * sx;
        float y1 = view_y + (float)tri->y1 * sy;
        float x2 = view_x + (float)tri->x2 * sx;
        float y2 = view_y + (float)tri->y2 * sy;
        float x3 = view_x + (float)tri->x3 * sx;
        float y3 = view_y + (float)tri->y3 * sy;

        if (tri->kind == PS2_GS_TRI_TEXTURED) {
            GSTEXTURE *tex = ps2_gs_prepare_texture(gs, tri->texture_id);
            if (tex) {
#if PS2_GS_TEXTURE_BATCH_TEST
                if (ps2_gs_ensure_texture_batch()) {
                    const uint8_t texture_id = tri->texture_id;
                    const bool translucent = tri->alpha != 0;
                    int vertices = 0;
                    int j = i;

                    if (translucent) ps2_gs_set_alpha_mode(gs, GS_BLEND_BACK2FRONT);
                    else ps2_gs_set_alpha_mode(gs, GS_SETREG_ALPHA(0, 2, 2, 2, 0x80));
                    // TCC must stay enabled so CT16 A1 reaches the alpha test.
                    gs->PrimAlphaEnable = GS_SETTING_ON;

                    // Batch only a contiguous run. The first incompatible face terminates the
                    // packet, preserving RuneScape's original painter/priority sequence exactly.
                    while (j < ps2_gs_state.count &&
                           vertices < PS2_GS_TEXTURE_BATCH_VERTICES) {
                        Ps2GsTriangle *candidate = &ps2_gs_state.triangles[j];
                        if (candidate->kind != PS2_GS_TRI_TEXTURED ||
                            candidate->texture_id != texture_id ||
                            (candidate->alpha != 0) != translucent) {
                            break;
                        }

                        uint8_t a = ps2_gs_alpha_value(candidate->alpha);
                        uint8_t l1 = ps2_gs_texture_light(candidate->shade1);
                        uint8_t l2 = ps2_gs_texture_light(candidate->shade2);
                        uint8_t l3 = ps2_gs_texture_light(candidate->shade3);

                        float bx1 = view_x + (float)candidate->x1 * sx;
                        float by1 = view_y + (float)candidate->y1 * sy;
                        float bx2 = view_x + (float)candidate->x2 * sx;
                        float by2 = view_y + (float)candidate->y2 * sy;
                        float bx3 = view_x + (float)candidate->x3 * sx;
                        float by3 = view_y + (float)candidate->y3 * sy;

                        GSPRIMSTQPOINT *out = &ps2_gs_state.texture_batch[vertices];
                        out[0].rgbaq = color_to_RGBAQ(l1, l1, l1, a, candidate->q1);
                        out[0].stq = vertex_to_STQ(candidate->s1, candidate->t1);
                        out[0].xyz2 = vertex_to_XYZ2(gs, bx1, by1, 2);
                        out[1].rgbaq = color_to_RGBAQ(l2, l2, l2, a, candidate->q2);
                        out[1].stq = vertex_to_STQ(candidate->s2, candidate->t2);
                        out[1].xyz2 = vertex_to_XYZ2(gs, bx2, by2, 2);
                        out[2].rgbaq = color_to_RGBAQ(l3, l3, l3, a, candidate->q3);
                        out[2].stq = vertex_to_STQ(candidate->s3, candidate->t3);
                        out[2].xyz2 = vertex_to_XYZ2(gs, bx3, by3, 2);

                        vertices += 3;
                        j++;
                    }

                    if (vertices > 0) {
                        gsKit_prim_list_triangle_goraud_texture_stq_3d(
                            gs, tex, vertices, ps2_gs_state.texture_batch);
                        i = j;
                        continue;
                    }
                }
#endif
                // Allocation failure or batching disabled: retain the hardware-proven one-triangle
                // path exactly so this optimization cannot make a drawable face disappear.
                if (tri->alpha) ps2_gs_set_alpha_mode(gs, GS_BLEND_BACK2FRONT);
                else ps2_gs_set_alpha_mode(gs, GS_SETREG_ALPHA(0, 2, 2, 2, 0x80));
                gs->PrimAlphaEnable = GS_SETTING_ON;

                uint8_t a = ps2_gs_alpha_value(tri->alpha);
                uint8_t l1 = ps2_gs_texture_light(tri->shade1);
                uint8_t l2 = ps2_gs_texture_light(tri->shade2);
                uint8_t l3 = ps2_gs_texture_light(tri->shade3);
                GSPRIMSTQPOINT verts[3];
                verts[0].rgbaq = color_to_RGBAQ(l1, l1, l1, a, tri->q1);
                verts[0].stq = vertex_to_STQ(tri->s1, tri->t1);
                verts[0].xyz2 = vertex_to_XYZ2(gs, x1, y1, 2);
                verts[1].rgbaq = color_to_RGBAQ(l2, l2, l2, a, tri->q2);
                verts[1].stq = vertex_to_STQ(tri->s2, tri->t2);
                verts[1].xyz2 = vertex_to_XYZ2(gs, x2, y2, 2);
                verts[2].rgbaq = color_to_RGBAQ(l3, l3, l3, a, tri->q3);
                verts[2].stq = vertex_to_STQ(tri->s3, tri->t3);
                verts[2].xyz2 = vertex_to_XYZ2(gs, x3, y3, 2);
                gsKit_prim_list_triangle_goraud_texture_stq_3d(gs, tex, 3, verts);
                i++;
                continue;
            }
            tri->kind = PS2_GS_TRI_FLAT;
        }

#if PS2_GS_UNTEXTURED_BATCH_TEST
        if ((tri->kind == PS2_GS_TRI_FLAT || tri->kind == PS2_GS_TRI_GOURAUD) &&
            ps2_gs_ensure_untextured_batch()) {
            const bool translucent = tri->alpha != 0;
            int vertices = 0;
            int j = i;

            if (translucent) {
                ps2_gs_set_alpha_mode(gs, GS_BLEND_BACK2FRONT);
                gs->PrimAlphaEnable = GS_SETTING_ON;
            } else {
                gs->PrimAlphaEnable = GS_SETTING_OFF;
            }

            // Flat and Gouraud faces can share the same Gouraud list packet. Flat faces simply
            // repeat their RGB at all three vertices. Stop at the first textured face or alpha-mode
            // transition so the original painter ordering remains exactly intact.
            while (j < ps2_gs_state.count &&
                   vertices < PS2_GS_UNTEXTURED_BATCH_VERTICES) {
                Ps2GsTriangle *candidate = &ps2_gs_state.triangles[j];
                if ((candidate->kind != PS2_GS_TRI_FLAT &&
                     candidate->kind != PS2_GS_TRI_GOURAUD) ||
                    (candidate->alpha != 0) != translucent) {
                    break;
                }

                uint32_t c1 = candidate->color1;
                uint32_t c2 = candidate->kind == PS2_GS_TRI_FLAT ? c1 : candidate->color2;
                uint32_t c3 = candidate->kind == PS2_GS_TRI_FLAT ? c1 : candidate->color3;

                float bx1 = view_x + (float)candidate->x1 * sx;
                float by1 = view_y + (float)candidate->y1 * sy;
                float bx2 = view_x + (float)candidate->x2 * sx;
                float by2 = view_y + (float)candidate->y2 * sy;
                float bx3 = view_x + (float)candidate->x3 * sx;
                float by3 = view_y + (float)candidate->y3 * sy;

                GSPRIMPOINT *out = &ps2_gs_state.untextured_batch[vertices];
                out[0].rgbaq = rgbaq_to_RGBAQ(ps2_gs_color(c1, candidate->alpha));
                out[0].xyz2 = vertex_to_XYZ2(gs, bx1, by1, 2);
                out[1].rgbaq = rgbaq_to_RGBAQ(ps2_gs_color(c2, candidate->alpha));
                out[1].xyz2 = vertex_to_XYZ2(gs, bx2, by2, 2);
                out[2].rgbaq = rgbaq_to_RGBAQ(ps2_gs_color(c3, candidate->alpha));
                out[2].xyz2 = vertex_to_XYZ2(gs, bx3, by3, 2);

                vertices += 3;
                j++;
            }

            if (vertices > 0) {
                gsKit_prim_list_triangle_gouraud_3d(
                    gs, vertices, ps2_gs_state.untextured_batch);
                i = j;
                continue;
            }
        }
#endif

        // Allocation failure or batching disabled: preserve the already-proven one-face path.
        if (tri->alpha) {
            ps2_gs_set_alpha_mode(gs, GS_BLEND_BACK2FRONT);
            gs->PrimAlphaEnable = GS_SETTING_ON;
        } else {
            gs->PrimAlphaEnable = GS_SETTING_OFF;
        }

        if (tri->kind == PS2_GS_TRI_GOURAUD) {
            gsKit_prim_triangle_gouraud_3d(
                gs, x1, y1, 2, x2, y2, 2, x3, y3, 2,
                ps2_gs_color(tri->color1, tri->alpha),
                ps2_gs_color(tri->color2, tri->alpha),
                ps2_gs_color(tri->color3, tri->alpha));
        } else {
            gsKit_prim_triangle_3d(
                gs, x1, y1, 2, x2, y2, 2, x3, y3, 2,
                ps2_gs_color(tri->color1, tri->alpha));
        }
        i++;
    }

    gs->PrimAlphaEnable=saved_alpha_enable;
    gs->Test->ATE=saved_ate;gs->Test->ATST=saved_atst;gs->Test->AREF=saved_aref;gs->Test->AFAIL=saved_afail;
    gsKit_set_test(gs,0);gsKit_set_primalpha(gs,saved_alpha_mode,saved_pabe);gsKit_set_scissor(gs,GS_SCISSOR_RESET);
    ps2_gs_state.count=0;ps2_gs_state.overflow=0;
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_discard(void) {
    if(ps2_gs_state.magic==PS2_GS_STATE_MAGIC){ps2_gs_state.count=0;ps2_gs_state.overflow=0;}
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_shutdown(void) {
    if(ps2_gs_state.magic!=PS2_GS_STATE_MAGIC)return;
    free(ps2_gs_state.triangles);
    free(ps2_gs_state.textures);
    free(ps2_gs_state.texture_upload);
    free(ps2_gs_state.texture_batch);
    free(ps2_gs_state.untextured_batch);
#if PS2_GS_DIRECT_VIEWPORT_TEST
    free(ps2_gs_state.viewport_overlay_texture);
    free(ps2_gs_state.viewport_overlay_upload);
#endif
    ps2_gs_reset_state();
}

#endif
