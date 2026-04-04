/* retro_main.c  –  EmulationStation PS5 libretro frontend
 * Multi-system launcher: NES, SNES, Mega Drive, Game Boy, GBA.
 * Dynamically loads libretro cores (.bin) uploaded via FTP.
 * Falls back to built-in NES emulator when no NES core is present.
 */
#include "nes.h"
#include "tables.h"
#include "ftp.h"
#include "libretro.h"

/* ── UI colours (NES palette indices) ──────────────────────────────────── */
#define COL_BG    0x0F
#define COL_BRAND 0x16
#define COL_HEAD  0x21
#define COL_LINE  0x02
#define COL_SEL   0x2A
#define COL_NORM  0x30
#define COL_DIM   0x00
#define COL_CUR   0x16
#define COL_NUM   0x21
#define COL_OK    0x2A

static const char *RESP_204K = "HTTP/1.1 204\r\nConnection:keep-alive\r\nAccess-Control-Allow-Origin:*\r\n\r\n";
static const char *RESP_CORS = "HTTP/1.1 204\r\nAccess-Control-Allow-Origin:*\r\nAccess-Control-Allow-Methods:POST\r\nConnection:keep-alive\r\n\r\n";

/* ── System table ───────────────────────────────────────────────────────── */
#define NUM_SYSTEMS 7

static const char *SYS_NAME[NUM_SYSTEMS] = {
    "Nintendo Ent. System",
    "Super Nintendo",
    "Sega Mega Drive",
    "Game Boy / Color",
    "Game Boy Advance",
    "PlayStation 1",
    "PlayStation 2",
};
static const char *SYS_SHORT[NUM_SYSTEMS] = {
    "NES", "SNES", "MEGA DRIVE", "GAME BOY", "GBA", "PS1", "PS2"
};
static const char *SYS_EXT1[NUM_SYSTEMS] = { ".nes", ".sfc", ".md",  ".gb",  ".gba", ".bin", ".iso" };
static const char *SYS_EXT2[NUM_SYSTEMS] = { ".rom", ".smc", ".bin", ".gbc", "",     ".iso", ".img" };
static const char *SYS_CORE[NUM_SYSTEMS] = {
    "nes_core.bin", "snes_core.bin", "md_core.bin", "gb_core.bin", "gba_core.bin",
    "ps1_core.bin", "ps2_core.bin"
};
static const u8 SYS_COL[NUM_SYSTEMS] = { 0x15, 0x23, 0x14, 0x2A, 0x27, 0x22, 0x14 };

/* ── Static globals for libretro callbacks ──────────────────────────────── */
static void *g_gadget;
static void *g_aud_out_fn;
static s32   g_aud_h      = -1;
static s32   g_video_h    = -1;
static void *g_vid_flip_fn;
static void *g_wait_eq_fn;
static u64   g_eq;
static u32  *g_fbs[2];
static int   g_active;
static u64   g_total_frames;
static u32   g_core_w = 256, g_core_h = 240;
static u16   g_retro_pad;
static void *g_pad_read_fn;
static s32   g_pad_h = -1;
static u8    g_pad_buf[128];
static int   g_want_menu;
static int   g_want_exit;

/* audio ring buffer for sceAudioOutOutput (256-sample blocks) */
static s16   g_abuf[256 * 2];
static int   g_abuf_pos;

/* ── Helpers (no libc) ──────────────────────────────────────────────────── */
static int es_tolower(int c) { return (c>='A'&&c<='Z') ? c+32 : c; }

static int es_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

static int es_strncmp_ci(const char *a, const char *b, int n) {
    for (int i=0; i<n; i++) {
        int x = es_tolower((u8)a[i]), y = es_tolower((u8)b[i]);
        if (x!=y) return x-y;
        if (!x) return 0;
    }
    return 0;
}

static int ext_match(const char *name, const char *ext1, const char *ext2) {
    int nlen = es_strlen(name);
    int e1   = es_strlen(ext1);
    int e2   = es_strlen(ext2);
    if (nlen < 2) return 0;
    if (e1 > 0 && nlen >= e1 && es_strncmp_ci(name + nlen - e1, ext1, e1) == 0) return 1;
    if (e2 > 0 && nlen >= e2 && es_strncmp_ci(name + nlen - e2, ext2, e2) == 0) return 1;
    return 0;
}

static void es_memset(void *p, u8 v, u64 n) {
    u8 *b = (u8*)p; for (u64 i=0; i<n; i++) b[i]=v;
}

static void es_memcpy(void *dst, const void *src, u64 n) {
    u8 *d=(u8*)dst; const u8 *s=(const u8*)src;
    for (u64 i=0; i<n; i++) d[i]=s[i];
}

/* ── UDP log ────────────────────────────────────────────────────────────── */
static void udp_log(void *G, void *sendto, s32 fd, u8 *sa, const char *msg) {
    if (fd < 0 || !sendto) return;
    NC(G, sendto, (u64)fd, (u64)msg, (u64)es_strlen(msg), 0, (u64)sa, 16);
}

static void clear_fb(u32 *fb) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
}

