/* Direct3D 5 (IDirect3D, IDirect3D2, IDirect3DDevice2) on the Metal renderer.
 * d3d.vd hands Direct3D pre-transformed vertices, so the device rasterises
 * D3DTLVERTEX lists with the game's render states. The render target is the
 * Metal colour texture; its guest pixels are synchronised only when the game
 * locks or blits from it. */
#include "runtime.h"
#include "ddraw.h"
#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t com_object(unsigned, unsigned);
void gheap_free(int, uint32_t);
static uint32_t A(CPU *c, unsigned i){ return MR32(c->esp + 4 * i); }
static float AF(CPU *c, unsigned i){ uint32_t v = A(c, i); float f; memcpy(&f, &v, 4); return f; }

#define E_NOINTERFACE 0x80004002u
#define D3DERR_INVALIDPRIMITIVETYPE 0x887602deu

static const uint8_t IID_D3D[16]  = {0x80,0x00,0xba,0x3b,0x21,0x24,0xcf,0x11,0xa3,0x1a,0x00,0xaa,0x00,0xb9,0x33,0x56};
static const uint8_t IID_D3D2[16] = {0xc1,0x1e,0xae,0x6a,0x2a,0x66,0xd0,0x11,0x88,0x9d,0x00,0xaa,0x00,0xbb,0xb7,0x6a};
static const uint8_t IID_HAL[16]  = {0xe0,0x3d,0xe6,0x84,0xaa,0x46,0xcf,0x11,0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e};
static const uint8_t IID_TEX[16]  = {0xe0,0xd9,0xdc,0x2c,0xa0,0x25,0xcf,0x11,0xa3,0x1a,0x00,0xaa,0x00,0xb9,0x33,0x56};
static const uint8_t IID_TEX2[16] = {0x02,0x15,0x28,0x93,0xf8,0x8c,0xd0,0x11,0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29};

static const uint8_t IID_DEV1[16] = {0x00,0x88,0x10,0x64,0x7d,0x95,0xd0,0x11,0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29};
static const uint8_t IID_DEV2[16] = {0xc0,0xc1,0xd5,0x93,0x6f,0xa6,0xd0,0x11,0x8a,0xde,0x00,0xa0,0xc9,0x05,0x41,0x29};
static uint32_t d3d1_obj, d3d2_obj, device1_obj;
static Surface *rt;             /* render target (back buffer) */
static int guest_dirty;         /* guest pixels of rt newer than the GPU colour */
static int trace = -1;

/* ---- materials and viewports ---- */
#define MAT_MAX 256
static struct { int used; uint32_t obj; float diffuse[4]; uint32_t texture; } mats[MAT_MAX];
#define VP_MAX 32
static struct { int used; uint32_t obj; int x, y, w, h; float minz, maxz; uint32_t background; } vps[VP_MAX];
static int current_vp = -1;

/* ---- device state ---- */
static uint32_t rs[256];
static uint32_t device_obj;
static struct { int active, indexed; uint32_t type, vtype; RenderVertex *v; unsigned n, cap; } imm;

static void sync_trace(void){ if (trace < 0){ const char *e = getenv("GEARHEAD_D3D_TRACE"); trace = e ? atoi(e) : 0; } }

/* ---- render target synchronisation ---- */
static void rt_to_gpu(void){
    if (!rt || !guest_dirty) return;
    guest_dirty = 0;
    uint32_t *buf = malloc((size_t)rt->w * rt->h * 4);
    const uint8_t *p = g_mem + rt->pixels;
    for (unsigned y = 0; y < rt->h; y++) for (unsigned x = 0; x < rt->w; x++){
        uint16_t v; memcpy(&v, p + (size_t)y * rt->pitch + 2 * x, 2);
        unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        buf[(size_t)y * rt->w + x] = 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
    }
    render_write(0, 0, rt->w, rt->h, buf, 0);
    free(buf);
}
/* BGRA (from a Metal texture) into a 16-bit 565 guest surface */
static void bgra_to_565(Surface *s, const uint32_t *buf){
    uint8_t *p = g_mem + s->pixels;
    for (unsigned y = 0; y < s->h; y++) for (unsigned x = 0; x < s->w; x++){
        uint32_t c = buf[(size_t)y * s->w + x];
        uint16_t v = (uint16_t)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31));
        memcpy(p + (size_t)y * s->pitch + 2 * x, &v, 2);
    }
}
static void rt_lock(Surface *s){
    if (s != rt || !s->gpu_dirty) return;
    if (guest_dirty) rt_to_gpu();
    uint32_t *buf = malloc((size_t)s->w * s->h * 4);
    if (render_read(0, 0, s->w, s->h, buf, 0)) bgra_to_565(s, buf);
    static unsigned locks; locks++;
    const char *dump = getenv("GEARHEAD_DUMP_LOCKS");
    if (dump && locks % (unsigned)atoi(dump) == 0){
        char path[64]; snprintf(path, sizeof path, "out/rt-%06u.bgra", locks);
        FILE *f = fopen(path, "wb"); if (f){ fwrite(buf, 4, (size_t)s->w * s->h, f); fclose(f); }
    }
    free(buf);
    s->gpu_dirty = 0;
}
static void rt_unlock(Surface *s){ if (s == rt) guest_dirty = 1; }
static void gpu_begin(void){ if (rt){ rt_to_gpu(); rt->gpu_dirty = 1; } }

