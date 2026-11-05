#include "runtime.h"
#include "image.h"
#include "input.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void heap_init(void);
int heap_api_kind(const char *);
uint32_t heap_api(CPU *, int);
uint32_t gheap_alloc(int, uint32_t, int);
int fileio_call(CPU *, const char *, uint32_t *);
int fileio_is_handle(uint32_t);
int win32_ext_call(CPU *, const char *, uint32_t *);
int locale_call(CPU *, const char *, uint32_t *);
int network_call(CPU *, const char *, const char *, uint32_t *);
int environment_call(CPU *,const char *,uint32_t *);
int mapping_call(CPU *,const char *,uint32_t *);
int profile_call(CPU *,const char *,uint32_t *);
void guest_fair_yield(void);
void win32_thread_term_check(CPU *);

jmp_buf g_exit_jmp;
uint32_t g_mouse_hook, g_keyboard_hook;
int g_exit_code, g_exit_armed;
static uint8_t tls_used[128];
static uint32_t command_line,command_line_wide;
static uint32_t calls[1024];
static uint64_t start_us;

static uint32_t A(CPU *c, unsigned i){ return *(uint32_t *)guest_ptr(c->esp + 4 * i, 4); }
static const char *S(uint32_t p){
    for (uint32_t n = 0; n < 32768; n++){
        if (!mem_valid(p + n, 1)) break;
        if (!MR8(p + n)) return (char *)g_mem + p;
    }
    runtime_fail(g_cur_cpu, "invalid guest string", p);
    return NULL;
}
static unsigned upper_1252(unsigned ch){
    if((ch>='a'&&ch<='z')||(ch>=0xe0&&ch<=0xfe&&ch!=0xf7))return ch-32;
    switch(ch){case 0x9a:return 0x8a;case 0x9c:return 0x8c;case 0x9e:return 0x8e;case 0xff:return 0x9f;default:return ch;}
}

uint64_t host_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}
uint32_t host_ms(void){ return (uint32_t)((host_us() - start_us) / 1000); }
uint32_t win32_alloc(uint32_t size, int zero){ return gheap_alloc(1, size, zero); }
void win32_set_last_error(CPU *c, uint32_t value){ if (c && c->fs_base) MR32(c->fs_base + 0x34) = value; }
uint32_t win32_get_last_error(CPU *c){ return c->fs_base ? MR32(c->fs_base + 0x34) : 0; }
int win32_caller_module(void){ return 0; }

void platform_init(void){
    heap_init();
    start_us = host_us();
    char line[512];const char *extra=getenv("GEARHEAD_ARGS");
    snprintf(line,sizeof line,"\"C:\\Gearhead Garage\\ghg.exe\" -MustBeHere%s%s",extra&&*extra?" ":"",extra?extra:"");
    command_line = win32_alloc(sizeof line, 1);
    strcpy((char *)g_mem + command_line, line);
    command_line_wide=win32_alloc(2*sizeof line,1);
    for(unsigned i=0;i<strlen(line);i++)MR16(command_line_wide+2*i)=(unsigned char)line[i];
    fprintf(stderr,"guest command line: %s\n",line);
    if (image_import_count > 512){ fputs("too many imports\n", stderr); exit(1); }
    for (uint32_t i = 0; i < image_import_count; i++) MR32(image_imports[i].iat) = 0xf0000000u + i;
}

/* System DLLs the game loads by name; their exports are native (virtual imports). */
typedef struct { const char *dll, *name; int nargs; } VirtualImport;
static const VirtualImport virtual_imports[] = {
    {"DDRAW.dll","DirectDrawCreate",3},{"DDRAW.dll","DirectDrawEnumerateA",2},{"DDRAW.dll","DirectDrawCreateClipper",3},
    {"DINPUT.dll","DirectInputCreateA",4},{"DSOUND.dll","DirectSoundCreate",3},{"DSOUND.dll","DirectSoundEnumerateA",2},
    {"KERNEL32.dll","IsProcessorFeaturePresent",1},
};
#define VIRTUAL_COUNT (sizeof virtual_imports / sizeof *virtual_imports)
static const char *virtual_dlls[] = {"DDRAW.dll","DINPUT.dll","DSOUND.dll","KERNEL32.dll","USER32.dll"};
#define VIRTUAL_DLL_BASE 0xd8000000u