/* ── Web controller helpers (verbatim from main.c) ──────────────────────── */
static int poll_ready(void *G, void *poll, s32 fd, s32 ms) {
    u8 pfd[8];
    *(s32*)pfd = fd; *(u16*)(pfd+4)=0x0001; *(u16*)(pfd+6)=0;
    return (s32)NC(G, poll, (u64)pfd, 1, (u64)ms, 0, 0, 0) > 0;
}
static int parse_pad_last(u8 *buf, s32 len) {
    int val=-1;
    for (s32 i=len-2; i>=1; i--) {
        if (buf[i]=='/'&&buf[i+1]=='b') {
            val=0;
            for (s32 j=i+2; j<len&&j<i+8; j++) {
                if (buf[j]>='0'&&buf[j]<='9') val=val*10+(buf[j]-'0'); else break;
            }
            break;
        }
    }
    return val;
}
static int count_posts(u8 *buf, s32 len) {
    int c=0;
    for (s32 i=0; i<len-4; i++)
        if (buf[i]=='P'&&buf[i+1]=='O'&&buf[i+2]=='S'&&buf[i+3]=='T') c++;
    return c;
}
static u8 web_handle(void *G, void *poll, void *accept, void *recv,
                     void *send, void *close, void *sso, s32 listen_fd,
                     s32 *keep_fd, u8 *page, u64 page_len, u8 *pad_out) {
    u8 got_input=0, req[512];
    if (*keep_fd>=0) {
        for (int r=0; r<8; r++) {
            if (!poll_ready(G,poll,*keep_fd,0)) break;
            s32 n=(s32)NC(G,recv,(u64)*keep_fd,(u64)req,512,0x80,0,0);
            if (n<=0){NC(G,close,(u64)*keep_fd,0,0,0,0,0);*keep_fd=-1;break;}
            int v=parse_pad_last(req,n);
            if (v>=0){*pad_out=(u8)v;got_input=1;
                int np=count_posts(req,n);
                for (int k=0;k<np;k++)
                    NC(G,send,(u64)*keep_fd,(u64)RESP_204K,(u64)es_strlen(RESP_204K),0,0,0);
            }
        }
    }
    for (int i=0; i<4; i++) {
        if (!poll_ready(G,poll,listen_fd,0)) break;
        u8 sa[16]; s32 sa_len=16;
        s32 client=(s32)NC(G,accept,(u64)listen_fd,(u64)sa,(u64)&sa_len,0,0,0);
        if (client<0) break;
        if (sso){s32 one=1;NC(G,sso,(u64)client,6,1,(u64)&one,4,0);}
        if (!poll_ready(G,poll,client,0)){NC(G,close,(u64)client,0,0,0,0,0);continue;}
        s32 n=(s32)NC(G,recv,(u64)client,(u64)req,512,0x80,0,0);
        if (n>7&&req[0]=='P'&&req[5]=='/'&&req[6]=='b') {
            int v=parse_pad_last(req,n);
            if (v>=0){*pad_out=(u8)v;got_input=1;}
            NC(G,send,(u64)client,(u64)RESP_204K,(u64)es_strlen(RESP_204K),0,0,0);
            if (*keep_fd>=0) NC(G,close,(u64)*keep_fd,0,0,0,0,0);
            *keep_fd=client;
        } else if (n>5&&req[0]=='G'&&req[4]=='/') {
            u64 off=0;
            while (off<page_len){u64 chunk=page_len-off;if(chunk>2048)chunk=2048;NC(G,send,(u64)client,(u64)(page+off),chunk,0,0,0);off+=chunk;}
            NC(G,close,(u64)client,0,0,0,0,0);
        } else if (n>0&&req[0]=='O') {
            NC(G,send,(u64)client,(u64)RESP_CORS,(u64)es_strlen(RESP_CORS),0,0,0);
            NC(G,close,(u64)client,0,0,0,0,0);
        } else {
            NC(G,close,(u64)client,0,0,0,0,0);
        }
    }
    return got_input;
}

/* ── DualSense → NES pad (menu navigation) ──────────────────────────────── */
static u8 ds_to_nes(u32 b) {
    u8 r=0;
    if (b&0x00004000) r|=0x01; /* CROSS    → A */
    if (b&0x00008000) r|=0x02; /* SQUARE   → B */
    if (b&0x00001000) r|=0x04; /* TRIANGLE → Sel */
    if (b&0x00002000) r|=0x08; /* CIRCLE   → Start */
    if (b&0x00000008) r|=0x08; /* OPTIONS  → Start */
    if (b&0x00000010) r|=0x10; /* UP */
    if (b&0x00000040) r|=0x20; /* DOWN */
    if (b&0x00000080) r|=0x40; /* LEFT */
    if (b&0x00000020) r|=0x80; /* RIGHT */
    if (b&0x00000400) r=0xFE;  /* L1 → Menu */
    if (b&0x00000800) r=0xFF;  /* R1 → Exit */
    return r;
}

static s32 read_native_pad(void *G, void *pad_read, s32 pad_h, u8 *pbuf) {
    if (pad_h<0||!pad_read) return -1;
    for (int i=0;i<128;i++) pbuf[i]=0;
    s32 n=(s32)NC(G,pad_read,(u64)pad_h,(u64)pbuf,1,0,0,0);
    if (n<=0||(u32)n>=0x80000000) return -1;
    u32 raw=*(u32*)pbuf;
    if (raw&0x80000000) return -1;
    return (s32)ds_to_nes(raw&0x001FFFFF);
}