/* After a flip the front buffer is the presented Metal frame. */
static void front_lock(Surface *s){
    if (!s->gpu_dirty || s->bpp != 16) return;
    uint32_t *buf = malloc((size_t)s->w * s->h * 4);
    if (render_read(0, 0, s->w, s->h, buf, 1)) bgra_to_565(s, buf);
    free(buf);
    s->gpu_dirty = 0;
}
static int flip_hook(Surface *front, Surface *back){
    if (back != rt) return 0;
    rt_to_gpu();
    render_present(0);
    static unsigned flips; flips++;
    const char *dump = getenv("GEARHEAD_DUMP_FLIPS");
    if (dump && flips % (unsigned)atoi(dump) == 0) render_capture_frame(100000 + flips);
    rt->gpu_dirty = 1;
    front->gpu_dirty = 1; front->lock_hook = front_lock;
    return 1;
}

/* ---- textures ---- */
static void texture_upload(Surface *t){
    if (!t->texture) t->texture = (uint32_t)(t - g_surf);
    if (t->uploaded_version == t->texture_version + 1) return;
    uint32_t *buf = malloc((size_t)t->w * t->h * 4);
    const uint8_t *p = g_mem + t->pixels;
    int key = t->has_ck && rs[41];
    for (unsigned y = 0; y < t->h; y++) for (unsigned x = 0; x < t->w; x++){
        uint32_t v = 0, out;
        if (t->bpp == 16){ uint16_t w; memcpy(&w, p + (size_t)y * t->pitch + 2 * x, 2); v = w; }
        else if (t->bpp == 32) memcpy(&v, p + (size_t)y * t->pitch + 4 * x, 4);
        else v = p[(size_t)y * t->pitch + x];
        if (t->bpp == 16 && t->amask == 0xf000){
            out = (((v >> 12) & 15) * 17u << 24) | (((v >> 8) & 15) * 17u << 16) | (((v >> 4) & 15) * 17u << 8) | ((v & 15) * 17u);
        } else if (t->bpp == 16 && t->gmask == 0x03e0){
            unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            out = ((t->amask && !(v & 0x8000)) ? 0 : 0xff000000u) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
        } else if (t->bpp == 16){
            unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            out = 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        } else if (t->bpp == 32) out = t->amask ? v : 0xff000000u | v;
        else out = 0xff000000u | v * 0x010101u;
        if (key && v >= t->ck_lo && v <= t->ck_hi) out &= 0x00ffffffu;
        buf[(size_t)y * t->w + x] = out;
    }
    render_texture(t->texture, t->w, t->h, 0, 1, buf);
    free(buf);
    t->uploaded_version = t->texture_version + 1;
}

/* ---- state translation ---- */
static unsigned blend_factor(uint32_t b){
    switch (b){
    case 1: return 1; case 2: return 0; case 3: return 6; case 4: return 8; case 5: return 2; case 6: return 3;
    case 7: return 4; case 8: return 5; case 9: return 7; case 10: return 9; default: return 0;
    }
}
static void build_state(RenderState *s, Surface **tex){
    memset(s, 0, sizeof *s);
    s->cull = rs[22] == 1 ? 0 : rs[22] == 2 ? 2 : 1;
    s->depth = rs[7] != 0; s->depth_write = rs[14] != 0;
    s->depth_func = rs[23] >= 1 && rs[23] <= 8 ? rs[23] - 1 : 3;
    s->blend = rs[27] != 0;
    if (rs[27]){
        uint32_t src = rs[19], dst = rs[20];
        if (src == 12){ src = 5; dst = 6; } else if (src == 13){ src = 6; dst = 5; }
        s->src_blend = blend_factor(src); s->dst_blend = blend_factor(dst);
    }
    if (rs[15]){ s->alpha_ref = (rs[24] & 255) | 0x100; s->alpha_func = rs[25] >= 1 && rs[25] <= 8 ? rs[25] - 1 : 7; }
    s->shade = rs[29] ? 2 : 1;
    Surface *t = NULL;
    if (rs[1] && rs[1] < SURF_MAX && g_surf[rs[1]].used) t = &g_surf[rs[1]];
    *tex = t;
    if (t){
        texture_upload(t);
        s->texture[0] = t->texture;
        switch (rs[21]){
        case 1: case 7: s->combine[0] = 0x70; break;          /* DECAL, COPY: texture */
        case 3: s->combine[0] = 0x71; break;                  /* DECALALPHA */
        case 4: s->combine[0] = 3; break;                     /* MODULATEALPHA */
        case 8: s->combine[0] = 0x72; break;                  /* ADD */
        default: s->combine[0] = t->amask || t->has_ck ? 3 : 0x73; break; /* MODULATE */
        }
        if (t->has_ck && rs[41] && !rs[15]){ s->alpha_ref = 0x100 | 1; s->alpha_func = 6; }
        s->filter[0] = rs[17] == 2 || rs[18] == 2 || rs[18] == 6;
        uint32_t au = rs[44] ? rs[44] : rs[3], av = rs[45] ? rs[45] : rs[3];
        s->address_u[0] = au == 2 ? 2 : au == 3 ? 0 : 1;
        s->address_v[0] = av == 2 ? 2 : av == 3 ? 0 : 1;
    }
    s->fog = rs[28] != 0; s->fog_color = rs[34];
    s->fog_mode = 0;
    s->brightness = 1;
    unsigned w = rt ? rt->w : ddraw_mode_width(), h = rt ? rt->h : ddraw_mode_height();
    s->clip[0] = 0; s->clip[1] = 0; s->clip[2] = (int)w; s->clip[3] = (int)h;
    if (current_vp >= 0 && vps[current_vp].w){
        s->clip[0] = vps[current_vp].x; s->clip[1] = vps[current_vp].y;
        s->clip[2] = vps[current_vp].x + vps[current_vp].w; s->clip[3] = vps[current_vp].y + vps[current_vp].h;
    }
}

