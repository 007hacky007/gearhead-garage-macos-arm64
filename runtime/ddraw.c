/* DirectDraw (DX5 interfaces) for the Gearhead Garage video drivers.
 * Surfaces live in guest memory; the primary surface is converted to BGRA
 * and shown through the Metal presenter whenever it changes. */
#include "runtime.h"
#include "render.h"
#include "ddraw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t com_object(unsigned, unsigned);
void gheap_free(int, uint32_t);
static uint32_t A(CPU *c, unsigned i){ return MR32(c->esp + 4 * i); }

#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_INVALIDPARAMS E_INVALIDARG
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTLOCKED 0x887602d2u   /* 0x88760000 | 584 */
#define DDERR_NOPALETTEATTACHED 0x8876023cu
#define DDERR_NOCLIPPERATTACHED 0x88760230u
#define DDERR_NOCOLORKEY 0x887600d7u

Surface g_surf[SURF_MAX];
static struct { uint8_t rgb[256][4]; uint32_t obj; int used; } pals[64];
static unsigned mode_w = 640, mode_h = 480, mode_bpp = 8;
static int window_ready;
static uint32_t dd_object, dd2_object;

Surface *surf_of(uint32_t self){
    if (!self || !mem_valid(self, 40) || MR32(self + 4) != FAM_SURFACE) return NULL;
    uint32_t i = MR32(self + 32);
    return i < SURF_MAX && g_surf[i].used ? &g_surf[i] : NULL;
}

static uint32_t pixel_bytes(unsigned bpp){ return bpp <= 8 ? 1 : bpp == 16 ? 2 : bpp == 24 ? 3 : 4; }

static void fill_format(uint32_t p, const Surface *s){
    memset(guest_ptr(p, 32), 0, 32);
    MR32(p) = 32; MR32(p + 12) = s->bpp;
    if (s->zbuffer){ MR32(p + 4) = 0x400; MR32(p + 16) = 0xffff; return; }
    if (s->bpp == 8){ MR32(p + 4) = 0x60; return; }   /* RGB | PALETTEINDEXED8 */
    MR32(p + 4) = 0x40 | (s->amask ? 1 : 0);
    MR32(p + 16) = s->rmask; MR32(p + 20) = s->gmask; MR32(p + 24) = s->bmask; MR32(p + 28) = s->amask;
}

static void default_masks(Surface *s){
    if (s->bpp == 16){ s->rmask = 0xf800; s->gmask = 0x07e0; s->bmask = 0x001f; s->amask = 0; }
    else if (s->bpp == 32 || s->bpp == 24){ s->rmask = 0xff0000; s->gmask = 0xff00; s->bmask = 0xff; s->amask = 0; }
}

static void fill_desc(uint32_t p, const Surface *s){
    uint32_t size = MR32(p);
    if (size < 108) size = 108;
    memset(guest_ptr(p, size), 0, size);
    MR32(p) = size;
    MR32(p + 4) = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s->back_count ? 0x20 : 0);
    MR32(p + 8) = s->h; MR32(p + 12) = s->w; MR32(p + 16) = s->pitch; MR32(p + 20) = s->back_count;
    if (s->has_ck){ MR32(p + 4) |= 0x10000; MR32(p + 64) = s->ck_lo; MR32(p + 68) = s->ck_hi; }
    fill_format(p + 72, s);
    MR32(p + 104) = s->caps;
}

static uint32_t new_surface(unsigned w, unsigned h, unsigned bpp, uint32_t caps){
    unsigned i;
    for (i = 1; i < SURF_MAX && g_surf[i].used; i++);
    if (i == SURF_MAX) return 0;
    Surface *s = &g_surf[i];
    memset(s, 0, sizeof *s);
    s->used = 1; s->w = w; s->h = h; s->bpp = bpp; s->caps = caps;
    s->pitch = (w * pixel_bytes(bpp) + 3) & ~3u;
    default_masks(s);
    s->pixels = win32_alloc(s->pitch * h + 16, 1);
    s->obj = com_object(FAM_SURFACE, 0);
    MR32(s->obj + 32) = i;
    return s->obj;
}

