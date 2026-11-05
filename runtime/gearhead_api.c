/* Win32 calls Gearhead Garage needs beyond the shared runtime. */
#include "runtime.h"
#include "input.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int fileio_resolve_save(const char *, char *, size_t);
void gheap_free(int, uint32_t);
static uint32_t A(CPU *c, unsigned i){ return MR32(c->esp + 4 * i); }
static const char *S(uint32_t p){
    for (uint32_t n = 0; n < 65536; n++){
        if (!mem_valid(p + n, 1)) break;
        if (!MR8(p + n)) return (char *)g_mem + p;
    }
    runtime_fail(g_cur_cpu, "invalid guest string", p);
    return NULL;
}

/* ---- WinMM multimedia file I/O (RIFF/WAVE parsing) ----------------------
 * MMIOINFO is kept in guest memory, so mmioGetInfo/mmioSetInfo are copies and
 * the game may read straight from the buffer between mmioAdvance calls. */
enum { MI_FLAGS = 0, MI_IOPROC = 4, MI_ERROR = 12, MI_CCH = 20, MI_BUFFER = 24, MI_NEXT = 28,
       MI_ENDREAD = 32, MI_ENDWRITE = 36, MI_BUFOFF = 40, MI_DISKOFF = 44, MI_HMMIO = 68, MI_SIZE = 72 };
#define MMIO_BASE 0x00c80000u
#define MMIO_MAX 64
#define FOURCC_MEM 0x204d454du
typedef struct { int used, fd; uint32_t mem, memsize, info, own; int64_t size; } Mmio;
static Mmio mmio[MMIO_MAX];

static Mmio *mmio_get(uint32_t h){
    uint32_t i = h - MMIO_BASE;
    return i < MMIO_MAX && mmio[i].used ? &mmio[i] : NULL;
}
static int64_t mmio_pos(Mmio *m){
    uint32_t i = m->info;
    return (int64_t)(int32_t)MR32(i + MI_BUFOFF) + (MR32(i + MI_NEXT) - MR32(i + MI_BUFFER));
}
/* Refill the buffer from logical offset p. */
static void mmio_fill(Mmio *m, int64_t p){
    uint32_t i = m->info, buf = MR32(i + MI_BUFFER);
    if (m->mem){
        MR32(i + MI_BUFOFF) = 0; MR32(i + MI_NEXT) = buf + (uint32_t)(p < 0 ? 0 : p > m->memsize ? m->memsize : p);
        MR32(i + MI_ENDREAD) = buf + m->memsize; MR32(i + MI_DISKOFF) = m->memsize; return;
    }
    uint32_t cap = MR32(i + MI_CCH);
    ssize_t n = p >= 0 && p < m->size ? pread(m->fd, g_mem + buf, cap, p) : 0;
    if (n < 0) n = 0;
    MR32(i + MI_BUFOFF) = (uint32_t)p; MR32(i + MI_NEXT) = buf; MR32(i + MI_ENDREAD) = buf + (uint32_t)n;
    MR32(i + MI_ENDWRITE) = buf + cap; MR32(i + MI_DISKOFF) = (uint32_t)(p + n);
}
static void mmio_seek_to(Mmio *m, int64_t p){
    uint32_t i = m->info, buf = MR32(i + MI_BUFFER);
    int64_t lo = (int32_t)MR32(i + MI_BUFOFF), hi = lo + (MR32(i + MI_ENDREAD) - buf);
    if (m->mem || (p >= lo && p <= hi)){ if (m->mem) mmio_fill(m, p); else MR32(i + MI_NEXT) = buf + (uint32_t)(p - lo); return; }
    MR32(i + MI_BUFOFF) = (uint32_t)p; MR32(i + MI_NEXT) = MR32(i + MI_ENDREAD) = buf;
}
static uint32_t mmio_read(Mmio *m, uint32_t dst, uint32_t n){
    uint32_t i = m->info, done = 0;
    while (done < n){
        uint32_t next = MR32(i + MI_NEXT), end = MR32(i + MI_ENDREAD);
        if (next >= end){
            if (m->mem) break;
            int64_t p = mmio_pos(m); mmio_fill(m, p);
            if (MR32(i + MI_NEXT) >= MR32(i + MI_ENDREAD)) break;
            continue;
        }
        uint32_t k = end - next < n - done ? end - next : n - done;
        memmove(guest_ptr(dst + done, k), g_mem + next, k);
        MR32(i + MI_NEXT) = next + k; done += k;
    }
    return done;
}

static uint32_t mmio_bytes(Mmio *m, uint8_t *dst, uint32_t n){
    uint32_t i = m->info, got = 0;
    while (got < n){
        uint32_t next = MR32(i + MI_NEXT);
        if (next >= MR32(i + MI_ENDREAD)){
            if (m->mem) break;
            mmio_fill(m, mmio_pos(m));
            if (MR32(i + MI_NEXT) >= MR32(i + MI_ENDREAD)) break;
            continue;
        }
        dst[got++] = MR8(next); MR32(i + MI_NEXT) = next + 1;
    }
    return got;
}