static uint32_t module_handle(const char *name){
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    for (uint32_t i = 0; i < image_module_count; i++)
        if (!strcasecmp(base, image_modules[i].name)) return image_modules[i].base;
    char stem[64]; snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.'); if (dot && !strcasecmp(dot, ".dll")) *dot = 0;
    for (uint32_t i = 0; i < sizeof virtual_dlls / sizeof *virtual_dlls; i++){
        char v[64]; snprintf(v, sizeof v, "%s", virtual_dlls[i]); *strrchr(v, '.') = 0;
        if (!strcasecmp(stem, v)) return VIRTUAL_DLL_BASE + i;
    }
    for (uint32_t i = 0; i < image_import_count; i++){
        char v[64]; snprintf(v, sizeof v, "%s", image_imports[i].dll); char *d = strrchr(v, '.'); if (d) *d = 0;
        if (!strcasecmp(stem, v)) return 0xd0000000u + i;
    }
    return 0;
}
static const char *module_path(uint32_t handle){
    if (!handle || handle == IMAGE_BASE) return "C:\\Gearhead Garage\\ghg.exe";
    static char path[260];
    for (uint32_t i = 0; i < image_module_count; i++)
        if (handle == image_modules[i].base){ snprintf(path, sizeof path, "C:\\Gearhead Garage\\Data\\%s", image_modules[i].name); return path; }
    return NULL;
}