static void free_surface(Surface *s){
    if (s->back){ Surface *b = surf_of(s->back); if (b && b->front_owner) free_surface(b); }
    if (s->pixels) gheap_free(1, s->pixels);
    gheap_free(1, s->obj);
    if (s->release_hook) s->release_hook(s);
    memset(s, 0, sizeof *s);
}

static void ensure_window(void){
    if (window_ready) return;
    if (!render_mode(mode_w, mode_h)) runtime_fail(g_cur_cpu, "cannot open the game window", mode_w);
    window_ready = 1;
}

/* BGRA of one surface (8-bit through its palette, 16-bit 565/555). */
static void convert(const Surface *s, uint32_t *out){
    const uint8_t *src = g_mem + s->pixels;
    const uint8_t (*pal)[4] = NULL;
    uint32_t lut[256];
    if (s->bpp == 8){
        int k = -1;
        for (unsigned i = 0; i < 64; i++) if (pals[i].used && pals[i].obj == s->palette){ k = (int)i; break; }
        if (k >= 0) pal = (const uint8_t (*)[4])pals[k].rgb;
        for (unsigned i = 0; i < 256; i++)
            lut[i] = pal ? 0xff000000u | (pal[i][0] << 16) | (pal[i][1] << 8) | pal[i][2] : 0xff000000u | i * 0x010101u;
    }
    for (unsigned y = 0; y < s->h; y++){
        const uint8_t *row = src + (size_t)y * s->pitch;
        uint32_t *o = out + (size_t)y * s->w;
        if (s->bpp == 8) for (unsigned x = 0; x < s->w; x++) o[x] = lut[row[x]];
        else if (s->bpp == 16){
            int g6 = s->gmask == 0x07e0;
            for (unsigned x = 0; x < s->w; x++){
                uint16_t v; memcpy(&v, row + 2 * x, 2);
                unsigned r, g, b;
                if (g6){ r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; g = (g << 2) | (g >> 4); }
                else { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; g = (g << 3) | (g >> 2); }
                r = (r << 3) | (r >> 2); b = (b << 3) | (b >> 2);
                o[x] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }
        else for (unsigned x = 0; x < s->w; x++){ uint32_t v; memcpy(&v, row + 4 * x, 4); o[x] = 0xff000000u | v; }
    }
}

int (*ddraw_present_hook)(Surface *);
extern int (*ddraw_fill_hook)(Surface *, const int *, uint32_t);
void ddraw_present(Surface *s){
    if (!s || !(s->caps & 0x200)) return;
    if (getenv("GEARHEAD_PRESENT_TRACE")) fprintf(stderr, "present primary %u locked=%d\n", (unsigned)(s - g_surf), s->locked);
    ensure_window();
    if (s->lock_hook) s->lock_hook(s);
    if (ddraw_present_hook && ddraw_present_hook(s)) return;
    static uint32_t *frame; static size_t frame_size;
    size_t need = (size_t)s->w * s->h * 4;
    if (need > frame_size){ free(frame); frame = malloc(need); frame_size = need; }
    convert(s, frame);
    render_write(0, 0, s->w, s->h, frame, 1);
    render_present(1);
}

/* ---- blitting ---- */
static int clip_rect(const Surface *s, uint32_t rp, int r[4]){
    if (rp){ guest_ptr(rp, 16); r[0] = (int32_t)MR32(rp); r[1] = (int32_t)MR32(rp + 4); r[2] = (int32_t)MR32(rp + 8); r[3] = (int32_t)MR32(rp + 12); }
    else { r[0] = 0; r[1] = 0; r[2] = (int)s->w; r[3] = (int)s->h; }
    return r[2] > r[0] && r[3] > r[1];
}

static void fill(Surface *d, const int r[4], uint32_t color){
    unsigned bpp = pixel_bytes(d->bpp);
    if (ddraw_fill_hook && ddraw_fill_hook(d, r, color)) return;
    if (d->lock_hook) d->lock_hook(d);
    d->texture_version++;
    int x0 = r[0] < 0 ? 0 : r[0], y0 = r[1] < 0 ? 0 : r[1];
    int x1 = r[2] > (int)d->w ? (int)d->w : r[2], y1 = r[3] > (int)d->h ? (int)d->h : r[3];
    for (int y = y0; y < y1; y++){
        uint8_t *row = g_mem + d->pixels + (size_t)y * d->pitch;
        for (int x = x0; x < x1; x++){
            if (bpp == 1) row[x] = (uint8_t)color;
            else if (bpp == 2){ uint16_t v = (uint16_t)color; memcpy(row + 2 * x, &v, 2); }
            else memcpy(row + bpp * x, &color, bpp);
        }
    }
    if (d->unlock_hook) d->unlock_hook(d);
}

/* Stretching copy with an optional source colour key. */
static void copy(Surface *d, const int dr[4], Surface *s, const int sr[4], int key){
    if (pixel_bytes(d->bpp) != pixel_bytes(s->bpp)) return;
    if (s->lock_hook) s->lock_hook(s);
    if (d->lock_hook) d->lock_hook(d);
    unsigned bpp = pixel_bytes(d->bpp);
    int dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int same = d == s;
    uint8_t *tmp = NULL;
    if (same){
        tmp = malloc((size_t)s->pitch * s->h);
        memcpy(tmp, g_mem + s->pixels, (size_t)s->pitch * s->h);
    }
    const uint8_t *src = same ? tmp : g_mem + s->pixels;
    for (int y = 0; y < dh; y++){
        int dy = dr[1] + y; if (dy < 0 || dy >= (int)d->h) continue;
        int sy = sr[1] + (int)((int64_t)y * sh / dh); if (sy < 0 || sy >= (int)s->h) continue;
        uint8_t *drow = g_mem + d->pixels + (size_t)dy * d->pitch;
        const uint8_t *srow = src + (size_t)sy * s->pitch;
        if (!key && dw == sw && dr[0] >= 0 && dr[2] <= (int)d->w && sr[0] >= 0 && sr[2] <= (int)s->w){
            memmove(drow + bpp * dr[0], srow + bpp * sr[0], (size_t)bpp * dw); continue;
        }
        for (int x = 0; x < dw; x++){
            int dx = dr[0] + x; if (dx < 0 || dx >= (int)d->w) continue;
            int sx = sr[0] + (int)((int64_t)x * sw / dw); if (sx < 0 || sx >= (int)s->w) continue;
            uint32_t v = 0; memcpy(&v, srow + bpp * sx, bpp);
            if (key && v >= s->ck_lo && v <= s->ck_hi) continue;
            memcpy(drow + bpp * dx, &v, bpp);
        }
    }
    free(tmp);
    if (d->unlock_hook) d->unlock_hook(d);
    d->texture_version++;
}

static int mode_callback(CPU *c, uint32_t fn, uint32_t ctx, unsigned w, unsigned h, unsigned bpp){
    uint32_t p = win32_alloc(108, 1);
    Surface tmp = { .w = w, .h = h, .bpp = bpp, .pitch = w * pixel_bytes(bpp) };
    default_masks(&tmp);
    MR32(p) = 108; fill_desc(p, &tmp); MR32(p + 4) = 0x1006 | 0x8 | 0x40000; MR32(p + 104) = 0; MR32(p + 24) = 60;
    uint32_t args[] = { p, ctx };
    uint32_t r = call_guest(c, fn, 2, args);
    gheap_free(1, p);
    return (int)r;
}

static uint32_t dd_query(CPU *c, uint32_t self, uint32_t iid, uint32_t out){
    uint32_t first = MR32(iid);
    uint32_t r = 0, obj = 0;
    if (first == 0x6c14db80u){ obj = dd_object ? dd_object : self; }
    else if (first == 0xb3a6f3e0u){ if (!dd2_object) dd2_object = com_object(FAM_DDRAW2, 0); obj = dd2_object; }
    else {
        int d3d_query(CPU *, uint32_t, uint32_t *);
        if (!d3d_query(c, iid, &obj)){ fprintf(stderr, "DirectDraw: unavailable IID %08x\n", first); r = E_NOINTERFACE; }
    }
    if (obj && obj != self) MR32(obj + 8)++;
    else if (obj) MR32(self + 8)++;
    MR32(out) = obj;
    return r;
}

int ddraw_platform_call(CPU *c, const char *name, uint32_t *out){
    if (!strcmp(name, "DirectDrawCreate")){
        uint32_t p = A(c, 1);
        if (!dd_object) dd_object = com_object(FAM_DDRAW, 0);
        else MR32(dd_object + 8)++;
        MR32(p) = dd_object; *out = 0; return 1;
    }
    if (!strcmp(name, "DirectDrawEnumerateA")){
        uint32_t desc = win32_alloc(64, 1), drv = win32_alloc(64, 1);
        strcpy((char *)g_mem + desc, "Apple Metal"); strcpy((char *)g_mem + drv, "display");
        uint32_t args[] = { 0, desc, drv, A(c, 1) };
        call_guest(c, A(c, 0), 4, args);
        gheap_free(1, desc); gheap_free(1, drv); *out = 0; return 1;
    }
    return 0;
}

static int ddraw_object(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    switch (slot){
    case 0: *nargs = 3; *r = dd_query(c, self, A(c, 1), A(c, 2)); return 1;
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = MR32(self + 8) ? --MR32(self + 8) : 0; return 1;
    case 3: *r = 0; return 1;                                   /* Compact */
    case 4: *nargs = 4; MR32(A(c, 2)) = com_object(FAM_CLIPPER, 0); *r = 0; return 1;
    case 5: {                                                   /* CreatePalette(flags, entries, out, outer) */
        *nargs = 5;
        unsigned k; for (k = 0; k < 64 && pals[k].used; k++);
        if (k == 64){ *r = 0x8007000eu; return 1; }
        pals[k].used = 1; pals[k].obj = com_object(FAM_PALETTE, 0); MR32(pals[k].obj + 32) = k;
        if (A(c, 2)) memcpy(pals[k].rgb, guest_ptr(A(c, 2), 1024), 1024);
        MR32(A(c, 3)) = pals[k].obj; *r = 0; return 1;
    }
    case 6: {                                                   /* CreateSurface(desc, out, outer) */
        *nargs = 4;
        uint32_t d = A(c, 1), flags = MR32(d + 4), caps = (flags & 1) ? MR32(d + 104) : 0;
        unsigned w = (flags & 4) ? MR32(d + 12) : mode_w, h = (flags & 2) ? MR32(d + 8) : mode_h;
        unsigned bpp = mode_bpp;
        int z = !!(caps & 0x20000);
        if (flags & 0x1000){
            uint32_t pf = d + 72, pff = MR32(pf + 4);
            if (pff & 0x400) z = 1;
            bpp = MR32(pf + 12) ? MR32(pf + 12) : bpp;
        }
        if (z && (flags & 0x40)) bpp = MR32(d + 24) ? MR32(d + 24) : 16;
        if (z && !(flags & 0x1000) && !(flags & 0x40)) bpp = 16;
        if (caps & 0x200){ w = mode_w; h = mode_h; bpp = mode_bpp; }
        uint32_t obj = new_surface(w, h, bpp, caps | ((caps & 0x200) ? 0x4000 | 0x10000000 | 0x8000 : 0));
        if (!obj){ *r = 0x8007000eu; return 1; }
        Surface *s = surf_of(obj);
        s->zbuffer = z;
        if ((flags & 0x1000) && !z){
            uint32_t pf = d + 72;
            if (MR32(pf + 4) & 0x40){ s->rmask = MR32(pf + 16); s->gmask = MR32(pf + 20); s->bmask = MR32(pf + 24); s->amask = (MR32(pf + 4) & 1) ? MR32(pf + 28) : 0; }
        }
        if (flags & 0x10000){ s->has_ck = 1; s->ck_lo = MR32(d + 64); s->ck_hi = MR32(d + 68); }
        if ((caps & 0x200) && (flags & 0x20) && MR32(d + 20)){
            s->caps |= 0x20;
            s->back_count = MR32(d + 20);
            uint32_t back = new_surface(w, h, bpp, (caps & ~0x200u & ~0x20u) | 0x4 | 0x4000);
            Surface *b = surf_of(back); b->front_owner = 1; s = surf_of(obj); s->back = back;
            b->rmask = s->rmask; b->gmask = s->gmask; b->bmask = s->bmask;
        }
        if (caps & 0x200) ensure_window();
        if (getenv("GEARHEAD_DDRAW_TRACE")) fprintf(stderr, "CreateSurface %ux%u %ubpp caps=%08x -> %08x\n", w, h, bpp, caps, obj);
        MR32(A(c, 2)) = obj; *r = 0; return 1;
    }
    case 8: {                                                   /* EnumDisplayModes(flags, desc, ctx, cb) */
        *nargs = 5;
        static const unsigned modes[][2] = {{640,480},{800,600},{1024,768}};
        static const unsigned depths[] = {8, 16};
        uint32_t want = A(c, 2);
        for (unsigned di = 0; di < 2; di++) for (unsigned i = 0; i < 3; i++){
            if (want && (MR32(want + 4) & 0x1000) && MR32(want + 72 + 12) != depths[di]) continue;
            if (!mode_callback(c, A(c, 4), A(c, 3), modes[i][0], modes[i][1], depths[di])) { *r = 0; return 1; }
        }
        *r = 0; return 1;
    }
    case 11: {                                                  /* GetCaps(driver, hel) */
        *nargs = 3;
        for (unsigned k = 1; k <= 2; k++){
            uint32_t p = A(c, k); if (!p) continue;
            uint32_t size = MR32(p); if (size < 316) size = 316;
            memset(guest_ptr(p, size), 0, size); MR32(p) = size;
            MR32(p + 4) = 0x40 | 0x4000000 | 0x400000 | 0x8000000 | 0x40000 | 0x1;   /* BLT, COLORFILL, COLORKEY, PALETTE, BLTSTRETCH, 3D */
            MR32(p + 8) = 0x2000;                                                  /* NOPAGELOCKREQUIRED */
            MR32(p + 12) = 0x200;                                                  /* CKEY_SRCBLT */
            MR32(p + 24) = 0x4 | 0x40;                                             /* 8BIT palettes, PRIMARYSURFACE */
            MR32(p + 56) = 0x400;                                                  /* 16-bit z */
            MR32(p + 60) = 64u << 20; MR32(p + 64) = 60u << 20;
            MR32(p + 132) = 0x1000 | 0x2000 | 0x4000 | 0x200 | 0x40 | 0x20000 | 0x10 | 0x8 | 0x20 | 0x4;
        }
        *r = 0; return 1;
    }
    case 12: {                                                  /* GetDisplayMode(desc) */
        *nargs = 2; Surface tmp = { .w = mode_w, .h = mode_h, .bpp = mode_bpp, .pitch = mode_w * pixel_bytes(mode_bpp) };
        default_masks(&tmp); fill_desc(A(c, 1), &tmp); MR32(A(c, 1) + 24) = 60; *r = 0; return 1;
    }
    case 13: *nargs = 3; if (A(c, 1)) MR32(A(c, 1)) = 0; *r = 0; return 1;  /* GetFourCCCodes */
    case 15: *nargs = 2; MR32(A(c, 1)) = 60; *r = 0; return 1;
    case 16: *nargs = 2; MR32(A(c, 1)) = 0; *r = 0; return 1;
    case 17: *nargs = 2; MR32(A(c, 1)) = 1; *r = 0; return 1;
    case 19: *r = 0; return 1;                                  /* RestoreDisplayMode */
    case 20: *nargs = 3; *r = 0; return 1;                      /* SetCooperativeLevel */
    case 21:                                                    /* SetDisplayMode */
        *nargs = family == FAM_DDRAW ? 4 : 6;
        mode_w = A(c, 1); mode_h = A(c, 2); mode_bpp = A(c, 3);
        fprintf(stderr, "DirectDraw display mode %ux%ux%u\n", mode_w, mode_h, mode_bpp);
        window_ready = 0; ensure_window(); *r = 0; return 1;
    case 22: *nargs = 3; *r = 0; return 1;                      /* WaitForVerticalBlank */
    case 23:                                                    /* GetAvailableVidMem (IDirectDraw2) */
        if (family != FAM_DDRAW2) return 0;
        *nargs = 4; if (A(c, 2)) MR32(A(c, 2)) = 64u << 20; if (A(c, 3)) MR32(A(c, 3)) = 60u << 20; *r = 0; return 1;
    }
    return 0;
}

static int surface_method(CPU *c, uint32_t self, unsigned slot, uint32_t *r, uint32_t *nargs){
    Surface *s = surf_of(self);
    if (!s) runtime_fail(c, "invalid DirectDraw surface", self);
    static int dtrace = -1; if (dtrace < 0){ const char *e = getenv("GEARHEAD_DDRAW_TRACE"); dtrace = e ? atoi(e) : 0; }
    if (dtrace > 1 && (slot == 5 || slot == 7 || slot == 11 || slot == 25 || slot == 32))
        fprintf(stderr, "surface %u (%ux%u caps %x) slot %u args %08x %08x %08x %08x %08x\n", (unsigned)(s - g_surf), s->w, s->h, s->caps, slot, A(c, 1), A(c, 2), A(c, 3), A(c, 4), A(c, 5));
    switch (slot){
    case 0: {
        *nargs = 3; uint32_t first = MR32(A(c, 1));
        if (first == 0x6c14db81u || first == 0x57805885u || first == 0xda044e00u){ MR32(self + 8)++; MR32(A(c, 2)) = self; *r = 0; return 1; }
        int d3d_surface_query(CPU *, Surface *, uint32_t, uint32_t *);
        uint32_t obj = 0;
        if (d3d_surface_query(c, s, A(c, 1), &obj)){ MR32(A(c, 2)) = obj; *r = obj ? 0 : E_NOINTERFACE; return 1; }
        fprintf(stderr, "DirectDraw surface: unavailable IID %08x\n", first);
        MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    }
    case 1: *r = ++MR32(self + 8); return 1;
    case 2:
        *r = --MR32(self + 8);
        if (!*r && !s->front_owner) free_surface(s);
        else if (!*r) MR32(self + 8) = 1;
        return 1;
    case 3: *nargs = 2; {                                       /* AddAttachedSurface */
        Surface *z = surf_of(A(c, 1)); if (z && z->zbuffer) s->zbuf = A(c, 1); else if (z) s->back = A(c, 1);
        if (z) MR32(A(c, 1) + 8)++;
        *r = 0; return 1; }
    case 5: {                                                   /* Blt(dst, src, srcrect, flags, fx) */
        *nargs = 6;
        uint32_t flags = A(c, 4), fx = A(c, 5);
        int dr[4]; if (!clip_rect(s, A(c, 1), dr)){ *r = 0; return 1; }
        if (flags & 0x400){ fill(s, dr, MR32(fx + 80)); }
        else if (flags & 0x2000000){ if (s->depth_fill) s->depth_fill(s, dr, MR32(fx + 80)); else fill(s, dr, MR32(fx + 80)); }
        else {
            Surface *src = surf_of(A(c, 2));
            if (src){
                int sr[4]; clip_rect(src, A(c, 3), sr);
                if (ddraw_blt_hook && ddraw_blt_hook(s, dr, src, sr, (flags & 0x8000) != 0)) {}
                else copy(s, dr, src, sr, (flags & 0x8000) && src->has_ck);
            }
        }
        if (s->caps & 0x200) ddraw_present(s);
        *r = 0; return 1;
    }
    case 7: {                                                   /* BltFast(x, y, src, rect, trans) */
        *nargs = 6;
        Surface *src = surf_of(A(c, 3));
        if (src){
            int sr[4]; clip_rect(src, A(c, 4), sr);
            int dr[4] = { (int32_t)A(c, 1), (int32_t)A(c, 2), 0, 0 };
            dr[2] = dr[0] + sr[2] - sr[0]; dr[3] = dr[1] + sr[3] - sr[1];
            if (ddraw_blt_hook && ddraw_blt_hook(s, dr, src, sr, (A(c, 5) & 1) != 0)) {}
            else copy(s, dr, src, sr, (A(c, 5) & 1) && src->has_ck);
            if (s->caps & 0x200) ddraw_present(s);
        }
        *r = 0; return 1;
    }
    case 8: *nargs = 3; *r = 0; return 1;                       /* DeleteAttachedSurface */
    case 11: {                                                  /* Flip(target, flags) */
        *nargs = 3;
        Surface *b = surf_of(s->back);
        if (b && ddraw_flip_hook && ddraw_flip_hook(s, b)){ *r = 0; return 1; }
        if (b){ uint32_t t = s->pixels; s->pixels = b->pixels; b->pixels = t; }
        ddraw_present(s);
        *r = 0; return 1;
    }
    case 12: {                                                  /* GetAttachedSurface(caps, out) */
        *nargs = 3; uint32_t want = MR32(A(c, 1));
        uint32_t found = 0;
        if ((want & 0x20000) && s->zbuf) found = s->zbuf;
        else if ((want & 0x4) && s->back) found = s->back;
        else if ((want & 0x2000) && s->back) found = s->back;
        if (!found){ MR32(A(c, 2)) = 0; *r = DDERR_NOTFOUND; return 1; }
        MR32(found + 8)++; MR32(A(c, 2)) = found; *r = 0; return 1;
    }
    case 13: case 18: *nargs = 2; *r = 0; return 1;             /* GetBltStatus, GetFlipStatus */
    case 14: *nargs = 2; MR32(A(c, 1)) = s->caps; *r = 0; return 1;
    case 15: *nargs = 2; MR32(A(c, 1)) = s->clipper; if (s->clipper) MR32(s->clipper + 8)++; *r = s->clipper ? 0 : DDERR_NOCLIPPERATTACHED; return 1;
    case 16: *nargs = 3; if (!s->has_ck){ *r = DDERR_NOCOLORKEY; return 1; } MR32(A(c, 2)) = s->ck_lo; MR32(A(c, 2) + 4) = s->ck_hi; *r = 0; return 1;
    case 20: *nargs = 2; MR32(A(c, 1)) = s->palette; if (s->palette) MR32(s->palette + 8)++; *r = s->palette ? 0 : DDERR_NOPALETTEATTACHED; return 1;
    case 21: *nargs = 2; fill_format(A(c, 1), s); *r = 0; return 1;
    case 22: *nargs = 2; fill_desc(A(c, 1), s); MR32(A(c, 1) + 36) = 0; *r = 0; return 1;
    case 24: case 27: *r = 0; return 1;                         /* IsLost, Restore */
    case 25: {                                                  /* Lock(rect, desc, flags, event) */
        *nargs = 5;
        uint32_t d = A(c, 2), rp = A(c, 1);
        if (s->lock_hook) s->lock_hook(s);
        fill_desc(d, s);
        uint32_t ptr = s->pixels;
        if (rp) ptr += MR32(rp + 4) * s->pitch + MR32(rp) * pixel_bytes(s->bpp);
        MR32(d + 4) |= 0x800; MR32(d + 36) = ptr; s->locked++;
        *r = 0; return 1;
    }
    case 28: *nargs = 2; s->clipper = A(c, 1); *r = 0; return 1;
    case 29: *nargs = 3;                                        /* SetColorKey(flags, key) */
        if (A(c, 2)){ s->has_ck = 1; s->ck_lo = MR32(A(c, 2)); s->ck_hi = MR32(A(c, 2) + 4); } else s->has_ck = 0;
        *r = 0; return 1;
    case 31: *nargs = 2; s->palette = A(c, 1); if (s->palette) MR32(s->palette + 8)++;
        if (s->caps & 0x200) ddraw_present(s);
        *r = 0; return 1;
    case 32:                                                    /* Unlock(ptr) */
        *nargs = 2;
        /* The game unlocks the front buffer after locking only the back one. */
        if (!s->locked){ if (getenv("GEARHEAD_PRESENT_TRACE")) fprintf(stderr, "unlock of unlocked surface %u\n", (unsigned)(s - g_surf)); *r = 0x887601b8u; return 1; }
        s->locked--;
        if (s->unlock_hook) s->unlock_hook(s);
        s->texture_version++;
        if (s->caps & 0x200) ddraw_present(s);
        *r = 0; return 1;
    case 36: *nargs = 2; MR32(A(c, 1)) = dd_object; if (dd_object) MR32(dd_object + 8)++; *r = 0; return 1;
    case 37: case 38: *nargs = 2; *r = 0; return 1;             /* PageLock, PageUnlock */
    }
    return 0;
}

static int palette_method(CPU *c, uint32_t self, unsigned slot, uint32_t *r, uint32_t *nargs){
    uint32_t k = MR32(self + 32);
    if (k >= 64 || !pals[k].used) runtime_fail(c, "invalid DirectDraw palette", self);
    switch (slot){
    case 0: *nargs = 3; MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = --MR32(self + 8); if (!*r){ pals[k].used = 0; gheap_free(1, self); } return 1;
    case 3: *nargs = 2; MR32(A(c, 1)) = 0x4 | 0x40; *r = 0; return 1;
    case 4: case 6: {
        *nargs = 5; uint32_t base = A(c, 2), n = A(c, 3), p = A(c, 4);
        if (base > 256 || n > 256 - base){ *r = E_INVALIDARG; return 1; }
        if (slot == 4) memcpy(guest_ptr(p, 4 * n), pals[k].rgb[base], 4 * n);
        else {
            memcpy(pals[k].rgb[base], guest_ptr(p, 4 * n), 4 * n);
            for (unsigned i = 1; i < SURF_MAX; i++)
                if (g_surf[i].used && (g_surf[i].caps & 0x200) && g_surf[i].palette == self) ddraw_present(&g_surf[i]);
        }
        *r = 0; return 1;
    }
    }
    return 0;
}

static int clipper_method(CPU *c, uint32_t self, unsigned slot, uint32_t *r, uint32_t *nargs){
    switch (slot){
    case 0: *nargs = 3; MR32(A(c, 2)) = 0; *r = E_NOINTERFACE; return 1;
    case 1: *r = ++MR32(self + 8); return 1;
    case 2: *r = --MR32(self + 8); if (!*r) gheap_free(1, self); return 1;
    case 4: *nargs = 2; MR32(A(c, 1)) = MR32(self + 36); *r = 0; return 1;
    case 6: *nargs = 2; MR32(A(c, 1)) = 0; *r = 0; return 1;
    case 7: *nargs = 3; *r = 0; return 1;
    case 8: *nargs = 3; MR32(self + 36) = A(c, 2); *r = 0; return 1;
    }
    return 0;
}

int ddraw_method(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    *nargs = 1;
    if (family == FAM_DDRAW || family == FAM_DDRAW2) return ddraw_object(c, self, family, slot, r, nargs);
    if (family == FAM_SURFACE) return surface_method(c, self, slot, r, nargs);
    if (family == FAM_PALETTE) return palette_method(c, self, slot, r, nargs);
    if (family == FAM_CLIPPER) return clipper_method(c, self, slot, r, nargs);
    int d3d_method(CPU *, uint32_t, unsigned, unsigned, uint32_t *, uint32_t *);
    return d3d_method(c, self, family, slot, r, nargs);
}

unsigned ddraw_mode_width(void){ return mode_w; }
unsigned ddraw_mode_height(void){ return mode_h; }
int (*ddraw_blt_hook)(Surface *, const int *, Surface *, const int *, int);
int (*ddraw_flip_hook)(Surface *, Surface *);
int (*ddraw_fill_hook)(Surface *, const int *, uint32_t);
