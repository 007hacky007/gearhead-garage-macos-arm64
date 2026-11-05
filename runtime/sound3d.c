/* DirectSound 3D listener and buffers: distance attenuation and stereo pan. */
#include "runtime.h"
#include "audio.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

uint32_t com_object(unsigned, unsigned);
void gheap_free(int, uint32_t);
static uint32_t A(CPU *c, unsigned i){ return MR32(c->esp + 4 * i); }
static float F(CPU *c, unsigned i){ uint32_t v = A(c, i); float f; memcpy(&f, &v, 4); return f; }

enum { FAM_LISTENER = 20, FAM_BUFFER3D = 21 };
/* SoundBuffer guest layout (sound.c): voice at +40, volume at +60, pan at +64 */
#define SB_VOICE 40
#define SB_VOLUME 60
#define SB_PAN 64

static struct { float pos[3], front[3], top[3], distance, rolloff; } L = {{0,0,0},{0,0,1},{0,1,0},1,1};
#define B3D_MAX 512
static struct { uint32_t obj, buffer; float pos[3], min, max; uint32_t mode; int used; } b3d[B3D_MAX];

static void apply(unsigned i){
    uint32_t b = b3d[i].buffer;
    int voice = (int)MR32(b + SB_VOICE), volume = (int32_t)MR32(b + SB_VOLUME), pan = (int32_t)MR32(b + SB_PAN);
    if (b3d[i].mode != 2){
        float d[3], rel[3];
        for (int k = 0; k < 3; k++) rel[k] = b3d[i].mode == 1 ? b3d[i].pos[k] : b3d[i].pos[k] - L.pos[k];
        float dist = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]) * L.distance;
        float minimum = b3d[i].min > 0 ? b3d[i].min : 1;
        if (dist > b3d[i].max && b3d[i].max > 0) dist = b3d[i].max;
        float gain = dist <= minimum ? 1.f : minimum / (minimum + (dist - minimum) * L.rolloff);
        volume += (int)(2000.f * log10f(gain > 1e-5f ? gain : 1e-5f));
        float right[3] = { L.top[1] * L.front[2] - L.top[2] * L.front[1], L.top[2] * L.front[0] - L.top[0] * L.front[2], L.top[0] * L.front[1] - L.top[1] * L.front[0] };
        float len = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        float side = len > 1e-4f ? (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / len : 0;
        (void)d;
        pan = (int)(side * 3000.f);
    }
    if (volume < -10000) volume = -10000;
    audio_set_volume(voice, volume);
    audio_set_pan(voice, pan);
}
static void apply_all(void){ for (unsigned i = 0; i < B3D_MAX; i++) if (b3d[i].used) apply(i); }

/* QueryInterface on a DirectSound buffer for a 3D interface; 0 if not 3D. */
uint32_t sound3d_query(uint32_t buffer, uint32_t first, int primary){
    if (first == 0x279afa84u && primary){
        uint32_t o = com_object(FAM_LISTENER, 0); MR32(o + 32) = buffer; return o;
    }
    if (first == 0x279afa86u && !primary){
        for (unsigned i = 0; i < B3D_MAX; i++) if (!b3d[i].used){
            memset(&b3d[i], 0, sizeof b3d[i]); b3d[i].used = 1; b3d[i].buffer = buffer; b3d[i].min = 1; b3d[i].max = 1e9f;
            b3d[i].obj = com_object(FAM_BUFFER3D, 0); MR32(b3d[i].obj + 32) = i; return b3d[i].obj;
        }
    }
    return 0;
}
/* The game's own SetVolume/SetPan go through the 3D effect too. */
void sound3d_buffer_changed(uint32_t buffer){
    for (unsigned i = 0; i < B3D_MAX; i++) if (b3d[i].used && b3d[i].buffer == buffer){ apply(i); return; }
}
void sound3d_buffer_released(uint32_t buffer){
    for (unsigned i = 0; i < B3D_MAX; i++) if (b3d[i].used && b3d[i].buffer == buffer) b3d[i].buffer = 0, b3d[i].used = 0;
}