static int core_call(CPU *c, const Import *imp, uint32_t *out){
    const char *n = imp->name;
    uint32_t r = 0;
    if (!strcasecmp(imp->dll,"MSVCRT.dll")){
        if(!strcmp(n,"_CItan"))ST(0)=tan(ST(0));
        else if(!strcmp(n,"_CIatan2")){double value=atan2(ST(1),ST(0));FPOP();ST(0)=value;}
        else if(!strcmp(n,"strrchr")||!strcmp(n,"strchr")){
            const char *start=S(A(c,0));const char *found=!strcmp(n,"strchr")?strchr(start,(int)A(c,1)):strrchr(start,(int)A(c,1));
            if(found)r=A(c,0)+(uint32_t)(found-start);
        }
        else if(!strcmp(n,"_stricmp"))r=(uint32_t)strcasecmp(S(A(c,0)),S(A(c,1)));
        else return 0;
    }
    /* Windows 98 SE: d3d.vd initialises only on the Windows 9x platform. */
    else if (!strcmp(n, "GetVersion")) r = 0xc8ae0a04u;
    else if (!strcmp(n, "GetVersionExA")){
        uint32_t p = A(c, 0); uint8_t *v = guest_ptr(p, 148);
        memset(v + 4, 0, 144); MR32(p+4)=4; MR32(p+8)=10; MR32(p+12)=0x040a08ae; MR32(p+16)=1; strcpy((char *)v+20," A "); r=1;
    }
    else if (!strcmp(n, "GetCommandLineA")) r=command_line;
    else if (!strcmp(n, "GetCommandLineW")) r=command_line_wide;
    else if (!strcmp(n, "GetStartupInfoA")){
        uint32_t p=A(c,0); memset(guest_ptr(p,68),0,68); MR32(p)=68;
    }
    else if (!strcmp(n, "GetStdHandle")) r=0x10u+(A(c,0)&3);
    else if (!strcmp(n, "GetFileType")){
        uint32_t h=A(c,0); r=h>=0x10&&h<=0x13 ? 2 : fileio_is_handle(h) ? 1 : 0;
        if (!r) win32_set_last_error(c,6);
    }
    else if (!strcmp(n, "SetHandleCount")) r=A(c,0);
    else if (!strcmp(n, "GetACP")) r=1252;
    else if (!strcmp(n, "GetOEMCP")) r=437;
    else if (!strcmp(n, "GetCPInfo")){
        uint32_t p=A(c,1); memset(guest_ptr(p,20),0,20); MR32(p)=1; MR16(p+4)='?'; r=1;
    }
    else if (!strcmp(n, "IsValidCodePage")) r=A(c,0)==1252 || A(c,0)==437 || A(c,0)==65001;
    else if (!strcmp(n, "TlsAlloc")){
        r=0xffffffff;
        for (unsigned i=0;i<128;i++) if (!tls_used[i]){ tls_used[i]=1; r=i; break; }
    }
    else if (!strcmp(n, "TlsFree")){
        uint32_t i=A(c,0);
        if (i<128 && tls_used[i]){
            tls_used[i]=0; r=1;
            for (uint32_t teb=0xc00000;teb<0xd00000;teb+=0x2000)
                if (mem_valid(teb+0x2c,4) && MR32(teb+0x18)==teb){
                    uint32_t array=MR32(teb+0x2c);
                    if (mem_valid(array+4*i,4)) MR32(array+4*i)=0;
                }
        }
    }
    else if (!strcmp(n, "TlsGetValue") || !strcmp(n, "TlsSetValue")){
        uint32_t i=A(c,0), array=MR32(c->fs_base+0x2c);
        if (i>=128 || !tls_used[i]){ win32_set_last_error(c,87); r=0; }
        else if (!strcmp(n,"TlsGetValue")){ r=MR32(array+4*i); win32_set_last_error(c,0); }
        else { MR32(array+4*i)=A(c,1); r=1; }
    }
    else if (!strcmp(n, "GetCurrentThreadId")) r=MR32(c->fs_base+0x24);
    else if (!strcmp(n, "GetCurrentProcessId")) r=100;
    else if (!strcmp(n, "GetCurrentProcess")) r=0xffffffff;
    else if (!strcmp(n, "GetLastError")) r=win32_get_last_error(c);
    else if (!strcmp(n, "SetLastError")) win32_set_last_error(c,A(c,0));
    else if(!strcmp(n,"CompareFileTime")){
        uint64_t a,b;memcpy(&a,guest_ptr(A(c,0),8),8);memcpy(&b,guest_ptr(A(c,1),8),8);
        r=a<b?0xffffffffu:a>b?1:0;
    }
    else if (!strcmp(n, "GetTickCount")) r=host_ms();
    else if (!strcmp(n, "QueryPerformanceCounter") || !strcmp(n, "QueryPerformanceFrequency")){
        uint64_t v=!strcmp(n,"QueryPerformanceCounter") ? host_us() : 1000000;
        memcpy(guest_ptr(A(c,0),8),&v,8); r=1;
    }
    else if (!strcmp(n, "GetModuleFileNameA")||!strcmp(n,"GetModuleFileNameW")){
        const char *s=module_path(A(c,0)); uint32_t p=A(c,1), size=A(c,2);
        if(!s){win32_set_last_error(c,126);*out=0;return 1;}
        uint32_t len=(uint32_t)strlen(s);
        int wide=n[strlen(n)-1]=='W';
        if (size){
            uint32_t k=len<size ? len : size-1;guest_ptr(p,(uint64_t)(k+1)*(wide?2:1));
            if(wide){for(uint32_t i=0;i<k;i++)MR16(p+2*i)=(unsigned char)s[i];MR16(p+2*k)=0;}
            else{memcpy(g_mem+p,s,k);MR8(p+k)=0;}
            r=k;
        }
    }
    else if (!strcmp(n, "GetModuleHandleA")) r=A(c,0) ? module_handle(S(A(c,0))) : IMAGE_BASE;
    else if (!strcmp(n, "LoadLibraryA")){
        const char *name=S(A(c,0)); r=module_handle(name);
        fprintf(stderr,"LoadLibraryA(%s) -> %08x\n",name,r);
        /* A linked guest DLL runs its entry point (CRT start-up, DllMain) once. */
        static uint8_t attached[16];
        for(uint32_t i=1;r&&i<image_module_count&&i<16;i++)if(image_modules[i].base==r&&!attached[i]){
            attached[i]=1;uint32_t args[]={r,1,0};uint32_t ok=call_guest(c,image_modules[i].entry,3,args);
            fprintf(stderr,"DllMain(%s, PROCESS_ATTACH) -> %u\n",image_modules[i].name,ok);
            if(!ok){attached[i]=0;r=0;win32_set_last_error(c,1114);}
        }
        if (!r) win32_set_last_error(c,126);
    }
    else if (!strcmp(n, "GetProcAddress")){
        uint32_t name=A(c,1), handle=A(c,0);
        const char *dll=NULL;
        if(name>0xffff)
            for(uint32_t i=0;i<image_export_count;i++)
                if(image_exports[i].module==handle&&!strcmp(image_exports[i].name,S(name))){r=image_exports[i].address;break;}
        if (handle>=VIRTUAL_DLL_BASE && handle-VIRTUAL_DLL_BASE<sizeof virtual_dlls/sizeof *virtual_dlls) dll=virtual_dlls[handle-VIRTUAL_DLL_BASE];
        if (handle>=0xd0000000u && handle-0xd0000000u<image_import_count) dll=image_imports[handle-0xd0000000u].dll;
        for (uint32_t i=0;!r&&dll&&name>0xffff&&i<VIRTUAL_COUNT;i++)
            if(!strcasecmp(virtual_imports[i].dll,dll)&&!strcmp(virtual_imports[i].name,S(name)))r=0xf0000000u+image_import_count+i;
        for (uint32_t i=0;!r && dll && i<image_import_count;i++){
            const Import *e=&image_imports[i];
            int match=name>0xffff ? !strcmp(e->name,S(name)) : e->name[0]=='#' && (unsigned)atoi(e->name+1)==name;
            if (!strcasecmp(e->dll,dll) && match){ r=0xf0000000u+i; break; }
        }
        if (!r) win32_set_last_error(c,127);
    }
    else if (!strcmp(n, "FreeLibrary")) r=1;
    /* Synthetic native modules have no guest DLL thread-entry callbacks. */
    else if (!strcmp(n,"DisableThreadLibraryCalls"))r=A(c,0)>=0xd0000000u;
    else if (!strcmp(n, "SetUnhandledExceptionFilter")) { static uint32_t filter; r=filter; filter=A(c,0); }
    else if (!strcmp(n, "UnhandledExceptionFilter")) runtime_fail(c,"unhandled guest exception",c->eip);
    else if (!strcmp(n, "RaiseException") || !strcmp(n, "RtlUnwind")) runtime_fail(c,n,A(c,0));
    else if (!strcmp(n, "InterlockedIncrement")) r=++*(uint32_t *)guest_ptr(A(c,0),4);
    else if (!strcmp(n, "InterlockedDecrement")) r=--*(uint32_t *)guest_ptr(A(c,0),4);
    else if (!strcmp(n, "InterlockedExchange")) { uint32_t *p=guest_ptr(A(c,0),4); r=*p; *p=A(c,1); }
    else if (!strcmp(n, "ExitProcess") || !strcmp(n, "TerminateProcess")){
        g_exit_code=(int)A(c,!strcmp(n,"ExitProcess")?0:1);
        fprintf(stderr,"guest %s(%d)\n",n,g_exit_code);
        if (g_exit_armed) longjmp(g_exit_jmp,1);
        exit(g_exit_code);
    }
    else if (!strcmp(n,"GetKeyboardType")){
        unsigned type=A(c,0);r=type==0?4:type==1?0:type==2?12:0;
    }
    else if (!strcmp(n,"GetKeyState")||!strcmp(n,"GetAsyncKeyState"))r=(uint32_t)input_key_state(A(c,0));
    else if (!strcmp(n,"ScreenToClient")||!strcmp(n,"ClientToScreen")){
        guest_ptr(A(c,1),8);
        if(A(c,0)==0x00b00001u)r=1;
        else win32_set_last_error(c,1400);
    }
    else if (!strcmp(n,"CharUpperA")){
        uint32_t p=A(c,0);
        if(p<=0xffff)r=upper_1252(p);
        else{size_t length=strlen(S(p));for(size_t i=0;i<length;i++)MR8(p+i)=(uint8_t)upper_1252(MR8(p+i));r=p;}
    }
    else if(!strcmp(n,"CharUpperBuffA")){
        uint32_t p=A(c,0),count=A(c,1);guest_ptr(p,count);
        for(uint32_t i=0;i<count;i++)MR8(p+i)=(uint8_t)upper_1252(MR8(p+i));r=count;
    }
    /* Cocoa owns global shortcuts; Windows keyboard hooks cannot intercept other Mac apps. */
    else if(!strcmp(n,"SetWindowsHookExA")){
        if(getenv("GEARHEAD_INPUT_TRACE"))fprintf(stderr,"requested Windows hook type=%u procedure=%08x\n",A(c,0),A(c,1));
        if(A(c,0)==7){g_mouse_hook=A(c,1);r=0x00b50007u;}
        else if(A(c,0)==2){g_keyboard_hook=A(c,1);r=0x00b50002u;}
        else{win32_set_last_error(c,120);r=0;}
    }
    else if(!strcmp(n,"UnhookWindowsHookEx")){if(A(c,0)==0x00b50007u){g_mouse_hook=0;r=1;}else if(A(c,0)==0x00b50002u){g_keyboard_hook=0;r=1;}else{win32_set_last_error(c,6);r=0;}}
    else if(!strcmp(n,"CallNextHookEx"))r=0;
    else if(!strcmp(n,"SwapMouseButton"))r=(uint32_t)input_swap_buttons(!!A(c,0));
    else if (!strcmp(n,"GetDriveTypeA")){
        const char *p=A(c,0)?S(A(c,0)):"C:";
        r=(p[0]=='D'||p[0]=='d')?5:(p[0]=='C'||p[0]=='c')?3:1;
    }
    else if (!strcmp(n,"GetWindowDC"))r=0x00b20000u;
    else if (!strcmp(n,"ReleaseDC"))r=A(c,1)==0x00b20000u;
    else if (!strcmp(n,"GetDeviceCaps")){
        unsigned width,height;void metal_game_size(unsigned *,unsigned *);metal_game_size(&width,&height);
        switch(A(c,1)){
        case 2:r=1;break;case 8:r=width;break;case 10:r=height;break;
        case 12:case 108:r=32;break;case 14:r=1;break;case 24:r=0xffffffff;break;
        case 88:case 90:r=96;break;case 116:r=60;break;default:r=0;break;
        }
    }
    else if (!strcmp(n,"GetStockObject"))r=0x00b30000u+A(c,0);
    else if (!strcmp(n,"AdjustWindowRectEx")){guest_ptr(A(c,0),16);r=1;}
    else if (!strcmp(n,"SystemParametersInfoA")){
        /* Virtual Windows screen-saver/power flags never change Mac settings. */
        static uint32_t saver,power;
        switch(A(c,0)){
        case 0x10:*(uint32_t *)guest_ptr(A(c,2),4)=saver;r=1;break;
        case 0x11:saver=!!A(c,1);r=1;break;
        case 0x54:*(uint32_t *)guest_ptr(A(c,2),4)=power;r=1;break;
        case 0x56:power=!!A(c,1);r=1;break;
        default:win32_set_last_error(c,87);break;
        }
    }
    else if (!strcmp(n,"CreateWindowExA")){
        win32_ext_call(c,n,&r);
        unsigned w=A(c,6),h=A(c,7);
        /* The video drivers create a 40x40 window off-screen; the picture is 640x480. */
        if(w<320||w>4096)w=640;if(h<240||h>4096)h=480;
        void host_open_window(unsigned,unsigned);host_open_window(w,h);
    }
    else return 0;
    *out=r; return 1;
}