/* ---- drawing ---- */
static void vertex_in(RenderVertex *o, uint32_t vtype, uint32_t p){
    memset(o, 0, sizeof *o);
    if (vtype == 3){                 /* D3DTLVERTEX */
        memcpy(o, guest_ptr(p, 32), 32);
    } else runtime_fail(g_cur_cpu, "untransformed Direct3D vertex type", vtype);
}
static unsigned vertex_size(uint32_t vtype){ return 32; (void)vtype; }

static RenderVertex *scratch; static size_t scratch_cap;
static RenderVertex *grow(size_t n){
    if (n > scratch_cap){ scratch_cap = n * 2; scratch = realloc(scratch, scratch_cap * sizeof *scratch); }
    return scratch;
}

/* Expand a primitive into a list and draw it. */
static void draw(uint32_t type, const RenderVertex *v, unsigned n, const uint16_t *idx, unsigned ni){
    if (!rt) return;
    sync_trace();
    RenderState s; Surface *tex;
    build_state(&s, &tex);
    unsigned count = idx ? ni : n;
    if (!count) return;
    #define V(k) (idx ? v[idx[k] < n ? idx[k] : 0] : v[k])
    unsigned prim = 0; size_t out_n = 0; RenderVertex *o;
    switch (type){
    case 1: prim = 2; o = grow(count); for (unsigned i = 0; i < count; i++) o[out_n++] = V(i); break;
    case 2: prim = 1; o = grow(count); for (unsigned i = 0; i + 1 < count; i += 2){ o[out_n++] = V(i); o[out_n++] = V(i + 1); } break;
    case 3: prim = 1; o = grow(count * 2); for (unsigned i = 0; i + 1 < count; i++){ o[out_n++] = V(i); o[out_n++] = V(i + 1); } break;
    case 4: o = grow(count); for (unsigned i = 0; i + 2 < count; i += 3){ o[out_n++] = V(i); o[out_n++] = V(i + 1); o[out_n++] = V(i + 2); } break;
    case 5: o = grow(count * 3); for (unsigned i = 0; i + 2 < count; i++){
        if (i & 1){ o[out_n++] = V(i + 1); o[out_n++] = V(i); o[out_n++] = V(i + 2); }
        else { o[out_n++] = V(i); o[out_n++] = V(i + 1); o[out_n++] = V(i + 2); } } break;
    case 6: o = grow(count * 3); for (unsigned i = 1; i + 1 < count; i++){ o[out_n++] = V(0); o[out_n++] = V(i); o[out_n++] = V(i + 1); } break;
    default: return;
    }
    #undef V
    int flat = rs[9] == 1;
    if (flat && prim == 0) for (size_t i = 0; i + 2 < out_n; i += 3){
        o[i + 1].diffuse = o[i + 2].diffuse = o[i].diffuse;
        o[i + 1].specular = o[i + 2].specular = o[i].specular;
    }
    if (flat && prim == 1) for (size_t i = 0; i + 1 < out_n; i += 2) o[i + 1].diffuse = o[i].diffuse;
    if (trace > 1) fprintf(stderr, "d3d draw type=%u n=%zu tex=%u blend=%u/%u/%u z=%u/%u\n", type, out_n, s.texture[0], s.blend, s.src_blend, s.dst_blend, s.depth, s.depth_write);
    if (trace > 2) for (size_t i = 0; i < out_n && i < 3; i++)
        fprintf(stderr, "  v%zu %.2f %.2f z=%.4f rhw=%.4f c=%08x s=%08x uv=%.3f,%.3f\n", i, o[i].x, o[i].y, o[i].z, o[i].rhw, o[i].diffuse, o[i].specular, o[i].u, o[i].v);
    gpu_begin();
    render_draw(o, (unsigned)out_n, prim, &s);
}

static void clear(unsigned count, uint32_t rects, uint32_t flags, uint32_t color, float z){
    gpu_begin();
    for (unsigned i = 0; i < (count ? count : 1); i++){
        int clip[4] = { 0, 0, (int)rt->w, (int)rt->h };
        if (count && rects){ uint32_t r = rects + 16 * i; clip[0] = (int32_t)MR32(r); clip[1] = (int32_t)MR32(r + 4); clip[2] = (int32_t)MR32(r + 8); clip[3] = (int32_t)MR32(r + 12); }
        int full = clip[0] <= 0 && clip[1] <= 0 && clip[2] >= (int)rt->w && clip[3] >= (int)rt->h;
        if ((flags & 3) == 3 && full){ render_clear(color, z, 0, clip); continue; }
        if ((flags & 2) && full && !(flags & 1)){ render_clear(color, z, 1, clip); continue; }
        RenderState s; memset(&s, 0, sizeof s);
        s.brightness = 1; memcpy(s.clip, clip, sizeof clip);
        s.depth = 1; s.depth_func = 7; s.depth_write = (flags & 2) != 0; s.depth_only = !(flags & 1);
        RenderVertex v[6]; float xy[][2] = {{0,0},{(float)rt->w,0},{(float)rt->w,(float)rt->h},{0,0},{(float)rt->w,(float)rt->h},{0,(float)rt->h}};
        for (unsigned k = 0; k < 6; k++) v[k] = (RenderVertex){ .x = xy[k][0] - .5f, .y = xy[k][1] - .5f, .z = z, .rhw = 1, .diffuse = color };
        render_draw(v, 6, 0, &s);
    }
}

