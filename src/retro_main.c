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
#define NUM_SYSTEMS 5

static const char *SYS_NAME[NUM_SYSTEMS] = {
    "Nintendo Ent. System",
    "Super Nintendo",
    "Sega Mega Drive",
    "Game Boy / Color",
    "Game Boy Advance",
};
static const char *SYS_SHORT[NUM_SYSTEMS] = {
    "NES", "SNES", "MEGA DRIVE", "GAME BOY", "GBA"
};
static const char *SYS_EXT1[NUM_SYSTEMS] = { ".nes", ".sfc", ".md",  ".gb",  ".gba" };
static const char *SYS_EXT2[NUM_SYSTEMS] = { ".rom", ".smc", ".bin", ".gbc", ""     };
static const char *SYS_CORE[NUM_SYSTEMS] = {
    "nes_core.bin", "snes_core.bin", "md_core.bin", "gb_core.bin", "gba_core.bin"
};
static const u8 SYS_COL[NUM_SYSTEMS] = { 0x15, 0x23, 0x14, 0x2A, 0x27 };

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
    if (size < sizeof(struct core_header)) return 0;
    struct core_header *hdr = (struct core_header*)data;
    if (hdr->magic != CORE_MAGIC) return 0;

    /* Map as executable */
    void *mem = (void*)NC(G, mmap_fn, 0, size, 7, 0x1002, (u64)-1, 0);
    if ((s64)mem == -1 || !mem) return 0;
    es_memcpy(mem, data, size);

    u64 base = (u64)mem;
    ci->base = (u8*)mem;
    ci->size = size;

#define BIND(field, off) \
    ci->field = (void*)(base + (u64)hdr->off)

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