void platform_call(CPU *c, uint32_t index){
    static Import virtual_entries[VIRTUAL_COUNT];
    const Import *imp;
    if (index<image_import_count) imp=&image_imports[index];
    else if (index-image_import_count<VIRTUAL_COUNT){
        Import *v=&virtual_entries[index-image_import_count];
        const VirtualImport *s=&virtual_imports[index-image_import_count];
        v->dll=s->dll;v->name=s->name;v->nargs=s->nargs;imp=v;
    }
    else runtime_fail(c,"unknown import thunk",index);
    win32_thread_term_check(c);
    guest_fair_yield();
    POP32(c->eip);
    if (!calls[index]++ || getenv("GEARHEAD_TRACE")){
        fprintf(stderr,"[%s!%s] from %08x",imp->dll,imp->name,c->eip);
        for (int i=0;i<imp->nargs && i<8;i++) fprintf(stderr," %08x",A(c,(unsigned)i));
        fputc('\n',stderr);
    }
    uint32_t r=0;
    int kind=heap_api_kind(imp->name);
    int gearhead_call(CPU *,const char *,const char *,uint32_t *);
    if (kind>0) r=heap_api(c,kind);
    else if (gearhead_call(c,imp->dll,imp->name,&r)) {}
    else if (environment_call(c,imp->name,&r)) {}
    else if (core_call(c,imp,&r)) {}
    else if (com_platform_call(c,imp->dll,imp->name,&r)) {}
    else if (network_call(c,imp->dll,imp->name,&r)) {}
    else if (locale_call(c,imp->name,&r)) {}
    else if (mapping_call(c,imp->name,&r)) {}
    else if (profile_call(c,imp->name,&r)) {}
    else if (fileio_call(c,imp->name,&r)) {}
    else if (win32_ext_call(c,imp->name,&r)) {}
    else runtime_fail(c,imp->name,c->eip);
    c->eax=r;
    c->esp+=(uint32_t)imp->nargs*4;
}