static void depth_fill(Surface *z, const int *r, uint32_t value){
    (void)z;
    if (!rt) return;
    uint32_t rect[4] = { (uint32_t)r[0], (uint32_t)r[1], (uint32_t)r[2], (uint32_t)r[3] };
    uint32_t p = win32_alloc(16, 0); memcpy(g_mem + p, rect, 16);
    clear(1, p, 2, 0, value >= 0xffff ? 1.f : value / 65535.f);
    gheap_free(1, p);
}

static int fill_hook(Surface *d, const int *r, uint32_t v){
    if (d != rt) return 0;
    unsigned cr = (v >> 11) & 31, cg = (v >> 5) & 63, cb = v & 31;
    uint32_t color = 0xff000000u | (((cr << 3) | (cr >> 2)) << 16) | (((cg << 2) | (cg >> 4)) << 8) | ((cb << 3) | (cb >> 2));
    uint32_t p = win32_alloc(16, 0);
    MR32(p) = (uint32_t)r[0]; MR32(p + 4) = (uint32_t)r[1]; MR32(p + 8) = (uint32_t)r[2]; MR32(p + 12) = (uint32_t)r[3];
    clear(1, p, 1, color, 1.f);
    gheap_free(1, p);
    return 1;
}

/* 2D blits onto the render target while the GPU holds it: draw a textured quad. */
static int blt_hook(Surface *d, const int *dr, Surface *src, const int *sr, int key){
    if (d != rt || !rt->gpu_dirty) return 0;
    if (src->bpp != 16 && src->bpp != 32) return 0;
    if (src == rt){ rt_lock(rt); return 0; }
    uint32_t saved_ck = rs[41]; rs[41] = key;
    int saved_has = src->has_ck; if (!key) src->has_ck = 0;
    src->uploaded_version = 0;
    texture_upload(src);
    src->has_ck = saved_has; rs[41] = saved_ck; src->uploaded_version = 0;
    RenderState s; memset(&s, 0, sizeof s);
    s.brightness = 1; s.texture[0] = src->texture; s.combine[0] = 0x70; s.depth_func = 7;
    s.clip[2] = (int)rt->w; s.clip[3] = (int)rt->h;
    if (key){ s.alpha_ref = 0x101; s.alpha_func = 6; }
    float u0 = (float)sr[0] / src->w, v0 = (float)sr[1] / src->h, u1 = (float)sr[2] / src->w, v1 = (float)sr[3] / src->h;
    float x0 = dr[0] - .5f, y0 = dr[1] - .5f, x1 = dr[2] - .5f, y1 = dr[3] - .5f;
    RenderVertex v[6] = {
        {x0,y0,0,1,0xffffffff,0,u0,v0,0,0},{x1,y0,0,1,0xffffffff,0,u1,v0,0,0},{x1,y1,0,1,0xffffffff,0,u1,v1,0,0},
        {x0,y0,0,1,0xffffffff,0,u0,v0,0,0},{x1,y1,0,1,0xffffffff,0,u1,v1,0,0},{x0,y1,0,1,0xffffffff,0,u0,v1,0,0}};
    gpu_begin();
    render_draw(v, 6, 0, &s);
    return 1;
}

/* ---- descriptions ---- */
static void fill_device_desc(uint32_t p, int hal){
    if (!p) return;
    uint32_t size = MR32(p); if (size < 204 || size > 252) size = 204;
    memset(guest_ptr(p, size), 0, size);
    MR32(p) = size;
    if (!hal){ return; }
    MR32(p + 4) = 0x7ff; MR32(p + 8) = 2;
    MR32(p + 12) = 0x1 | 0x10 | 0x40 | 0x200 | 0x400 | 0x800 | 0x80000;
    MR32(p + 16) = 8; MR32(p + 20) = 1; MR32(p + 24) = 1;
    MR32(p + 28) = 16; MR32(p + 32) = 7; MR32(p + 36) = 2; MR32(p + 40) = 8;
    for (unsigned k = 0; k < 2; k++){
        uint32_t c = p + 44 + 56 * k;
        MR32(c) = 56; MR32(c + 4) = 0x2 | 0x10 | 0x20 | 0x40; MR32(c + 8) = 0x1 | 0x10 | 0x20 | 0x80 | 0x100;
        MR32(c + 12) = 0xff; MR32(c + 16) = 0x1fff; MR32(c + 20) = 0x1fff; MR32(c + 24) = 0xff;
        MR32(c + 28) = 0x7ffff; MR32(c + 32) = 0x1 | 0x2 | 0x4 | 0x8; MR32(c + 36) = 0x3f; MR32(c + 40) = 0xff; MR32(c + 44) = 0x17;
    }
    MR32(p + 156) = 0x400 | 0x100; MR32(p + 160) = 0x400; MR32(p + 164) = 0; MR32(p + 168) = 65535;
    if (size >= 204){ MR32(p + 172) = 1; MR32(p + 176) = 1; MR32(p + 180) = 2048; MR32(p + 184) = 2048; }
}