static uint32_t mmio_call(CPU *c, const char *n, int *handled){
    *handled = 1;
    if (!strcmp(n, "mmioOpenA")){
        uint32_t name = A(c, 0), user = A(c, 1), flags = A(c, 2);
        unsigned k; for (k = 0; k < MMIO_MAX && mmio[k].used; k++);
        if (k == MMIO_MAX) return 0;
        Mmio *m = &mmio[k]; memset(m, 0, sizeof *m); m->fd = -1;
        if ((flags & 3) != 0){ if (user) MR16(user + MI_ERROR) = 260; return 0; }   /* read only */
        if (user && MR32(user + MI_IOPROC) == FOURCC_MEM){
            m->mem = MR32(user + MI_BUFFER); m->memsize = MR32(user + MI_CCH);
            if (!m->mem || !mem_valid(m->mem, m->memsize)){ MR16(user + MI_ERROR) = 257; return 0; }
        } else {
            char path[1024];
            if (!name || !fileio_resolve_save(S(name), path, sizeof path) || (m->fd = open(path, O_RDONLY)) < 0){
                if (user) MR16(user + MI_ERROR) = 257;
                if (getenv("GEARHEAD_FILE_TRACE")) fprintf(stderr, "mmioOpenA(%s) failed\n", name ? S(name) : "");
                return 0;
            }
            struct stat st; fstat(m->fd, &st); m->size = st.st_size;
        }
        m->used = 1;
        m->info = win32_alloc(MI_SIZE, 1);
        uint32_t i = m->info;
        if (m->mem){ MR32(i + MI_IOPROC) = FOURCC_MEM; MR32(i + MI_BUFFER) = m->mem; MR32(i + MI_CCH) = m->memsize; }
        else {
            uint32_t cap = user && MR32(user + MI_CCH) ? MR32(user + MI_CCH) : 8192;
            m->own = win32_alloc(cap, 0); MR32(i + MI_BUFFER) = m->own; MR32(i + MI_CCH) = cap;
        }
        MR32(i + MI_FLAGS) = flags; MR32(i + MI_HMMIO) = MMIO_BASE + k;
        mmio_fill(m, 0);
        return MMIO_BASE + k;
    }
    if (strncmp(n, "mmio", 4)){ *handled = 0; return 0; }
    Mmio *m = mmio_get(A(c, 0));
    if (!m) return !strcmp(n, "mmioSeek") || !strcmp(n, "mmioRead") ? 0xffffffffu : 258;
    uint32_t i = m->info;
    if (!strcmp(n, "mmioClose")){
        if (m->fd >= 0) close(m->fd);
        if (m->own) gheap_free(1, m->own);
        gheap_free(1, m->info); memset(m, 0, sizeof *m); return 0;
    }
    if (!strcmp(n, "mmioRead")) return mmio_read(m, A(c, 1), A(c, 2));
    if (!strcmp(n, "mmioSeek")){
        int64_t base = A(c, 2) == 0 ? 0 : A(c, 2) == 1 ? mmio_pos(m) : (m->mem ? m->memsize : m->size);
        int64_t p = base + (int32_t)A(c, 1);
        if (p < 0) return 0xffffffffu;
        mmio_seek_to(m, p); return (uint32_t)p;
    }
    if (!strcmp(n, "mmioGetInfo")){ memcpy(guest_ptr(A(c, 1), MI_SIZE), g_mem + i, MI_SIZE); return 0; }
    if (!strcmp(n, "mmioSetInfo")){
        uint32_t u = A(c, 1);
        MR32(i + MI_NEXT) = MR32(u + MI_NEXT); MR32(i + MI_ENDREAD) = MR32(u + MI_ENDREAD); return 0;
    }
    if (!strcmp(n, "mmioAdvance")){
        uint32_t u = A(c, 1);
        if (u){ MR32(i + MI_NEXT) = MR32(u + MI_NEXT); }
        if (!m->mem) mmio_fill(m, (int32_t)MR32(i + MI_BUFOFF) + (MR32(i + MI_ENDREAD) - MR32(i + MI_BUFFER)));
        if (u) memcpy(g_mem + u, g_mem + i, MI_SIZE);
        return 0;
    }
    if (!strcmp(n, "mmioDescend")){
        uint32_t ck = A(c, 1), parent = A(c, 2), flags = A(c, 3);
        int64_t limit = parent ? (int64_t)MR32(parent + 12) + MR32(parent + 4) : (int64_t)1 << 40;
        uint32_t want = MR32(ck), want_type = MR32(ck + 8);
        for (;;){
            int64_t at = mmio_pos(m);
            if (at + 8 > limit) return 265;
            uint32_t h[3] = {0, 0, 0};
            if (mmio_bytes(m, (uint8_t *)h, 8) < 8) return 265;
            int list = h[0] == 0x46464952u || h[0] == 0x5453494cu;   /* RIFF, LIST */
            if (list && mmio_bytes(m, (uint8_t *)&h[2], 4) < 4) return 265;
            int match = !(flags & 0x70) ||
                ((flags & 0x10) && h[0] == want) ||
                ((flags & 0x20) && h[0] == 0x46464952u && h[2] == want_type) ||
                ((flags & 0x40) && h[0] == 0x5453494cu && h[2] == want_type);
            if (match){
                MR32(ck) = h[0]; MR32(ck + 4) = h[1]; MR32(ck + 8) = h[2];
                MR32(ck + 12) = (uint32_t)(at + 8); MR32(ck + 16) = 0;
                return 0;
            }
            mmio_seek_to(m, at + 8 + h[1] + (h[1] & 1));
        }
    }
    if (!strcmp(n, "mmioAscend")){
        uint32_t ck = A(c, 1), size = MR32(ck + 4);
        mmio_seek_to(m, (int64_t)MR32(ck + 12) + size + (size & 1));
        return 0;
    }
    *handled = 0; return 0;
}