/* DualSense → libretro joypad bitmask */
static u16 ds_to_retro(u32 ds) {
    u16 r=0;
    if (ds&0x00004000) r|=(1<<RETRO_DEVICE_ID_JOYPAD_B);
    if (ds&0x00002000) r|=(1<<RETRO_DEVICE_ID_JOYPAD_A);
    if (ds&0x00008000) r|=(1<<RETRO_DEVICE_ID_JOYPAD_X);
    if (ds&0x00001000) r|=(1<<RETRO_DEVICE_ID_JOYPAD_Y);
    if (ds&0x00000001) r|=(1<<RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (ds&0x00000008) r|=(1<<RETRO_DEVICE_ID_JOYPAD_START);
    if (ds&0x00000010) r|=(1<<RETRO_DEVICE_ID_JOYPAD_UP);
    if (ds&0x00000040) r|=(1<<RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (ds&0x00000080) r|=(1<<RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (ds&0x00000020) r|=(1<<RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (ds&0x00000400) r|=(1<<RETRO_DEVICE_ID_JOYPAD_L);
    if (ds&0x00000800) r|=(1<<RETRO_DEVICE_ID_JOYPAD_R);
    if (ds&0x00000100) r|=(1<<RETRO_DEVICE_ID_JOYPAD_L2);
    if (ds&0x00000200) r|=(1<<RETRO_DEVICE_ID_JOYPAD_R2);
    return r;
}

/* ── NES reset (for built-in NES path) ──────────────────────────────────── */
static void init_ntsc(struct NES *nes) {
    nes->is_pal=0; nes->cpu_freq=1789773; nes->num_scanlines=262;
    nes->fc_step[0][0]=7457;  nes->fc_step[0][1]=14913;
    nes->fc_step[0][2]=22371; nes->fc_step[0][3]=29828;
    nes->fc_step[0][4]=29829; nes->fc_step[0][5]=29830;
    nes->fc_step[1][0]=7457;  nes->fc_step[1][1]=14913;
    nes->fc_step[1][2]=22371; nes->fc_step[1][3]=29829;
    nes->fc_step[1][4]=37281; nes->fc_step[1][5]=37282;
}
static void init_pal(struct NES *nes) {
    nes->is_pal=1; nes->cpu_freq=1662607; nes->num_scanlines=312;
    nes->fc_step[0][0]=8313;  nes->fc_step[0][1]=16627;
    nes->fc_step[0][2]=24939; nes->fc_step[0][3]=33252;
    nes->fc_step[0][4]=33253; nes->fc_step[0][5]=33254;
    nes->fc_step[1][0]=8313;  nes->fc_step[1][1]=16627;
    nes->fc_step[1][2]=24939; nes->fc_step[1][3]=33253;
    nes->fc_step[1][4]=41565; nes->fc_step[1][5]=41566;
}
static void nes_reset_state(struct NES *nes) {
    u8 *p=(u8*)nes; for (u32 i=0;i<sizeof(struct NES);i++) p[i]=0;
    init_ntsc(nes); nes->noise.shift_reg=1;
}

/* ── libretro callbacks ─────────────────────────────────────────────────── */
/* XRGB8888 → BGRA8888 swap (PS5 framebuffer format) */
static u32 swap_rb(u32 px) {
    return ((px&0x000000FF)<<16)|((px&0x0000FF00))|((px&0x00FF0000)>>16)|0xFF000000;
}

static void retro_video_cb(const void *data, u32 w, u32 h, u64 pitch) {
    if (!data||!g_vid_flip_fn) return;
    g_core_w=w; g_core_h=h;
    u32 *fb = g_fbs[g_active];
    const u32 *src = (const u32*)data;
    u32 pitch32 = (u32)(pitch/4);
    for (u32 y=0; y<SCR_H; y++) {
        u32 sy = y*h/SCR_H;
        for (u32 x=0; x<SCR_W; x++) {
            u32 sx = x*w/SCR_W;
            fb[y*SCR_W+x] = swap_rb(src[sy*pitch32+sx]);
        }
    }
    NC(g_gadget, g_vid_flip_fn, (u64)g_video_h, (u64)g_active, 1, g_total_frames, 0, 0);
    if (g_wait_eq_fn && g_eq) {
        u8 evt[64]; s32 cnt=0;
        NC(g_gadget, g_wait_eq_fn, g_eq, (u64)evt, 1, (u64)&cnt, 0, 0);
    }
    g_active^=1; g_total_frames++;
}

static u64 retro_audio_batch_cb(const s16 *data, u64 frames) {
    if (!data||!g_aud_out_fn||g_aud_h<0) return frames;
    u64 pos=0;
    while (pos<frames) {
        u64 chunk=frames-pos;
        if ((u64)g_abuf_pos+chunk > 256) chunk=256-(u64)g_abuf_pos;
        es_memcpy(g_abuf + g_abuf_pos*2, data + pos*2, chunk*4);
        g_abuf_pos+=(int)chunk; pos+=chunk;
        if (g_abuf_pos>=256) {
            NC(g_gadget, g_aud_out_fn, (u64)g_aud_h, (u64)g_abuf, 0,0,0,0);
            g_abuf_pos=0;
        }
    }
    return frames;
}

static void retro_input_poll_cb(void) {
    if (g_pad_h<0||!g_pad_read_fn) { g_retro_pad=0; return; }
    es_memset(g_pad_buf,0,128);
    s32 n=(s32)NC(g_gadget,g_pad_read_fn,(u64)g_pad_h,(u64)g_pad_buf,1,0,0,0);
    if (n<=0||(u32)n>=0x80000000) { g_retro_pad=0; return; }
    u32 raw=*(u32*)g_pad_buf;
    if (raw&0x80000000) { g_retro_pad=0; return; }
    /* L1 = menu, R1 = exit */
    if (raw&0x00000400) { g_want_menu=1; }
    if (raw&0x00000800) { g_want_exit=1; }
    g_retro_pad = ds_to_retro(raw & 0x001FFFFF);
}

static s16 retro_input_state_cb(u32 port, u32 device, u32 index, u32 id) {
    if (port!=0||device!=RETRO_DEVICE_JOYPAD||index!=0) return 0;
    if (id>15) return 0;
    return (g_retro_pad & (1u<<id)) ? 1 : 0;
}

static int retro_env_cb(u32 cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return 1; /* accept whatever format the core advertises */
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (data) {
            struct retro_game_geometry *g=(struct retro_game_geometry*)data;
            g_core_w=g->base_width; g_core_h=g->base_height;
        }
        return 1;
    default:
        return 0;
    }
}

/* ── Core loader ─────────────────────────────────────────────────────────── */
static int load_core(struct core_instance *ci, u8 *data, u64 size,
                     void *G, void *mmap_fn, void *munmap_fn) {
    if (size < 64) return 0;

    /* Map binary as RWX */
    void *mem = (void*)NC(G, mmap_fn, 0, size, 7, 0x1002, (u64)-1, 0);
    if ((s64)mem == -1 || !mem) return 0;
    es_memcpy(mem, data, size);

    u64 base = (u64)mem;

    /* Call _core_start(base, &hdr) at byte 0.
     * Signature: void _core_start(u64 mapped_base, struct core_header *out) */
    struct core_header hdr;
    typedef void (*core_start_t)(u64, struct core_header *);
    ((core_start_t)base)(base, &hdr);

    if (hdr.magic != CORE_MAGIC) {
        NC(G, munmap_fn, base, size, 0, 0, 0, 0);
        (void)munmap_fn;
        return 0;
    }

    ci->base = (u8*)mem;
    ci->size = size;

#define BIND(field, off) \
    ci->field = (void*)(base + (u64)hdr.off)

    BIND(retro_init,                 retro_init_off);
    BIND(retro_deinit,               retro_deinit_off);
    BIND(retro_set_environment,      retro_set_environment_off);
    BIND(retro_set_video_refresh,    retro_set_video_refresh_off);
    BIND(retro_set_audio_sample,     retro_set_audio_sample_off);
    BIND(retro_set_audio_sample_batch, retro_set_audio_sample_batch_off);
    BIND(retro_set_input_poll,       retro_set_input_poll_off);
    BIND(retro_set_input_state,      retro_set_input_state_off);
    BIND(retro_get_system_info,      retro_get_system_info_off);
    BIND(retro_get_system_av_info,   retro_get_system_av_info_off);
    BIND(retro_load_game,            retro_load_game_off);
    BIND(retro_unload_game,          retro_unload_game_off);
    BIND(retro_run,                  retro_run_off);
    BIND(retro_reset,                retro_reset_off);
#undef BIND
    (void)munmap_fn;
    return 1;
}

/* ── _start entry point ─────────────────────────────────────────────────── */
__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;
    g_gadget = G;
    ext->step = 1;

    void *usleep    = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelUsleep");
    void *cancel    = SYM(G,D,LIBKERNEL_HANDLE,"scePthreadCancel");
    void *load_mod  = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelLoadStartModule");
    void *alloc_dm  = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelAllocateDirectMemory");
    void *map_dm    = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelMapDirectMemory");
    void *dm_size   = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelGetDirectMemorySize");
    void *create_eq = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelCreateEqueue");
    void *wait_eq   = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelWaitEqueue");
    void *mmap      = SYM(G,D,LIBKERNEL_HANDLE,"mmap");
    void *munmap    = SYM(G,D,LIBKERNEL_HANDLE,"munmap");
    void *kopen     = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelOpen");
    void *kread     = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelRead");
    void *kwrite    = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelWrite");
    void *kclose    = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelClose");
    void *kmkdir    = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelMkdir");
    void *delete_eq = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelDeleteEqueue");
    void *recvfrom  = SYM(G,D,LIBKERNEL_HANDLE,"recvfrom");
    void *sendto    = SYM(G,D,LIBKERNEL_HANDLE,"sendto");
    void *accept    = SYM(G,D,LIBKERNEL_HANDLE,"accept");
    void *poll      = SYM(G,D,LIBKERNEL_HANDLE,"poll");
    void *setsockopt_fn = SYM(G,D,LIBKERNEL_HANDLE,"setsockopt");
    void *getsockname_fn = SYM(G,D,LIBKERNEL_HANDLE,"getsockname");
    void *getdents  = SYM(G,D,LIBKERNEL_HANDLE,"sceKernelGetdents");
    if (!getdents) getdents = SYM(G,D,LIBKERNEL_HANDLE,"getdents");

    s32 log_fd = ext->log_fd;
    u8 log_sa[16];
    for (int i=0;i<16;i++) log_sa[i]=ext->log_addr[i];
    s32 web_fd    = (s32)ext->dbg[0];
    u8 *web_page  = (u8*)ext->dbg[1];
    u64 web_len   = ext->dbg[2];
    s32 userId    = (s32)ext->dbg[3];
    s32 ftp_fd    = (s32)ext->dbg[4];
    s32 ftp_data_fd = (s32)ext->dbg[5];

    if (!usleep||!load_mod){ext->status=-1;ext->step=2;return;}

    s32 vid_mod = (s32)NC(G,load_mod,(u64)"libSceVideoOut.sprx",0,0,0,0,0);
    s32 aud_mod = (s32)NC(G,load_mod,(u64)"libSceAudioOut.sprx",0,0,0,0,0);

    void *vid_open  = SYM(G,D,vid_mod,"sceVideoOutOpen");
    void *vid_close = SYM(G,D,vid_mod,"sceVideoOutClose");
    void *vid_reg   = SYM(G,D,vid_mod,"sceVideoOutRegisterBuffers");
    void *vid_flip  = SYM(G,D,vid_mod,"sceVideoOutSubmitFlip");
    void *vid_rate  = SYM(G,D,vid_mod,"sceVideoOutSetFlipRate");
    void *vid_evt   = SYM(G,D,vid_mod,"sceVideoOutAddFlipEvent");
    void *aud_open  = SYM(G,D,aud_mod,"sceAudioOutOpen");
    void *aud_out   = SYM(G,D,aud_mod,"sceAudioOutOutput");
    void *aud_close = SYM(G,D,aud_mod,"sceAudioOutClose");

    ext->step=5;
    if (cancel){u64 gs=*(u64*)(eboot_base+EBOOT_GS_THREAD);if(gs)NC(G,cancel,gs,0,0,0,0,0);}
    NC(G,usleep,300000,0,0,0,0,0);

    s32 emu_vid=*(s32*)(eboot_base+EBOOT_VIDOUT);
    if (vid_close&&emu_vid>=0) NC(G,vid_close,(u64)emu_vid,0,0,0,0,0);
    NC(G,usleep,100000,0,0,0,0,0);

    s32 video=(s32)NC(G,vid_open,0xFF,0,0,0,0,0);
    if (video<0){ext->status=-10;ext->step=11;return;}
    g_video_h=video;

    u64 eq=0;
    if (create_eq) NC(G,create_eq,(u64)&eq,(u64)"esq",0,0,0,0);
    if (vid_evt&&eq) NC(G,vid_evt,eq,(u64)video,0,0,0,0);
    g_eq=eq; g_wait_eq_fn=wait_eq; g_vid_flip_fn=vid_flip;

    u64 mem_total=dm_size?(u64)NC(G,dm_size,0,0,0,0,0,0):0x300000000ULL;
    u64 phys=0;
    NC(G,alloc_dm,0,mem_total,FB_TOTAL,0x200000,3,(u64)&phys);
    void *vmem=0;
    NC(G,map_dm,(u64)&vmem,FB_TOTAL,0x33,0,phys,0x200000);
    if (!vmem){ext->status=-21;ext->step=22;return;}

    u8 attr[64]; for(int i=0;i<64;i++) attr[i]=0;
    *(u32*)(attr+0)=0x80000000; *(u32*)(attr+4)=1;
    *(u32*)(attr+12)=SCR_W; *(u32*)(attr+16)=SCR_H; *(u32*)(attr+20)=SCR_W;

    void *fbs[2]; fbs[0]=vmem; fbs[1]=(u8*)vmem+FB_ALIGNED;
    g_fbs[0]=(u32*)fbs[0]; g_fbs[1]=(u32*)fbs[1]; g_active=0;

    if (NC(G,vid_reg,(u64)video,0,(u64)fbs,2,(u64)attr,0)!=0){ext->status=-30;ext->step=30;return;}
    if (vid_rate) NC(G,vid_rate,(u64)video,0,0,0,0,0);
    clear_fb(g_fbs[0]); clear_fb(g_fbs[1]);

    struct NES *nes=(struct NES*)NC(G,mmap,0,sizeof(struct NES)+0x10000,3,0x1002,(u64)-1,0);
    if ((s64)nes==-1){ext->status=-40;ext->step=16;return;}
    nes_reset_state(nes);

    u8 *rom_buf=(u8*)NC(G,mmap,0,0xC0000,3,0x1002,(u64)-1,0);
    if ((s64)rom_buf==-1) rom_buf=0;
    u8 *chr_ram=(u8*)NC(G,mmap,0,0x2000,3,0x1002,(u64)-1,0);

    NC(G,load_mod,(u64)"libSceUserService.sprx",0,0,0,0,0);
    if (aud_close) for(int h=0;h<8;h++) NC(G,aud_close,(u64)h,0,0,0,0,0);

    s32 audio_h=-1;
    if (aud_open) audio_h=(s32)NC(G,aud_open,0xFF,0,0,SAMPLES_PER_BUF,SAMPLE_RATE,AUDIO_S16_STEREO);
    g_aud_out_fn=aud_out; g_aud_h=audio_h;

    s32 pad_mod=(s32)NC(G,load_mod,(u64)"libScePad.sprx",0,0,0,0,0);
    void *pad_init_fn=SYM(G,D,pad_mod,"scePadInit");
    void *pad_geth   =SYM(G,D,pad_mod,"scePadGetHandle");
    void *pad_read   =SYM(G,D,pad_mod,"scePadRead");
    if (pad_init_fn) NC(G,pad_init_fn,0,0,0,0,0,0);
    s32 pad_h=-1;
    if (pad_geth) pad_h=(s32)NC(G,pad_geth,(u64)userId,0,0,0,0,0);
    g_pad_read_fn=pad_read; g_pad_h=pad_h;

    nes->gadget=G; nes->audio_out_fn=aud_out; nes->audio_handle=audio_h;
    nes->noise.shift_reg=1;

    udp_log(G,sendto,log_fd,log_sa,"EmulationStation PS5 v1.0\n");

    /* ── FTP + ROM scan ─────────────────────────────────────────────────── */
    struct rom_entry *roms=(struct rom_entry*)NC(G,mmap,0,
        sizeof(struct rom_entry)*MAX_ROMS,3,0x1002,(u64)-1,0);
    int rom_count=0;
    const char *rom_dir=ROM_DIR;

    if ((s64)roms!=-1) {
        u8 *scr=nes->screen;
        for(int i=0;i<NES_W*NES_H;i++) scr[i]=COL_BG;
        draw_centered(scr,50,"EmulationStation PS5",COL_BRAND);
        draw_centered(scr,70,"by EgyDevTeam",COL_HEAD);
        draw_centered(scr,110,"FTP Server on port 1337",COL_NORM);
        draw_centered(scr,130,"Upload ROMs + cores...",COL_DIM);
        scale_to_framebuf(g_fbs[0],scr,0); scale_to_framebuf(g_fbs[1],scr,0);
        NC(G,vid_flip,(u64)video,0,1,0,0,0);

        rom_count=ftp_serve(ftp_fd,ftp_data_fd,G,D,load_mod,mmap,
            kopen,kwrite,kclose,kmkdir,getdents,usleep,
            recvfrom,sendto,accept,getsockname_fn,
            log_fd,log_sa,userId,roms,MAX_ROMS);
    }

    /* scan filesystem for additional ROMs (all supported extensions) */
    if (kopen&&getdents&&(s64)roms!=-1) {
        s32 dfd=(s32)NC(G,kopen,(u64)ROM_DIR,0x20000,0,0,0,0);
        if (dfd<0){rom_dir="/savedata0/";dfd=(s32)NC(G,kopen,(u64)"/savedata0/",0x20000,0,0,0,0);}
        if (dfd>=0) {
            u8 *dbuf=(u8*)NC(G,mmap,0,0x2000,3,0x1002,(u64)-1,0);
            if ((s64)dbuf!=-1) {
                for(;;){
                    s32 nr=(s32)NC(G,getdents,(u64)dfd,(u64)dbuf,0x2000,0,0,0);
                    if(nr<=0) break;
                    int off=0;
                    while(off<nr&&rom_count<MAX_ROMS){
                        u16 reclen=*(u16*)(dbuf+off+4);
                        u8  namlen=*(u8*)(dbuf+off+7);
                        char *name=(char*)(dbuf+off+8);
                        if(reclen==0) break;
                        /* accept any system extension */
                        int ok=0;
                        for(int si=0;si<NUM_SYSTEMS&&!ok;si++)
                            ok=ext_match(name,SYS_EXT1[si],SYS_EXT2[si]);
                        if(namlen>0&&ok){
                            int dup=0;
                            for(int j=0;j<rom_count;j++){
                                int m=1;
                                for(int c=0;c<47;c++){
                                    if(roms[j].filename[c]!=name[c]){m=0;break;}
                                    if(!name[c]) break;
                                }
                                if(m){dup=1;break;}
                            }
                            if(!dup){
                                int k=0;
                                while(name[k]&&k<47){roms[rom_count].filename[k]=name[k];k++;}
                                roms[rom_count].filename[k]='\0';
                                extract_rom_name(name,roms[rom_count].display,MAX_NAME);
                                rom_count++;
                            }
                        }
                        off+=reclen;
                    }
                    if(rom_count>=MAX_ROMS) break;
                }
                if(munmap) NC(G,munmap,(u64)dbuf,0x2000,0,0,0,0);
            }
            NC(G,kclose,(u64)dfd,0,0,0,0,0);
        }
    }

    int has_web=(web_fd>=0&&poll&&accept);
    int input_src=0;
    u8  web_pad=0;
    s32 web_client=-1;
    int sys_sel=0;

    /* ── Outer loop: system picker → rom picker → run ───────────────────── */
outer:
    while(1) {
        /* SYSTEM PICKER */
        {
            int cursor=0,mframe=0;
            u8 prev_btn=0;
            for(;;){
                u8 btn=0;
                u8 wb=0;
                if(has_web){
                    int wg=web_handle(G,poll,accept,recvfrom,sendto,kclose,
                        setsockopt_fn,web_fd,&web_client,web_page,web_len,&wb);
                    if(wg) web_pad=wb;
                }
                s32 nb=read_native_pad(G,pad_read,pad_h,nes->oam);
                if(input_src==0){
                    if(nb>0&&nb<0xFE){input_src=1;btn=(u8)nb;}
                    else if(web_pad>0&&web_pad<0xFE){input_src=2;btn=web_pad;}
                    if(web_pad>=0xFE) btn=web_pad;
                    if(nb>=0xFE)      btn=(u8)nb;
                } else if(input_src==1){if(nb>=0)btn=(u8)nb;}
                  else{btn=web_pad;}
                if(btn>=0xFE) web_pad=0;
                if(btn==0xFF) goto es_done;

                u8 pressed=btn&~prev_btn;
                if(pressed&0x10){cursor--;if(cursor<0)cursor=NUM_SYSTEMS-1;}
                if(pressed&0x20){cursor++;if(cursor>=NUM_SYSTEMS)cursor=0;}
                if((pressed&0x01)||(pressed&0x08)){sys_sel=cursor;break;}
                prev_btn=btn;

                u8 *scr=nes->screen;
                for(int i=0;i<NES_W*NES_H;i++) scr[i]=COL_BG;
                draw_centered(scr,4,"EmulationStation",COL_BRAND);
                draw_centered(scr,14,"PS5  by EgyDevTeam",COL_BRAND);
                draw_hline(scr,24,4,NES_W-4,COL_LINE);
                draw_centered(scr,28,"SELECT SYSTEM",COL_HEAD);
                draw_hline(scr,38,4,NES_W-4,COL_LINE);
                for(int i=0;i<NUM_SYSTEMS;i++){
                    int y=50+i*18;
                    int sel=(i==cursor);
                    if(sel&&(mframe/8)%2) draw_char(scr,2,y,'>',COL_CUR);
                    draw_str(scr,16,y,SYS_NAME[i],sel?COL_SEL:COL_NORM);
                    draw_str(scr,16,y+8,SYS_SHORT[i],sel?SYS_COL[i]:COL_DIM);
                }
                draw_hline(scr,NES_H-16,4,NES_W-4,COL_LINE);
                draw_centered(scr,NES_H-12,"UP/DOWN:SELECT  A:CONFIRM  R1:EXIT",COL_DIM);

                scale_to_framebuf(g_fbs[g_active],scr,0);
                NC(G,vid_flip,(u64)video,(u64)g_active,1,g_total_frames,0,0);
                if(eq&&wait_eq){u8 evt[64];s32 cnt=0;NC(G,wait_eq,eq,(u64)evt,1,(u64)&cnt,0,0);}
                g_active^=1; g_total_frames++; mframe++;
            }
        }

        /* ROM PICKER for selected system */
        int selected=-1;
        {
            int cursor=0,scroll=0,mframe=0,hold=0,visible=17;
            u8 prev_btn=0;
            /* count roms for this system */
            int sys_count=0;
            int sys_idx[MAX_ROMS];
            for(int i=0;i<rom_count;i++)
                if(ext_match(roms[i].filename,SYS_EXT1[sys_sel],SYS_EXT2[sys_sel]))
                    sys_idx[sys_count++]=i;
            if(visible>sys_count) visible=sys_count;

            if(sys_count==0){
                /* no roms for this system */
                for(int f=0;f<120;f++){
                    u8 *scr=nes->screen;
                    for(int i=0;i<NES_W*NES_H;i++) scr[i]=COL_BG;
                    draw_centered(scr,80,SYS_NAME[sys_sel],SYS_COL[sys_sel]);
                    draw_centered(scr,100,"NO ROMS FOUND",COL_BRAND);
                    draw_centered(scr,118,"Upload via FTP port 1337",COL_DIM);
                    scale_to_framebuf(g_fbs[g_active],scr,0);
                    NC(G,vid_flip,(u64)video,(u64)g_active,1,g_total_frames,0,0);
                    if(eq&&wait_eq){u8 ev[64];s32 cnt=0;NC(G,wait_eq,eq,(u64)ev,1,(u64)&cnt,0,0);}
                    g_active^=1; g_total_frames++;
                }
                goto outer;
            }

            for(;;){
                u8 btn=0;
                u8 wb=0;
                if(has_web){int wg=web_handle(G,poll,accept,recvfrom,sendto,kclose,setsockopt_fn,web_fd,&web_client,web_page,web_len,&wb);if(wg)web_pad=wb;}
                s32 nb=read_native_pad(G,pad_read,pad_h,nes->oam);
                if(input_src==0){if(nb>0&&nb<0xFE){input_src=1;btn=(u8)nb;}else if(web_pad>0&&web_pad<0xFE){input_src=2;btn=web_pad;}if(web_pad>=0xFE)btn=web_pad;if(nb>=0xFE)btn=(u8)nb;}
                else if(input_src==1){if(nb>=0)btn=(u8)nb;}
                else{btn=web_pad;}
                if(btn>=0xFE) web_pad=0;
                if(btn==0xFF) goto es_done;
                if(btn==0xFE) goto outer;  /* L1 = back to system picker */

                u8 pressed=btn&~prev_btn;
                int move=0;
                if(btn&0x10){hold++;if((pressed&0x10)||(hold>12&&hold%4==0))move=-1;}
                else if(btn&0x20){hold++;if((pressed&0x20)||(hold>12&&hold%4==0))move=1;}
                else hold=0;
                if(move){cursor+=move;if(cursor<0)cursor=sys_count-1;if(cursor>=sys_count)cursor=0;if(cursor<scroll)scroll=cursor;if(cursor>=scroll+visible)scroll=cursor-visible+1;}
                if((pressed&0x01)||(pressed&0x08)){selected=sys_idx[cursor];break;}
                prev_btn=btn;

                u8 *scr=nes->screen;
                for(int i=0;i<NES_W*NES_H;i++) scr[i]=COL_BG;
                draw_centered(scr,4,SYS_NAME[sys_sel],SYS_COL[sys_sel]);
                draw_hline(scr,14,4,NES_W-4,COL_LINE);
                draw_centered(scr,18,"SELECT GAME",COL_HEAD);
                draw_hline(scr,28,4,NES_W-4,COL_LINE);
                int ly=36;
                for(int i=0;i<visible&&scroll+i<sys_count;i++){
                    int idx=scroll+i; int sel=(idx==cursor);
                    int y=ly+i*10;
                    if(sel&&(mframe/10)%2) draw_char(scr,2,y,'>',COL_CUR);
                    draw_str(scr,10,y,roms[sys_idx[idx]].display,sel?COL_SEL:COL_NORM);
                }
                draw_hline(scr,NES_H-16,4,NES_W-4,COL_LINE);
                draw_centered(scr,NES_H-12,"L1:BACK  R1:EXIT",COL_DIM);
                scale_to_framebuf(g_fbs[g_active],scr,0);
                NC(G,vid_flip,(u64)video,(u64)g_active,1,g_total_frames,0,0);
                if(eq&&wait_eq){u8 ev[64];s32 cnt=0;NC(G,wait_eq,eq,(u64)ev,1,(u64)&cnt,0,0);}
                g_active^=1; g_total_frames++; mframe++;
            }
        }
        if(selected<0) goto outer;

        /* build rom path */
        char rom_path[96];
        {int pi=0;const char *p=rom_dir;while(*p)rom_path[pi++]=*p++;
         const char *f=roms[selected].filename;while(*f&&pi<94)rom_path[pi++]=*f++;
         rom_path[pi]=0;}

        /* try to load core binary */
        struct core_instance ci;
        es_memset(&ci,0,sizeof(ci));
        int core_ok=0;
        u8 *core_data=0;
        u64 core_size=0;

        char core_path[80];
        {int pi=0;const char *p=ROM_DIR;while(*p)core_path[pi++]=*p++;
         const char *c=SYS_CORE[sys_sel];while(*c&&pi<78)core_path[pi++]=*c++;
         core_path[pi]=0;}

        if(kopen&&kread&&kclose&&mmap){
            s32 cfd=(s32)NC(G,kopen,(u64)core_path,0,0,0,0,0);
            if(cfd>=0){
                /* get size by reading up to 4MB */
                u8 *cbuf=(u8*)NC(G,mmap,0,0x400000,3,0x1002,(u64)-1,0);
                if((s64)cbuf!=-1){
                    s32 total=0;
                    for(;;){s32 got=(s32)NC(G,kread,(u64)cfd,(u64)(cbuf+total),(u64)(0x400000-total),0,0,0);if(got<=0)break;total+=got;}
                    if(total>0){core_data=cbuf;core_size=(u64)total;}
                }
                NC(G,kclose,(u64)cfd,0,0,0,0,0);
            }
        }
        if(core_data&&core_size>0)
            core_ok=load_core(&ci,core_data,core_size,G,mmap,munmap);

        /* no core for non-NES: show message */
        if(!core_ok&&sys_sel!=0){
            for(int f=0;f<180;f++){
                u8 *scr=nes->screen;
                for(int i=0;i<NES_W*NES_H;i++) scr[i]=COL_BG;
                draw_centered(scr,70,SYS_NAME[sys_sel],SYS_COL[sys_sel]);
                draw_centered(scr,90,"CORE NOT LOADED",COL_BRAND);
                draw_centered(scr,108,SYS_CORE[sys_sel],COL_DIM);
                draw_centered(scr,120,"Upload via FTP port 1337",COL_DIM);
                scale_to_framebuf(g_fbs[g_active],scr,0);
                NC(G,vid_flip,(u64)video,(u64)g_active,1,g_total_frames,0,0);
                if(eq&&wait_eq){u8 ev[64];s32 cnt=0;NC(G,wait_eq,eq,(u64)ev,1,(u64)&cnt,0,0);}
                g_active^=1; g_total_frames++;
            }
            if(core_data&&munmap) NC(G,munmap,(u64)core_data,0x400000,0,0,0,0);
            goto outer;
        }

        /* ── Built-in NES path (system==0, no core .bin) ──────────────── */
        if(!core_ok&&sys_sel==0){
            clear_fb(g_fbs[0]); clear_fb(g_fbs[1]);
            nes_reset_state(nes);
            nes->gadget=G; nes->audio_out_fn=aud_out; nes->audio_handle=audio_h;
            nes->noise.shift_reg=1; nes->rom_loaded=0;

            if(kopen&&kread&&kclose&&rom_buf){
                s32 fd=(s32)NC(G,kopen,(u64)rom_path,0,0,0,0,0);
                if(fd>=0){
                    u8 hdr[16];
                    s32 hr=(s32)NC(G,kread,(u64)fd,(u64)hdr,16,0,0,0);
                    if(hr==16&&hdr[0]=='N'&&hdr[1]=='E'&&hdr[2]=='S'&&hdr[3]==0x1A){
                        nes->prg_size=hdr[4]*0x4000; nes->chr_size=hdr[5]*0x2000;
                        nes->chr_banks=hdr[5]; nes->mirror=hdr[6]&1;
                        nes->mapper=(hdr[7]&0xF0)|((hdr[6]>>4)&0x0F);
                        nes->prg_banks=hdr[4];
                        int total=0;
                        while(total<nes->prg_size){s32 got=(s32)NC(G,kread,(u64)fd,(u64)(rom_buf+total),(u64)(nes->prg_size-total),0,0,0);if(got<=0)break;total+=got;}
                        nes->prg=rom_buf;
                        if(nes->chr_size>0){
                            total=0;
                            while(total<nes->chr_size){s32 got=(s32)NC(G,kread,(u64)fd,(u64)(rom_buf+nes->prg_size+total),(u64)(nes->chr_size-total),0,0,0);if(got<=0)break;total+=got;}
                            nes->chr=rom_buf+nes->prg_size; nes->chr_is_ram=0;
                        } else {nes->chr=chr_ram;nes->chr_size=0x2000;nes->chr_is_ram=1;for(int i=0;i<0x2000;i++)chr_ram[i]=0;}
                        if(nes->mapper==1) nes->mmc1_ctrl=0x0C;
                        else if(nes->mapper==69){for(int i=0;i<8;i++)nes->fme7_chr[i]=i;int last=(nes->prg_size/0x2000)-1;nes->fme7_prg[2]=last-1;nes->fme7_prg[3]=last;}
                        nes->sp=0xFD; nes->flags=0x04|0x20; nes->prev_irq_inhibit=0x04;
                        nes->pc=cpu_read16(nes,0xFFFC); nes->rom_loaded=1;
                        if(nes->prg_size>=0x200&&nes->prg[0x0108]==0x11) init_pal(nes);
                    }
                    NC(G,kclose,(u64)fd,0,0,0,0,0);
                }
            }
            if(!nes->rom_loaded) goto outer;

            int back_to_menu=0;
            for(;;){
                u8 wb=0;
                if(has_web){int wg=web_handle(G,poll,accept,recvfrom,sendto,kclose,setsockopt_fn,web_fd,&web_client,web_page,web_len,&wb);if(wg)web_pad=wb;}
                s32 nb=read_native_pad(G,pad_read,pad_h,nes->oam);
                if(input_src==0){if(nb>0&&nb<0xFE){input_src=1;nes->pad_state=(u8)nb;}else if(web_pad>0&&web_pad<0xFE){input_src=2;nes->pad_state=web_pad;}if(web_pad>=0xFE)nes->pad_state=web_pad;if(nb>=0xFE)nes->pad_state=(u8)nb;}
                else if(input_src==1){if(nb>=0)nes->pad_state=(u8)nb;}
                else{nes->pad_state=web_pad;}
                if(nes->pad_state>=0xFE) web_pad=0;
                if(nes->pad_state==0xFF) goto es_done;
                if(nes->pad_state==0xFE){back_to_menu=1;nes->pad_state=0;break;}
                run_frame(nes);
                scale_to_framebuf(g_fbs[g_active],nes->screen,nes->ppu_mask);
                NC(G,vid_flip,(u64)video,(u64)g_active,1,g_total_frames,0,0);
                apu_flush(nes);
                if(eq&&wait_eq){u8 ev[64];s32 cnt=0;NC(G,wait_eq,eq,(u64)ev,1,(u64)&cnt,0,0);}
                g_active^=1; g_total_frames++; ext->frame_count=g_total_frames;
            }
            if(back_to_menu) goto outer;
            goto es_done;
        }

        /* ── libretro core run loop ────────────────────────────────────── */
        if(core_ok){
            g_abuf_pos=0; g_want_menu=0; g_want_exit=0;
            ci.retro_set_environment(retro_env_cb);
            ci.retro_set_video_refresh(retro_video_cb);
            ci.retro_set_audio_sample_batch(retro_audio_batch_cb);
            ci.retro_set_input_poll(retro_input_poll_cb);
            ci.retro_set_input_state(retro_input_state_cb);
            if(ci.retro_set_audio_sample) ci.retro_set_audio_sample(0);
            ci.retro_init();

            /* load ROM data */
            if(kopen&&kread&&kclose&&rom_buf){
                s32 fd=(s32)NC(G,kopen,(u64)rom_path,0,0,0,0,0);
                if(fd>=0){
                    s32 total=0;
                    while(total<0xBFFFF){s32 got=(s32)NC(G,kread,(u64)fd,(u64)(rom_buf+total),(u64)(0xBFFFF-total),0,0,0);if(got<=0)break;total+=got;}
                    NC(G,kclose,(u64)fd,0,0,0,0,0);
                    struct retro_game_info gi;
                    gi.path=rom_path; gi.data=rom_buf; gi.size=(u64)total; gi.meta=0;
                    if(ci.retro_load_game(&gi)){
                        for(;;){
                            g_want_menu=0; g_want_exit=0;
                            ci.retro_run();
                            ext->frame_count=g_total_frames;
                            if(g_want_exit) goto es_done;
                            if(g_want_menu){ci.retro_unload_game();ci.retro_deinit();if(core_data&&munmap)NC(G,munmap,(u64)core_data,0x400000,0,0,0,0);goto outer;}
                        }
                    }
                }
            }
            ci.retro_deinit();
            if(core_data&&munmap) NC(G,munmap,(u64)core_data,0x400000,0,0,0,0);
        }
        goto outer;
    } /* end outer while */

es_done:
    udp_log(G,sendto,log_fd,log_sa,"ES shutting down\n");
    if(aud_close&&audio_h>=0) NC(G,aud_close,(u64)audio_h,0,0,0,0,0);
    clear_fb(g_fbs[0]); clear_fb(g_fbs[1]);
    NC(G,vid_flip,(u64)video,(u64)g_active,1,g_total_frames,0,0);
    if(usleep) NC(G,usleep,50000,0,0,0,0,0);
    if(vid_close&&video>=0) NC(G,vid_close,(u64)video,0,0,0,0,0);
    if(delete_eq&&eq) NC(G,delete_eq,eq,0,0,0,0,0);
    if(web_client>=0&&kclose) NC(G,kclose,(u64)web_client,0,0,0,0,0);
    if(web_fd>=0&&kclose) NC(G,kclose,(u64)web_fd,0,0,0,0,0);
    if(munmap){
        if((s64)nes!=-1) NC(G,munmap,(u64)nes,(u64)(sizeof(struct NES)+0x10000),0,0,0,0);
        if(rom_buf) NC(G,munmap,(u64)rom_buf,0xC0000,0,0,0,0);
        if(chr_ram&&(s64)chr_ram!=-1) NC(G,munmap,(u64)chr_ram,0x2000,0,0,0,0);
        if((s64)roms!=-1) NC(G,munmap,(u64)roms,(u64)(sizeof(struct rom_entry)*MAX_ROMS),0,0,0,0);
    }
    ext->status=0; ext->step=99; ext->frame_count=g_total_frames;
}