static uint32_t enum_devices(CPU *c, uint32_t fn, uint32_t ctx){
    uint32_t guid = win32_alloc(16, 0), desc = win32_alloc(64, 1), name = win32_alloc(64, 1);
    uint32_t hal = win32_alloc(252, 1), hel = win32_alloc(252, 1);
    memcpy(g_mem + guid, IID_HAL, 16);
    strcpy((char *)g_mem + desc, "Apple Metal hardware rasterizer"); strcpy((char *)g_mem + name, "Direct3D HAL");
    MR32(hal) = 204; MR32(hel) = 204; fill_device_desc(hal, 1); fill_device_desc(hel, 1);
    uint32_t args[] = { guid, desc, name, hal, hel, ctx };
    call_guest(c, fn, 6, args);
    gheap_free(1, guid); gheap_free(1, desc); gheap_free(1, name); gheap_free(1, hal); gheap_free(1, hel);
    return 0;
}

static uint32_t enum_texture_formats(CPU *c, uint32_t fn, uint32_t ctx){
    static const uint32_t fmts[][5] = {   /* bits, r, g, b, a */
        {16, 0xf800, 0x07e0, 0x001f, 0}, {16, 0x7c00, 0x03e0, 0x001f, 0x8000}, {16, 0x0f00, 0x00f0, 0x000f, 0xf000}};
    uint32_t d = win32_alloc(108, 1);
    for (unsigned i = 0; i < 3; i++){
        memset(g_mem + d, 0, 108);
        MR32(d) = 108; MR32(d + 4) = 0x1000 | 1; MR32(d + 104) = 0x1000;
        uint32_t pf = d + 72;
        MR32(pf) = 32; MR32(pf + 4) = 0x40 | (fmts[i][4] ? 1 : 0); MR32(pf + 12) = fmts[i][0];
        MR32(pf + 16) = fmts[i][1]; MR32(pf + 20) = fmts[i][2]; MR32(pf + 24) = fmts[i][3]; MR32(pf + 28) = fmts[i][4];
        uint32_t args[] = { d, ctx };
        if (!call_guest(c, fn, 2, args)) break;
    }
    gheap_free(1, d);
    return 0;
}

static uint32_t new_material(unsigned family){
    for (unsigned i = 1; i < MAT_MAX; i++) if (!mats[i].used){
        memset(&mats[i], 0, sizeof mats[i]); mats[i].used = 1;
        mats[i].obj = com_object(family, 0); MR32(mats[i].obj + 32) = i; return mats[i].obj;
    }
    return 0;
}
static uint32_t new_viewport(void){
    for (unsigned i = 0; i < VP_MAX; i++) if (!vps[i].used){
        memset(&vps[i], 0, sizeof vps[i]); vps[i].used = 1; vps[i].maxz = 1;
        vps[i].obj = com_object(FAM_VIEWPORT2, 0); MR32(vps[i].obj + 32) = i; return vps[i].obj;
    }
    return 0;
}
static int vp_index(uint32_t obj){
    if (!obj || MR32(obj + 4) != FAM_VIEWPORT2) return -1;
    uint32_t i = MR32(obj + 32); return i < VP_MAX && vps[i].used ? (int)i : -1;
}

static uint32_t create_device(Surface *target){
    rt = target;
    rt->lock_hook = rt_lock; rt->unlock_hook = rt_unlock;
    if (rt->zbuf){ Surface *z = surf_of(rt->zbuf); if (z) z->depth_fill = depth_fill; }
    ddraw_blt_hook = blt_hook; ddraw_flip_hook = flip_hook; ddraw_fill_hook = fill_hook;
    memset(rs, 0, sizeof rs);
    rs[3] = 1; rs[4] = 1; rs[7] = rt->zbuf ? 1 : 0; rs[8] = 3; rs[9] = 2; rs[14] = 1; rs[16] = 1;
    rs[17] = 1; rs[18] = 1; rs[19] = 2; rs[20] = 1; rs[21] = 2; rs[22] = 3; rs[23] = 4; rs[25] = 8; rs[26] = 0;
    if (!device_obj) device_obj = com_object(FAM_D3DDEVICE2, 0);
    else MR32(device_obj + 8)++;
    fprintf(stderr, "Direct3D device on surface %u, %ux%u (z-buffer %s)\n", (unsigned)(rt - g_surf), rt->w, rt->h, rt->zbuf ? "yes" : "no");
    return device_obj;
}

int d3d_query(CPU *c, uint32_t iid, uint32_t *obj){
    (void)c;
    if (!memcmp(g_mem + iid, IID_D3D, 16)){ if (!d3d1_obj) d3d1_obj = com_object(FAM_D3D1, 0); *obj = d3d1_obj; return 1; }
    if (!memcmp(g_mem + iid, IID_D3D2, 16)){ if (!d3d2_obj) d3d2_obj = com_object(FAM_D3D2, 0); *obj = d3d2_obj; return 1; }
    *obj = 0; return 0;
}

int d3d_surface_query(CPU *c, Surface *s, uint32_t iid, uint32_t *obj){
    (void)c;
    if (!memcmp(g_mem + iid, IID_TEX2, 16) || !memcmp(g_mem + iid, IID_TEX, 16)){
        unsigned family = !memcmp(g_mem + iid, IID_TEX2, 16) ? FAM_TEXTURE2 : FAM_TEXTURE1;
        uint32_t t = com_object(family, 0); MR32(t + 32) = (uint32_t)(s - g_surf); MR32(s->obj + 8)++;
        *obj = t; return 1;
    }
    if (!memcmp(g_mem + iid, IID_HAL, 16)){ *obj = create_device(s); return 1; }
    return 0;
}