/* ---- keyboard text ---- */
static unsigned vk_char(unsigned vk, int shift){
    if (vk >= 'A' && vk <= 'Z') return shift ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return shift ? (unsigned)")!@#$%^&*("[vk - '0'] : vk;
    switch (vk){
    case 0x20: return ' '; case 0x0d: return '\r'; case 0x08: return 8; case 0x1b: return 27; case 0x09: return 9;
    case 0xba: return shift ? ':' : ';'; case 0xbb: return shift ? '+' : '='; case 0xbc: return shift ? '<' : ',';
    case 0xbd: return shift ? '_' : '-'; case 0xbe: return shift ? '>' : '.'; case 0xbf: return shift ? '?' : '/';
    case 0xc0: return shift ? '~' : '`'; case 0xdb: return shift ? '{' : '['; case 0xdc: return shift ? '|' : '\\';
    case 0xdd: return shift ? '}' : ']'; case 0xde: return shift ? '"' : '\'';
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: return '0' + vk - 0x60;
    default: return 0;
    }
}

int gearhead_call(CPU *c, const char *dll, const char *n, uint32_t *out){
    (void)dll;
    uint32_t r = 0;
    int handled;
    r = mmio_call(c, n, &handled);
    if (handled){ *out = r; return 1; }
    r = 0;
    if (!strcmp(n, "IsProcessorFeaturePresent")) r = A(c, 0) == 3 || A(c, 0) == 6 ? 1 : 0; /* MMX, SSE */
    else if (!strcmp(n, "SetConsoleTitleA") || !strcmp(n, "SetConsoleCtrlHandler") || !strcmp(n, "SetStdHandle")) r = 1;
    else if (!strcmp(n, "GetConsoleScreenBufferInfo")){ win32_set_last_error(c, 6); r = 0; }
    else if (!strcmp(n, "ClipCursor") || !strcmp(n, "MoveWindow") || !strcmp(n, "FillRect")) r = 1;
    else if (!strcmp(n, "FindWindowA")) r = 0;
    else if (!strcmp(n, "CreateSolidBrush")) r = 0x00b40000u | (A(c, 0) & 0xffff);
    else if (!strcmp(n, "SelectObject")) r = 0x00b30000u;
    else if (!strcmp(n, "SetRect")){
        uint32_t p = A(c, 0); guest_ptr(p, 16);
        MR32(p) = A(c, 1); MR32(p + 4) = A(c, 2); MR32(p + 8) = A(c, 3); MR32(p + 12) = A(c, 4); r = 1;
    }
    else if (!strcmp(n, "lstrlenA")) r = A(c, 0) ? (uint32_t)strlen(S(A(c, 0))) : 0;
    else if (!strcmp(n, "lstrcpyA")){
        const char *s = S(A(c, 1)); size_t l = strlen(s) + 1;
        memmove(guest_ptr(A(c, 0), l), s, l); r = A(c, 0);
    }
    else if (!strcmp(n, "lstrcatA")){
        const char *d = S(A(c, 0)), *s = S(A(c, 1)); size_t a = strlen(d), l = strlen(s) + 1;
        memmove(guest_ptr(A(c, 0) + (uint32_t)a, l), s, l); r = A(c, 0);
    }
    else if (!strcmp(n, "FormatMessageA")){
        /* (flags, source, id, lang, buffer, size, args) */
        uint32_t flags = A(c, 0), id = A(c, 2), buf = A(c, 4), size = A(c, 5);
        char text[64]; snprintf(text, sizeof text, "Windows error %u.\r\n", id);
        uint32_t l = (uint32_t)strlen(text);
        if (flags & 0x100){
            uint32_t p = win32_alloc(l + 1, 1); memcpy(g_mem + p, text, l + 1);
            MR32(buf) = p; r = l;
        } else if (size > l){ memcpy(guest_ptr(buf, l + 1), text, l + 1); r = l; }
    }
    else if (!strcmp(n, "ToAscii")){
        uint32_t vk = A(c, 0), state = A(c, 2), dst = A(c, 3);
        int shift = state && (MR8(state + 0x10) & 0x80);
        int caps = state && (MR8(state + 0x14) & 1);
        unsigned ch = vk_char(vk, shift ^ (caps && vk >= 'A' && vk <= 'Z'));
        if (ch){ MR16(dst) = (uint16_t)ch; r = 1; }
    }
    else return 0;
    *out = r;
    return 1;
}
