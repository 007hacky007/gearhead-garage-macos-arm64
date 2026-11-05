#ifndef GEARHEAD_DDRAW_H
#define GEARHEAD_DDRAW_H
#include "cpu.h"
enum { FAM_DDRAW = 3, FAM_SURFACE = 6, FAM_PALETTE = 7, FAM_CLIPPER = 8, FAM_DDRAW2 = 9,
       FAM_D3D2 = 10, FAM_D3DDEVICE2 = 11, FAM_VIEWPORT2 = 12, FAM_TEXTURE2 = 13, FAM_MATERIAL2 = 14,
       FAM_D3D1 = 15, FAM_TEXTURE1 = 16, FAM_LIGHT = 17, FAM_MATERIAL1 = 18, FAM_D3DDEVICE1 = 19 };
#define SURF_MAX 4096
typedef struct Surface {
    int used, front_owner, zbuffer, has_ck, locked;
    unsigned w, h, bpp, pitch, back_count;
    uint32_t obj, pixels, caps, back, zbuf, palette, clipper, ck_lo, ck_hi;
    uint32_t rmask, gmask, bmask, amask;
    uint32_t texture;           /* native texture handle (d3d.c) */
    uint32_t texture_version, uploaded_version;
    int gpu_dirty;              /* render target: GPU copy newer than guest pixels */
    void (*lock_hook)(struct Surface *);
    void (*unlock_hook)(struct Surface *);
    void (*release_hook)(struct Surface *);
    void (*depth_fill)(struct Surface *, const int *, uint32_t);
} Surface;
extern Surface g_surf[SURF_MAX];
Surface *surf_of(uint32_t self);
void ddraw_present(Surface *);
unsigned ddraw_mode_width(void);
unsigned ddraw_mode_height(void);
extern int (*ddraw_present_hook)(Surface *);
extern int (*ddraw_blt_hook)(Surface *, const int *, Surface *, const int *, int);
extern int (*ddraw_flip_hook)(Surface *, Surface *);
extern int (*ddraw_fill_hook)(Surface *, const int *, uint32_t);
#endif