int sound3d_method(CPU *c, uint32_t self, unsigned family, unsigned slot, uint32_t *r, uint32_t *nargs){
    *nargs = 1; *r = 0;
    if (slot == 0){ *nargs = 3; MR32(A(c, 2)) = 0; *r = 0x80004002u; return 1; }
    if (slot == 1){ *r = ++MR32(self + 8); return 1; }
    if (slot == 2){
        *r = --MR32(self + 8);
        if (!*r){ if (family == FAM_BUFFER3D){ unsigned i = MR32(self + 32); if (i < B3D_MAX) b3d[i].used = 0; } gheap_free(1, self); }
        return 1;
    }
    if (family == FAM_LISTENER){
        switch (slot){
        case 3: *nargs = 2; { uint32_t p = A(c, 1); MR32(p + 4) = 0; } return 1;
        case 4: case 5: case 8: *nargs = 2; { float v = slot == 4 ? L.distance : slot == 8 ? L.rolloff : 1; memcpy(guest_ptr(A(c, 1), 4), &v, 4); } return 1;
        case 6: *nargs = 3; memcpy(guest_ptr(A(c, 1), 12), L.front, 12); memcpy(guest_ptr(A(c, 2), 12), L.top, 12); return 1;
        case 7: case 9: *nargs = 2; memcpy(guest_ptr(A(c, 1), 12), slot == 7 ? L.pos : (float[3]){0,0,0}, 12); return 1;
        case 10: *nargs = 3; { uint32_t p = A(c, 1);           /* DS3DLISTENER: size, pos, vel, front, top, distance, rolloff, doppler */
                 memcpy(L.pos, guest_ptr(p + 4, 12), 12); memcpy(L.front, g_mem + p + 28, 12); memcpy(L.top, g_mem + p + 40, 12);
                 memcpy(&L.distance, g_mem + p + 52, 4); memcpy(&L.rolloff, g_mem + p + 56, 4); apply_all(); } return 1;
        case 11: *nargs = 3; L.distance = F(c, 1); apply_all(); return 1;
        case 12: *nargs = 3; return 1;
        case 13: *nargs = 8; L.front[0] = F(c, 1); L.front[1] = F(c, 2); L.front[2] = F(c, 3);
                 L.top[0] = F(c, 4); L.top[1] = F(c, 5); L.top[2] = F(c, 6); apply_all(); return 1;
        case 14: *nargs = 5; L.pos[0] = F(c, 1); L.pos[1] = F(c, 2); L.pos[2] = F(c, 3); apply_all(); return 1;
        case 15: *nargs = 3; L.rolloff = F(c, 1); apply_all(); return 1;
        case 16: *nargs = 5; return 1;
        case 17: apply_all(); return 1;
        }
        return 0;
    }
    unsigned i = MR32(self + 32);
    if (i >= B3D_MAX || !b3d[i].used) return 0;
    switch (slot){
    case 3: *nargs = 2; return 1;
    case 4: *nargs = 3; if (A(c, 1)) MR32(A(c, 1)) = 360; if (A(c, 2)) MR32(A(c, 2)) = 360; return 1;
    case 5: *nargs = 2; memcpy(guest_ptr(A(c, 1), 12), (float[3]){0,0,1}, 12); return 1;
    case 6: *nargs = 2; MR32(A(c, 1)) = 0; return 1;
    case 7: case 8: *nargs = 2; memcpy(guest_ptr(A(c, 1), 4), slot == 7 ? &b3d[i].max : &b3d[i].min, 4); return 1;
    case 9: *nargs = 2; MR32(A(c, 1)) = b3d[i].mode; return 1;
    case 10: case 11: *nargs = 2; memcpy(guest_ptr(A(c, 1), 12), slot == 10 ? b3d[i].pos : (float[3]){0,0,0}, 12); return 1;
    case 12: *nargs = 3; { uint32_t p = A(c, 1);              /* DS3DBUFFER: size, pos, vel, cone in/out, orient, outside vol, min, max, mode */
             memcpy(b3d[i].pos, guest_ptr(p + 4, 12), 12); memcpy(&b3d[i].min, g_mem + p + 48, 4); memcpy(&b3d[i].max, g_mem + p + 52, 4);
             b3d[i].mode = MR32(p + 56); apply(i); } return 1;
    case 13: *nargs = 4; return 1;
    case 14: *nargs = 5; return 1;
    case 15: *nargs = 3; return 1;
    case 16: *nargs = 3; b3d[i].max = F(c, 1); apply(i); return 1;
    case 17: *nargs = 3; b3d[i].min = F(c, 1); apply(i); return 1;
    case 18: *nargs = 3; b3d[i].mode = A(c, 1); apply(i); return 1;
    case 19: *nargs = 5; b3d[i].pos[0] = F(c, 1); b3d[i].pos[1] = F(c, 2); b3d[i].pos[2] = F(c, 3); apply(i); return 1;
    case 20: *nargs = 5; return 1;
    }
    return 0;
}