static Surface *texture_surface(uint32_t self){
    uint32_t i = MR32(self + 32);
    return i < SURF_MAX && g_surf[i].used ? &g_surf[i] : NULL;
}

static int d3d_object(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    if (family == FAM_D3D1 && slot >= 4) slot--;            /* IDirect3D has Initialize at slot 3 */
    else if (family == FAM_D3D1 && slot == 3){ *nargs = 2; *r = 0; return 1; }
    switch (slot){
    case 0: { *nargs = 3; uint32_t obj; if (d3d_query(c, A(c, 1), &obj)){ MR32(obj + 8)++; MR32(A(c, 2)) = obj; *r = 0; }
              else { MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; } return 1; }
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = MR32(self + 8) ? --MR32(self + 8) : 0; return 1;
    case 3: *nargs = 3; *r = enum_devices(c, A(c, 1), A(c, 2)); return 1;
    case 4: *nargs = 3; MR32(A(c, 1)) = com_object(FAM_LIGHT, 0); *r = 0; return 1;
    case 5: *nargs = 3; MR32(A(c, 1)) = new_material(family == FAM_D3D1 ? FAM_MATERIAL1 : FAM_MATERIAL2); *r = 0; return 1;
    case 6: *nargs = 3; MR32(A(c, 1)) = new_viewport(); *r = 0; return 1;
    case 7: {                                                /* FindDevice(search, result) */
        *nargs = 3; uint32_t res = A(c, 2);
        uint32_t size = MR32(res);
        memcpy(guest_ptr(res + 4, 16), IID_HAL, 16);
        if (size >= 4 + 16 + 2 * 204){ MR32(res + 20) = 204; fill_device_desc(res + 20, 1); MR32(res + 224) = 204; fill_device_desc(res + 224, 1); }
        *r = 0; return 1;
    }
    case 8:                                                  /* CreateDevice(clsid, surface, out) (IDirect3D2) */
        if (family != FAM_D3D2) return 0;
        *nargs = 4;
        { Surface *s = surf_of(A(c, 2)); if (!s) runtime_fail(c, "Direct3D device without a surface", A(c, 2));
          MR32(A(c, 3)) = create_device(s); *r = 0; }
        return 1;
    }
    return 0;
}

static int device_method(CPU *c, uint32_t self, unsigned slot, uint32_t *r, uint32_t *nargs){
    sync_trace();
    if (trace > 1 && (slot == 10 || slot == 11)) fprintf(stderr, "d3d %s\n", slot == 10 ? "BeginScene" : "EndScene");
    switch (slot){
    case 0: {
        *nargs = 3;
        if (!memcmp(g_mem + A(c, 1), IID_DEV1, 16)){
            if (!device1_obj) device1_obj = com_object(FAM_D3DDEVICE1, 0); else MR32(device1_obj + 8)++;
            MR32(A(c, 2)) = device1_obj; *r = 0; return 1;
        }
        if (!memcmp(g_mem + A(c, 1), IID_DEV2, 16)){ MR32(device_obj + 8)++; MR32(A(c, 2)) = device_obj; *r = 0; return 1; }
        fprintf(stderr, "Direct3D device: unavailable IID %08x-%04x\n", MR32(A(c, 1)), MR16(A(c, 1) + 4)); MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    }
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = MR32(self + 8) ? --MR32(self + 8) : 0; return 1;
    case 3: *nargs = 3; fill_device_desc(A(c, 1), 1); fill_device_desc(A(c, 2), 1); *r = 0; return 1;
    case 4: *nargs = 3; *r = 0; return 1;                    /* SwapTextureHandles */
    case 5: *nargs = 2; *r = 0; return 1;                    /* GetStats */
    case 6: case 7: *nargs = 2; *r = 0; return 1;            /* AddViewport, DeleteViewport */
    case 8: *nargs = 4; MR32(A(c, 2)) = 0; *r = 0; return 1;
    case 9: *nargs = 3; *r = enum_texture_formats(c, A(c, 1), A(c, 2)); return 1;
    case 10: rt_to_gpu(); *r = 0; return 1;                  /* BeginScene */
    /* EndScene: the game reads the back buffer through a pointer it keeps
       locked (video memory on Windows), so the finished scene goes back to
       guest memory here. */
    case 11: if (rt && rt->gpu_dirty) rt_lock(rt); *r = 0; return 1;
    case 12: *nargs = 2; if (!d3d2_obj) d3d2_obj = com_object(FAM_D3D2, 0); MR32(d3d2_obj + 8)++; MR32(A(c, 1)) = d3d2_obj; *r = 0; return 1;
    case 13: *nargs = 2; current_vp = vp_index(A(c, 1)); *r = 0; return 1;
    case 14: *nargs = 2; MR32(A(c, 1)) = current_vp >= 0 ? vps[current_vp].obj : 0; *r = 0; return 1;
    case 15: *nargs = 3; { Surface *s = surf_of(A(c, 1)); if (s) create_device(s); } *r = 0; return 1;
    case 16: *nargs = 2; MR32(A(c, 1)) = rt ? rt->obj : 0; if (rt) MR32(rt->obj + 8)++; *r = 0; return 1;
    case 17: case 18: {                                      /* Begin(type, vtype, flags) / BeginIndexed(type, vtype, verts, n, flags) */
        *nargs = slot == 17 ? 4 : 6;
        imm.active = 1; imm.indexed = slot == 18; imm.type = A(c, 1); imm.vtype = A(c, 2); imm.n = 0;
        if (slot == 18) runtime_fail(c, "BeginIndexed", A(c, 4));
        *r = 0; return 1;
    }
    case 19: {                                               /* Vertex(p) */
        *nargs = 2;
        if (imm.n == imm.cap){ imm.cap = imm.cap ? imm.cap * 2 : 256; imm.v = realloc(imm.v, imm.cap * sizeof *imm.v); }
        vertex_in(&imm.v[imm.n++], imm.vtype, A(c, 1));
        *r = 0; return 1;
    }
    case 20: *nargs = 2; runtime_fail(c, "Index", A(c, 1)); return 1;
    case 21: *nargs = 2; if (imm.active) draw(imm.type, imm.v, imm.n, NULL, 0); imm.active = 0; *r = 0; return 1;
    case 22: *nargs = 3; MR32(A(c, 2)) = A(c, 1) < 256 ? rs[A(c, 1)] : 0; *r = 0; return 1;
    case 23: *nargs = 3; if (A(c, 1) < 256) rs[A(c, 1)] = A(c, 2);
        if (trace > 0 && A(c, 1) != 1) fprintf(stderr, "d3d state %u = %08x\n", A(c, 1), A(c, 2));
        *r = 0; return 1;
    case 24: *nargs = 3; MR32(A(c, 2)) = 0; *r = 0; return 1;
    case 25: case 26: case 28: *nargs = 3; *r = 0; return 1;  /* SetLightState, SetTransform, MultiplyTransform */
    case 27: *nargs = 3; { float id[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; memcpy(guest_ptr(A(c, 2), 64), id, 64); } *r = 0; return 1;
    case 29: {                                               /* DrawPrimitive(type, vtype, verts, n, flags) */
        *nargs = 6;
        uint32_t vtype = A(c, 2), p = A(c, 3), n = A(c, 4);
        RenderVertex *v = malloc((n ? n : 1) * sizeof *v);
        for (uint32_t i = 0; i < n; i++) vertex_in(&v[i], vtype, p + i * vertex_size(vtype));
        draw(A(c, 1), v, n, NULL, 0); free(v);
        *r = 0; return 1;
    }
    case 30: {                                               /* DrawIndexedPrimitive(type, vtype, verts, n, idx, ni, flags) */
        *nargs = 8;
        uint32_t vtype = A(c, 2), p = A(c, 3), n = A(c, 4), ip = A(c, 5), ni = A(c, 6);
        RenderVertex *v = malloc((n ? n : 1) * sizeof *v);
        for (uint32_t i = 0; i < n; i++) vertex_in(&v[i], vtype, p + i * vertex_size(vtype));
        draw(A(c, 1), v, n, guest_ptr(ip, 2ull * ni), ni); free(v);
        *r = 0; return 1;
    }
    case 31: case 32: *nargs = 2; *r = 0; return 1;          /* Set/GetClipStatus */
    }
    return 0;
}

static int viewport_method(CPU *c, uint32_t self, unsigned slot, uint32_t *r, uint32_t *nargs){
    int i = vp_index(self);
    if (i < 0) runtime_fail(c, "invalid Direct3D viewport", self);
    switch (slot){
    case 0: *nargs = 3; MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = --MR32(self + 8); if (!*r){ vps[i].used = 0; if (current_vp == i) current_vp = -1; gheap_free(1, self); } return 1;
    case 3: *nargs = 2; *r = 0; return 1;
    case 4: case 16: {                                       /* GetViewport(2) */
        *nargs = 2; uint32_t p = A(c, 1);
        MR32(p + 4) = (uint32_t)vps[i].x; MR32(p + 8) = (uint32_t)vps[i].y; MR32(p + 12) = (uint32_t)vps[i].w; MR32(p + 16) = (uint32_t)vps[i].h;
        *r = 0; return 1;
    }
    case 5: case 17: {                                       /* SetViewport(2) */
        *nargs = 2; uint32_t p = A(c, 1);
        vps[i].x = (int32_t)MR32(p + 4); vps[i].y = (int32_t)MR32(p + 8); vps[i].w = (int32_t)MR32(p + 12); vps[i].h = (int32_t)MR32(p + 16);
        *r = 0; return 1;
    }
    case 6: *nargs = 6; runtime_fail(c, "TransformVertices", 0); return 1;
    case 7: *nargs = 4; *r = 0; return 1;
    case 8: *nargs = 2; vps[i].background = A(c, 1); *r = 0; return 1;
    case 9: *nargs = 3; MR32(A(c, 1)) = vps[i].background; if (A(c, 2)) MR32(A(c, 2)) = vps[i].background != 0; *r = 0; return 1;
    case 10: case 11: *nargs = 3; *r = 0; return 1;
    case 12: {                                               /* Clear(count, rects, flags) */
        *nargs = 4;
        if (trace > 1) fprintf(stderr, "d3d viewport Clear count=%u flags=%u\n", A(c, 1), A(c, 3));
        uint32_t color = 0xff000000u;
        uint32_t m = vps[i].background;
        if (m && m < MAT_MAX && mats[m].used){
            float *d = mats[m].diffuse;
            unsigned cr = (unsigned)(fminf(fmaxf(d[0], 0), 1) * 255), cg = (unsigned)(fminf(fmaxf(d[1], 0), 1) * 255), cb = (unsigned)(fminf(fmaxf(d[2], 0), 1) * 255);
            color = 0xff000000u | (cr << 16) | (cg << 8) | cb;
        }
        if (rt) clear(A(c, 1), A(c, 2), A(c, 3), color, 1.f);
        *r = 0; return 1;
    }
    case 13: case 14: *nargs = 2; *r = 0; return 1;
    case 15: *nargs = 4; MR32(A(c, 2)) = 0; *r = 0; return 1;
    }
    return 0;
}

static int material_method(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    uint32_t i = MR32(self + 32);
    if (i >= MAT_MAX || !mats[i].used) runtime_fail(c, "invalid Direct3D material", self);
    if (family == FAM_MATERIAL1){
        if (slot == 3){ *nargs = 2; *r = 0; return 1; }       /* Initialize */
        if (slot == 7 || slot == 8){ *r = 0; return 1; }      /* Reserve, Unreserve */
        if (slot >= 4) slot--;
    }
    switch (slot){
    case 0: *nargs = 3; MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = --MR32(self + 8); if (!*r){ mats[i].used = 0; gheap_free(1, self); } return 1;
    case 3: *nargs = 2; memcpy(mats[i].diffuse, guest_ptr(A(c, 1) + 4, 16), 16); mats[i].texture = MR32(A(c, 1) + 72); *r = 0; return 1;
    case 4: *nargs = 2; { uint32_t p = A(c, 1); memcpy(guest_ptr(p + 4, 16), mats[i].diffuse, 16); MR32(p + 72) = mats[i].texture; } *r = 0; return 1;
    case 5: *nargs = 3; MR32(A(c, 2)) = i; *r = 0; return 1;
    }
    return 0;
}

static int texture_method(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    Surface *t = texture_surface(self);
    if (family == FAM_TEXTURE1){
        if (slot == 3){ *nargs = 3; *r = 0; return 1; }       /* Initialize */
        if (slot == 7){ *r = 0; return 1; }                   /* Unload */
        if (slot >= 4) slot--;
    }
    switch (slot){
    case 0: *nargs = 3; MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = --MR32(self + 8); if (!*r){ if (t) MR32(t->obj + 8)--; gheap_free(1, self); } return 1;
    case 3: *nargs = 3; MR32(A(c, 2)) = t ? (uint32_t)(t - g_surf) : 0; *r = 0; return 1;   /* GetHandle */
    case 4: *nargs = 3; if (t) t->texture_version++; *r = 0; return 1;                      /* PaletteChanged */
    case 5: {                                                /* Load(source texture) */
        *nargs = 2;
        Surface *src = texture_surface(A(c, 1));
        if (t && src && src->pitch * src->h <= t->pitch * t->h + 16 && src->bpp == t->bpp){
            for (unsigned y = 0; y < src->h && y < t->h; y++)
                memcpy(g_mem + t->pixels + (size_t)y * t->pitch, g_mem + src->pixels + (size_t)y * src->pitch, src->pitch < t->pitch ? src->pitch : t->pitch);
            t->has_ck = src->has_ck; t->ck_lo = src->ck_lo; t->ck_hi = src->ck_hi; t->texture_version++;
        }
        *r = 0; return 1;
    }
    }
    return 0;
}

/* IDirect3DDevice (v1): the same device; slots that exist in IDirect3DDevice2 map onto it. */
static int device1_method(CPU *c, uint32_t self, unsigned slot, uint32_t *r, uint32_t *nargs){
    static const int map[22] = {0, 1, 2, -1, 3, 4, -1, 5, -1, 6, 7, 8, -1, -1, 9, -1, -1, -1, -1, 10, 11, 12};
    if (slot == 3){ *nargs = 4; *r = 0; return 1; }                         /* Initialize */
    if (slot == 15){ *nargs = 2; MR32(A(c, 1)) = 1; *r = 0; return 1; }     /* CreateMatrix */
    if (slot == 16 || slot == 17){ *nargs = 3; *r = 0; return 1; }          /* Set/GetMatrix */
    if (slot == 18){ *nargs = 2; *r = 0; return 1; }                        /* DeleteMatrix */
    if (slot == 2){ *r = MR32(self + 8) ? --MR32(self + 8) : 0; return 1; }
    if (slot == 1){ *r = ++MR32(self + 8); return 1; }
    if (slot < 22 && map[slot] >= 0) return device_method(c, device_obj, (unsigned)map[slot], r, nargs);
    return 0;
}

int d3d_method(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    switch (family){
    case FAM_D3DDEVICE1: return device1_method(c, self, slot, r, nargs);
    case FAM_D3D1: case FAM_D3D2: return d3d_object(c, self, family, slot, r, nargs);
    case FAM_D3DDEVICE2: return device_method(c, self, slot, r, nargs);
    case FAM_VIEWPORT2: return viewport_method(c, self, slot, r, nargs);
    case FAM_MATERIAL1: case FAM_MATERIAL2: return material_method(c, self, family, slot, r, nargs);
    case FAM_TEXTURE1: case FAM_TEXTURE2: return texture_method(c, self, family, slot, r, nargs);
    case FAM_LIGHT:
        if (slot == 1){ *r = ++MR32(self + 8); return 1; }
        if (slot == 2){ *r = MR32(self + 8) ? --MR32(self + 8) : 0; return 1; }
        *nargs = 2; *r = 0; return 1;
    }
    return 0;
}
