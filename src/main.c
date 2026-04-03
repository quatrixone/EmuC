#include "nes.h"
#include "tables.h"
#include "ftp.h"

#define COL_BG    0x0F
#define COL_BRAND 0x16
#define COL_HEAD  0x21
#define COL_LINE  0x02
#define COL_SEL   0x2A
#define COL_NORM  0x30
#define COL_DIM   0x00
#define COL_CUR   0x16
#define COL_NUM   0x21

static const char *RESP_204K = "HTTP/1.1 204\r\nConnection:keep-alive\r\nAccess-Control-Allow-Origin:*\r\n\r\n";
static const char *RESP_CORS = "HTTP/1.1 204\r\nAccess-Control-Allow-Origin:*\r\nAccess-Control-Allow-Methods:POST\r\nConnection:keep-alive\r\n\r\n";

static void udp_log(void *G, void *sendto, s32 fd, u8 *sa, const char *msg) {
    if (fd < 0 || !sendto) return;
    NC(G, sendto, (u64)fd, (u64)msg, (u64)str_len(msg), 0, (u64)sa, 16);
}

static void clear_fb(u32 *fb) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
}

static void init_ntsc(struct NES *nes) {
    nes->is_pal = 0;
    nes->cpu_freq = 1789773;
    nes->num_scanlines = 262;
    nes->fc_step[0][0] = 7457;  nes->fc_step[0][1] = 14913;
    nes->fc_step[0][2] = 22371; nes->fc_step[0][3] = 29828;
    nes->fc_step[0][4] = 29829; nes->fc_step[0][5] = 29830;
    nes->fc_step[1][0] = 7457;  nes->fc_step[1][1] = 14913;
    nes->fc_step[1][2] = 22371; nes->fc_step[1][3] = 29829;
    nes->fc_step[1][4] = 37281; nes->fc_step[1][5] = 37282;
}

static void init_pal(struct NES *nes) {
    nes->is_pal = 1;
    nes->cpu_freq = 1662607;
    nes->num_scanlines = 312;
    nes->fc_step[0][0] = 8313;  nes->fc_step[0][1] = 16627;
    nes->fc_step[0][2] = 24939; nes->fc_step[0][3] = 33252;
    nes->fc_step[0][4] = 33253; nes->fc_step[0][5] = 33254;
    nes->fc_step[1][0] = 8313;  nes->fc_step[1][1] = 16627;
    nes->fc_step[1][2] = 24939; nes->fc_step[1][3] = 33253;
    nes->fc_step[1][4] = 41565; nes->fc_step[1][5] = 41566;
}

static int poll_ready(void *G, void *poll, s32 fd, s32 timeout_ms) {
    u8 pfd[8];
    *(s32*)pfd = fd;
    *(u16*)(pfd + 4) = 0x0001;
    *(u16*)(pfd + 6) = 0;
    return (s32)NC(G, poll, (u64)pfd, 1, (u64)timeout_ms, 0, 0, 0) > 0;
}

static int parse_pad_last(u8 *buf, s32 len) {
    int val = -1;
    for (s32 i = len - 2; i >= 1; i--) {
        if (buf[i] == '/' && buf[i+1] == 'b') {
            val = 0;
            for (s32 j = i + 2; j < len && j < i + 8; j++) {
                if (buf[j] >= '0' && buf[j] <= '9') val = val * 10 + (buf[j] - '0');
                else break;
            }
            break;
        }
    }
    return val;
}

static int count_posts(u8 *buf, s32 len) {
    int c = 0;
    for (s32 i = 0; i < len - 4; i++)
        if (buf[i] == 'P' && buf[i+1] == 'O' && buf[i+2] == 'S' && buf[i+3] == 'T') c++;
    return c;
}

static u8 web_handle(void *G, void *poll, void *accept, void *recv,
                     void *send, void *close, void *sso, s32 listen_fd,
                     s32 *keep_fd, u8 *page, u64 page_len, u8 *pad_out) {
    u8 got_input = 0;
    u8 req[512];

    if (*keep_fd >= 0) {
        for (int r = 0; r < 8; r++) {
            if (!poll_ready(G, poll, *keep_fd, 0)) break;
            s32 n = (s32)NC(G, recv, (u64)*keep_fd, (u64)req, 512, 0x80, 0, 0);
            if (n <= 0) { NC(G, close, (u64)*keep_fd, 0,0,0,0,0); *keep_fd = -1; break; }
            int v = parse_pad_last(req, n);
            if (v >= 0) {
                *pad_out = (u8)v;
                got_input = 1;
                int np = count_posts(req, n);
                for (int k = 0; k < np; k++)
                    NC(G, send, (u64)*keep_fd, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (!poll_ready(G, poll, listen_fd, 0)) break;

        u8 sa[16]; s32 sa_len = 16;
        s32 client = (s32)NC(G, accept, (u64)listen_fd, (u64)sa, (u64)&sa_len, 0, 0, 0);
        if (client < 0) break;

        if (sso) { s32 one = 1; NC(G, sso, (u64)client, 6, 1, (u64)&one, 4, 0); }

        if (!poll_ready(G, poll, client, 0)) {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
            continue;
        }

        s32 n = (s32)NC(G, recv, (u64)client, (u64)req, 512, 0x80, 0, 0);

        if (n > 7 && req[0] == 'P' && req[5] == '/' && req[6] == 'b') {
            int v = parse_pad_last(req, n);
            if (v >= 0) { *pad_out = (u8)v; got_input = 1; }
            NC(G, send, (u64)client, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            if (*keep_fd >= 0) NC(G, close, (u64)*keep_fd, 0,0,0,0,0);
            *keep_fd = client;
        } else if (n > 5 && req[0] == 'G' && req[4] == '/') {
            u64 off = 0;
            while (off < page_len) {
                u64 chunk = page_len - off;
                if (chunk > 2048) chunk = 2048;
                NC(G, send, (u64)client, (u64)(page + off), chunk, 0, 0, 0);
                off += chunk;
            }
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else if (n > 0 && req[0] == 'O') {
            NC(G, send, (u64)client, (u64)RESP_CORS, (u64)str_len(RESP_CORS), 0, 0, 0);
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        }
    }
    return got_input;
}

static void nes_reset(struct NES *nes, void *G, void *audio_fn, s32 audio_h) {
    u8 *p = (u8 *)nes;
    for (u32 i = 0; i < sizeof(struct NES); i++) p[i] = 0;
    init_ntsc(nes);
    nes->gadget = G;
    nes->audio_out_fn = audio_fn;
    nes->audio_handle = audio_h;
    nes->noise.shift_reg = 1;
}

static u8 ds_to_nes(u32 b) {
    u8 r = 0;
    if (b & 0x00004000) r |= 0x01;  /* CROSS    → A     */
    if (b & 0x00008000) r |= 0x02;  /* SQUARE   → B     */
    if (b & 0x00001000) r |= 0x04;  /* TRIANGLE → Sel   */
    if (b & 0x00002000) r |= 0x08;  /* CIRCLE   → Start */
    if (b & 0x00000008) r |= 0x08;  /* OPTIONS  → Start */
    if (b & 0x00000010) r |= 0x10;  /* UP              */
    if (b & 0x00000040) r |= 0x20;  /* DOWN            */
    if (b & 0x00000080) r |= 0x40;  /* LEFT            */
    if (b & 0x00000020) r |= 0x80;  /* RIGHT           */
    if (b & 0x00000400) r = 0xFE;   /* L1 → Menu       */
    if (b & 0x00000800) r = 0xFF;   /* R1 → Exit       */
    return r;
}

static s32 read_native_pad(void *G, void *pad_read, s32 pad_h, u8 *pbuf) {
    if (pad_h < 0 || !pad_read) return -1;
    for (int i = 0; i < 128; i++) pbuf[i] = 0;
    s32 n = (s32)NC(G, pad_read, (u64)pad_h, (u64)pbuf, 1, 0, 0, 0);
    if (n <= 0 || (u32)n >= 0x80000000) return -1;
    u32 raw = *(u32 *)pbuf;
    if (raw & 0x80000000) return -1;
    return (s32)ds_to_nes(raw & 0x001FFFFF);
}

__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;
    ext->step = 1;

    void *usleep    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    void *cancel    = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    void *load_mod  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    void *alloc_dm  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    void *map_dm    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    void *dm_size   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    void *create_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    void *wait_eq   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    void *mmap      = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    void *munmap    = SYM(G, D, LIBKERNEL_HANDLE, "munmap");
    void *kopen     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    void *kread     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead");
    void *kwrite    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    void *kclose    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    void *kmkdir    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir");
    void *delete_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    void *recvfrom  = SYM(G, D, LIBKERNEL_HANDLE, "recvfrom");
    void *sendto    = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    void *accept    = SYM(G, D, LIBKERNEL_HANDLE, "accept");
    void *poll      = SYM(G, D, LIBKERNEL_HANDLE, "poll");
    void *setsockopt_fn = SYM(G, D, LIBKERNEL_HANDLE, "setsockopt");
    void *getsockname_fn = SYM(G, D, LIBKERNEL_HANDLE, "getsockname");
    void *getdents  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetdents");
    if (!getdents)   getdents = SYM(G, D, LIBKERNEL_HANDLE, "getdents");

    s32 log_fd = ext->log_fd;
    u8 log_sa[16];
    for (int i = 0; i < 16; i++) log_sa[i] = ext->log_addr[i];

    s32 web_fd   = (s32)ext->dbg[0];
    u8 *web_page = (u8 *)ext->dbg[1];
    u64 web_len  = ext->dbg[2];
    s32 userId   = (s32)ext->dbg[3];
    s32 ftp_fd   = (s32)ext->dbg[4];
    s32 ftp_data_fd = (s32)ext->dbg[5];

    if (!usleep || !load_mod) { ext->status = -1; ext->step = 2; return; }

    s32 vid_mod = (s32)NC(G, load_mod, (u64)"libSceVideoOut.sprx", 0,0,0,0,0);
    s32 aud_mod = (s32)NC(G, load_mod, (u64)"libSceAudioOut.sprx", 0,0,0,0,0);

    void *vid_open  = SYM(G, D, vid_mod, "sceVideoOutOpen");
    void *vid_close = SYM(G, D, vid_mod, "sceVideoOutClose");
    void *vid_reg   = SYM(G, D, vid_mod, "sceVideoOutRegisterBuffers");
    void *vid_flip  = SYM(G, D, vid_mod, "sceVideoOutSubmitFlip");
    void *vid_rate  = SYM(G, D, vid_mod, "sceVideoOutSetFlipRate");
    void *vid_evt   = SYM(G, D, vid_mod, "sceVideoOutAddFlipEvent");
    void *aud_open  = SYM(G, D, aud_mod, "sceAudioOutOpen");
    void *aud_out   = SYM(G, D, aud_mod, "sceAudioOutOutput");
    void *aud_close = SYM(G, D, aud_mod, "sceAudioOutClose");

    ext->step = 5;

    if (cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(G, cancel, gs, 0,0,0,0,0);
    }
    NC(G, usleep, 300000, 0,0,0,0,0);

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (vid_close && emu_vid >= 0) NC(G, vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(G, usleep, 100000, 0,0,0,0,0);

    s32 video = (s32)NC(G, vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (video < 0) { ext->status = -10; ext->step = 11; return; }

    u64 eq = 0;
    if (create_eq) NC(G, create_eq, (u64)&eq, (u64)"nesq", 0,0,0,0);
    if (vid_evt && eq) NC(G, vid_evt, eq, (u64)video, 0,0,0,0);

    u64 mem_total = dm_size ? NC(G, dm_size, 0,0,0,0,0,0) : 0x300000000ULL;
    u64 phys = 0;
    NC(G, alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    void *vmem = 0;
    NC(G, map_dm, (u64)&vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!vmem) { ext->status = -21; ext->step = 22; return; }

    u8 attr[64];
    for (int i = 0; i < 64; i++) attr[i] = 0;
    *(u32*)(attr + 0)  = 0x80000000;
    *(u32*)(attr + 4)  = 1;
    *(u32*)(attr + 12) = SCR_W;
    *(u32*)(attr + 16) = SCR_H;
    *(u32*)(attr + 20) = SCR_W;

    void *fbs[2];
    fbs[0] = vmem;
    fbs[1] = (u8*)vmem + FB_ALIGNED;

    if (NC(G, vid_reg, (u64)video, 0, (u64)fbs, 2, (u64)attr, 0) != 0) {
        ext->status = -30; ext->step = 30; return;
    }
    if (vid_rate) NC(G, vid_rate, (u64)video, 0, 0,0,0,0);
    clear_fb((u32*)fbs[0]);
    clear_fb((u32*)fbs[1]);

    struct NES *nes = (struct NES *)NC(G, mmap, 0,
        sizeof(struct NES) + 0x10000, 3, 0x1002, (u64)-1, 0);
    if ((s64)nes == -1) { ext->status = -40; ext->step = 16; return; }

    nes_reset(nes, G, 0, -1);

    u8 *rom_buf = (u8 *)NC(G, mmap, 0, 0xC0000, 3, 0x1002, (u64)-1, 0);
    if ((s64)rom_buf == -1) rom_buf = 0;
    u8 *chr_ram = (u8 *)NC(G, mmap, 0, 0x2000, 3, 0x1002, (u64)-1, 0);

    NC(G, load_mod, (u64)"libSceUserService.sprx", 0,0,0,0,0);
    if (aud_close)
        for (int h = 0; h < 8; h++) NC(G, aud_close, (u64)h, 0,0,0,0,0);

    s32 audio_h = -1;
    if (aud_open)
        audio_h = (s32)NC(G, aud_open, 0xFF, 0, 0, SAMPLES_PER_BUF, SAMPLE_RATE, AUDIO_S16_STEREO);

    s32 pad_mod = (s32)NC(G, load_mod, (u64)"libScePad.sprx", 0,0,0,0,0);
    void *pad_init_fn = SYM(G, D, pad_mod, "scePadInit");
    void *pad_geth    = SYM(G, D, pad_mod, "scePadGetHandle");
    void *pad_read    = SYM(G, D, pad_mod, "scePadRead");
    if (pad_init_fn) NC(G, pad_init_fn, 0,0,0,0,0,0);
    s32 pad_h = -1;
    if (pad_geth) pad_h = (s32)NC(G, pad_geth, (u64)userId, 0, 0, 0, 0, 0);
    u8 pad_buf[128];

    nes->gadget = G;
    nes->audio_out_fn = aud_out;
    nes->audio_handle = audio_h;
    nes->noise.shift_reg = 1;

    udp_log(G, sendto, log_fd, log_sa, "EgyDevTeam NES EMU V0.4 by egycnq\n");
    udp_log(G, sendto, log_fd, log_sa, pad_h >= 0 ? "Native pad OK\n" : "Native pad N/A\n");

    struct rom_entry *roms = (struct rom_entry *)NC(G, mmap, 0,
        sizeof(struct rom_entry) * MAX_ROMS, 3, 0x1002, (u64)-1, 0);
    int rom_count = 0;
    const char *rom_dir = ROM_DIR;

    if ((s64)roms != -1) {
        /* Show FTP waiting screen */
        u8 *scr = nes->screen;
        for (int i = 0; i < NES_W * NES_H; i++) scr[i] = COL_BG;
        draw_centered(scr, 50, "EGYDEVTEAM", COL_BRAND);
        draw_centered(scr, 80, "NES EMULATOR", COL_HEAD);
        draw_centered(scr, 120, "FTP Server on port 1337", COL_NORM);
        draw_centered(scr, 145, "Waiting for ROMs...", COL_DIM);
        scale_to_framebuf((u32*)fbs[0], scr, 0);
        scale_to_framebuf((u32*)fbs[1], scr, 0);
        NC(G, vid_flip, (u64)video, 0, 1, 0, 0, 0);

        rom_count = ftp_serve(ftp_fd, ftp_data_fd,
                              G, D, load_mod, mmap, kopen, kwrite, kclose,
                              kmkdir, getdents, usleep,
                              recvfrom, sendto, accept,
                              getsockname_fn,
                              log_fd, log_sa, userId,
                              roms, MAX_ROMS);
        if (rom_count > 0) udp_log(G, sendto, log_fd, log_sa, "FTP ROMs loaded\n");
    }

    /* Scan filesystem for any additional ROMs not from FTP */
    if (kopen && getdents && (s64)roms != -1) {
        s32 dfd = (s32)NC(G, kopen, (u64)ROM_DIR, 0x20000, 0, 0, 0, 0);
        if (dfd < 0) {
            rom_dir = "/savedata0/";
            dfd = (s32)NC(G, kopen, (u64)"/savedata0/", 0x20000, 0, 0, 0, 0);
        }
        if (dfd >= 0) {
            u8 *dbuf = (u8 *)NC(G, mmap, 0, 0x2000, 3, 0x1002, (u64)-1, 0);
            if ((s64)dbuf != -1) {
                for (;;) {
                    s32 nread = (s32)NC(G, getdents, (u64)dfd, (u64)dbuf, 0x2000, 0, 0, 0);
                    if (nread <= 0) break;
                    int off = 0;
                    while (off < nread && rom_count < MAX_ROMS) {
                        u16 reclen = *(u16 *)(dbuf + off + 4);
                        u8 namlen  = *(u8 *)(dbuf + off + 7);
                        char *name = (char *)(dbuf + off + 8);
                        if (reclen == 0) break;
                        if (namlen > 0 && is_rom_file(name)) {
                            /* skip if already registered by FTP */
                            int dup = 0;
                            for (int j = 0; j < rom_count; j++) {
                                int match = 1;
                                for (int c = 0; c < 47; c++) {
                                    if (roms[j].filename[c] != name[c]) { match = 0; break; }
                                    if (!name[c]) break;
                                }
                                if (match) { dup = 1; break; }
                            }
                            if (!dup) {
                                int k = 0;
                                while (name[k] && k < 47) { roms[rom_count].filename[k] = name[k]; k++; }
                                roms[rom_count].filename[k] = '\0';
                                extract_rom_name(name, roms[rom_count].display, MAX_NAME);
                                rom_count++;
                            }
                        }
                        off += reclen;
                    }
                    if (rom_count >= MAX_ROMS) break;
                }
                if (munmap) NC(G, munmap, (u64)dbuf, 0x2000, 0,0,0,0);
            }
            NC(G, kclose, (u64)dfd, 0,0,0,0,0);
        }
    }

    if (rom_count == 0 && kopen && kclose) {
        s32 tfd = (s32)NC(G, kopen, (u64)"/savedata0/nes.rom", 0,0,0,0,0);
        if (tfd >= 0) {
            rom_dir = "/savedata0/";
            NC(G, kclose, (u64)tfd, 0,0,0,0,0);
            const char *fn = "nes.rom";
            int k = 0; while (fn[k]) { roms[0].filename[k] = fn[k]; k++; }
            roms[0].filename[k] = '\0';
            extract_rom_name(fn, roms[0].display, MAX_NAME);
            rom_count = 1;
        }
    }

    u32 total_frames = 0;
    int active = 0;
    int has_web = (web_fd >= 0 && poll && accept);
    int input_src = 0;
    u8 web_pad = 0;
    s32 web_client = -1;

    for (;;) {
        int selected = 0;

        if (rom_count > 0) {
            int cursor = 0, scroll = 0, mframe = 0, hold = 0;
            u8 prev_btn = 0;
            int visible = 17;
            if (visible > rom_count) visible = rom_count;

            for (;;) {
                u8 btn = 0;

                u8 wb = 0; int web_got = 0;
                if (has_web) {
                    web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                        setsockopt_fn, web_fd, &web_client, web_page, web_len, &wb);
                    if (web_got) web_pad = wb;
                }

                s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

                if (input_src == 0) {
                    if (nb > 0 && nb < 0xFE) {
                        input_src = 1;  btn = (u8)nb;
                        udp_log(G, sendto, log_fd, log_sa, "Input: native pad\n");
                    }
                    else if (web_got && web_pad > 0 && web_pad < 0xFE) {
                        input_src = 2; btn = web_pad;
                        udp_log(G, sendto, log_fd, log_sa, "Input: web controller\n");
                    }
                    if (web_got && web_pad >= 0xFE) btn = web_pad;
                    if (nb >= 0xFE) btn = (u8)nb;
                } else if (input_src == 1) {
                    if (nb >= 0) btn = (u8)nb;
                } else {
                    btn = web_pad;
                }

                if (btn >= 0xFE) web_pad = 0;
                if (btn == 0xFF) goto done;

                u8 pressed = btn & ~prev_btn;
                int move = 0;
                if (btn & 0x10) { hold++; if ((pressed & 0x10) || (hold > 12 && hold % 4 == 0)) move = -1; }
                else if (btn & 0x20) { hold++; if ((pressed & 0x20) || (hold > 12 && hold % 4 == 0)) move = 1; }
                else hold = 0;

                if (move) {
                    cursor += move;
                    if (cursor < 0) cursor = rom_count - 1;
                    if (cursor >= rom_count) cursor = 0;
                    if (cursor < scroll) scroll = cursor;
                    if (cursor >= scroll + visible) scroll = cursor - visible + 1;
                }
                if ((pressed & 0x01) || (pressed & 0x08)) { selected = cursor; break; }
                prev_btn = btn;

                u8 *scr = nes->screen;
                for (int i = 0; i < NES_W * NES_H; i++) scr[i] = COL_BG;
                draw_centered(scr, 6, "EGYDEVTEAM", COL_BRAND);
                draw_centered(scr, 16, "NES EMU V0.4 by egycnq", COL_BRAND);
                draw_hline(scr, 26, 8, NES_W - 8, COL_LINE);
                draw_centered(scr, 30, "SELECT A GAME", COL_HEAD);
                draw_hline(scr, 40, 8, NES_W - 8, COL_LINE);

                if (scroll > 0) draw_centered(scr, 42, "-", COL_DIM);

                int ly = 50;
                for (int i = 0; i < visible && scroll + i < rom_count; i++) {
                    int idx = scroll + i;
                    int iy = ly + i * 10;
                    int sel = (idx == cursor);
                    int num = idx + 1;
                    if (sel && (mframe / 10) % 2)
                        draw_char(scr, 2, iy, '>', COL_CUR);
                    u8 nc = sel ? COL_SEL : COL_NUM;
                    int nx = 16;
                    if (rom_count >= 100) { draw_char(scr, nx, iy, '0' + num / 100, nc); nx += 8; }
                    draw_char(scr, nx, iy, '0' + (num / 10) % 10, nc); nx += 8;
                    draw_char(scr, nx, iy, '0' + num % 10, nc); nx += 8;
                    draw_char(scr, nx, iy, '.', nc); nx += 8;
                    draw_str(scr, nx + 2, iy, roms[idx].display, sel ? COL_SEL : COL_NORM);
                }

                if (scroll + visible < rom_count)
                    draw_centered(scr, ly + visible * 10, "-", COL_DIM);

                draw_hline(scr, NES_H - 16, 8, NES_W - 8, COL_LINE);
                draw_centered(scr, NES_H - 12, "L1/ESC:MENU  R1/TAB:EXIT", COL_DIM);

                scale_to_framebuf((u32*)fbs[active], scr, 0);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active ^= 1;
                mframe++;
                total_frames++;
            }
        } else {
            for (int f = 0; f < 300; f++) {
                u8 *scr = nes->screen;
                for (int i = 0; i < NES_W * NES_H; i++) scr[i] = COL_BG;
                draw_centered(scr, 80, "EGYDEVTEAM", COL_BRAND);
                draw_centered(scr, 92, "NES EMU V0.4 by egycnq", COL_BRAND);
                draw_centered(scr, 116, "NO ROMS FOUND!", COL_BRAND);
                draw_centered(scr, 136, "PUT .ROM OR .NES", COL_DIM);
                draw_centered(scr, 148, "IN /SAVEDATA0/", COL_DIM);
                scale_to_framebuf((u32*)fbs[active], scr, 0);
                NC(G, vid_flip, (u64)video, (u64)active, 1, (u64)f, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active ^= 1;
            }
            break;
        }

        char rom_path[96];
        { int pi = 0; const char *p = rom_dir;
          while (*p) rom_path[pi++] = *p++;
          const char *f = roms[selected].filename;
          while (*f && pi < 94) rom_path[pi++] = *f++;
          rom_path[pi] = 0; }
        clear_fb((u32*)fbs[0]);
        clear_fb((u32*)fbs[1]);
        nes_reset(nes, G, aud_out, audio_h);

        nes->rom_loaded = 0;
        if (kopen && kread && kclose && rom_buf) {
            s32 fd = (s32)NC(G, kopen, (u64)rom_path, 0,0,0,0,0);
            if (fd >= 0) {
                u8 hdr[16];
                s32 hr = (s32)NC(G, kread, (u64)fd, (u64)hdr, 16, 0, 0, 0);
                if (hr == 16 && hdr[0]=='N' && hdr[1]=='E' && hdr[2]=='S' && hdr[3]==0x1A) {
                    nes->prg_size = hdr[4] * 0x4000;
                    nes->chr_size = hdr[5] * 0x2000;
                    nes->chr_banks = hdr[5];
                    nes->mirror = hdr[6] & 1;
                    nes->mapper = (hdr[7] & 0xF0) | ((hdr[6] >> 4) & 0x0F);
                    nes->prg_banks = hdr[4];

                    int total = 0;
                    while (total < nes->prg_size) {
                        s32 got = (s32)NC(G, kread, (u64)fd,
                            (u64)(rom_buf + total), (u64)(nes->prg_size - total), 0, 0, 0);
                        if (got <= 0) break;
                        total += got;
                    }
                    nes->prg = rom_buf;

                    if (nes->chr_size > 0) {
                        total = 0;
                        while (total < nes->chr_size) {
                            s32 got = (s32)NC(G, kread, (u64)fd,
                                (u64)(rom_buf + nes->prg_size + total),
                                (u64)(nes->chr_size - total), 0, 0, 0);
                            if (got <= 0) break;
                            total += got;
                        }
                        nes->chr = rom_buf + nes->prg_size;
                        nes->chr_is_ram = 0;
                    } else {
                        nes->chr = chr_ram;
                        nes->chr_size = 0x2000;
                        nes->chr_is_ram = 1;
                        for (int i = 0; i < 0x2000; i++) chr_ram[i] = 0;
                    }

                    if (nes->mapper == 1) nes->mmc1_ctrl = 0x0C;
                    else if (nes->mapper == 69) {
                        for (int i = 0; i < 8; i++) nes->fme7_chr[i] = i;
                        int last = (nes->prg_size / 0x2000) - 1;
                        nes->fme7_prg[2] = last - 1;
                        nes->fme7_prg[3] = last;
                    }

                    nes->sp = 0xFD;
                    nes->flags = F_I | F_U;
                    nes->prev_irq_inhibit = F_I;
                    nes->pc = cpu_read16(nes, 0xFFFC);
                    nes->rom_loaded = 1;

                    if (nes->prg_size >= 0x200 && nes->prg[0x0108] == 0x11)
                        init_pal(nes);
                }
                NC(G, kclose, (u64)fd, 0,0,0,0,0);
            }
        }

        if (!nes->rom_loaded) continue;

        udp_log(G, sendto, log_fd, log_sa, rom_path);
        udp_log(G, sendto, log_fd, log_sa, nes->is_pal ? " PAL\n" : " NTSC\n");

        u32 frame = 0;
        int pal_acc = 60;
        int back_to_menu = 0;

        for (;;) {
            u8 wb = 0; int web_got = 0;
            if (has_web) {
                web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                    setsockopt_fn, web_fd, &web_client, web_page, web_len, &wb);
                if (web_got) web_pad = wb;
            }

            s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

            if (input_src == 0) {
                if (nb > 0 && nb < 0xFE) {
                    input_src = 1; nes->pad_state = (u8)nb;
                    udp_log(G, sendto, log_fd, log_sa, "Input: native pad\n");
                }
                else if (web_got && web_pad > 0 && web_pad < 0xFE) {
                    input_src = 2; nes->pad_state = web_pad;
                    udp_log(G, sendto, log_fd, log_sa, "Input: web controller\n");
                }
                if (web_got && web_pad >= 0xFE) nes->pad_state = web_pad;
                if (nb >= 0xFE) nes->pad_state = (u8)nb;
            } else if (input_src == 1) {
                if (nb >= 0) nes->pad_state = (u8)nb;
            } else {
                nes->pad_state = web_pad;
            }

            if (nes->pad_state >= 0xFE) web_pad = 0;
            if (nes->pad_state == 0xFF) goto done;
            if (nes->pad_state == 0xFE) { back_to_menu = 1; nes->pad_state = 0; break; }

            int run_game = 1;
            if (nes->is_pal) {
                pal_acc += 50;
                if (pal_acc >= 60) pal_acc -= 60; else run_game = 0;
            }

            if (run_game) run_frame(nes);

            scale_to_framebuf((u32*)fbs[active], nes->screen, nes->ppu_mask);
            NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);

            if (nes->rom_loaded) apu_flush(nes);

            if (eq && wait_eq) {
                u8 evt[64]; s32 cnt = 0;
                NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
            }

            active ^= 1;
            if (run_game) frame++;
            total_frames++;
            ext->frame_count = total_frames;
        }

        if (!back_to_menu) break;
    }

done:
    udp_log(G, sendto, log_fd, log_sa, "Shutting down...\n");

    if (aud_close && audio_h >= 0)
        NC(G, aud_close, (u64)audio_h, 0,0,0,0,0);

    clear_fb((u32*)fbs[0]);
    clear_fb((u32*)fbs[1]);
    NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
    if (usleep) NC(G, usleep, 50000, 0,0,0,0,0);

    if (vid_close && video >= 0)
        NC(G, vid_close, (u64)video, 0,0,0,0,0);

    if (delete_eq && eq)
        NC(G, delete_eq, eq, 0,0,0,0,0);

    if (web_client >= 0 && kclose)
        NC(G, kclose, (u64)web_client, 0,0,0,0,0);

    if (web_fd >= 0 && kclose)
        NC(G, kclose, (u64)web_fd, 0,0,0,0,0);

    if (munmap) {
        if ((s64)nes != -1)
            NC(G, munmap, (u64)nes, (u64)(sizeof(struct NES) + 0x10000), 0,0,0,0);
        if (rom_buf)
            NC(G, munmap, (u64)rom_buf, 0xC0000, 0,0,0,0);
        if (chr_ram && (s64)chr_ram != -1)
            NC(G, munmap, (u64)chr_ram, 0x2000, 0,0,0,0);
        if ((s64)roms != -1)
            NC(G, munmap, (u64)roms, (u64)(sizeof(struct rom_entry) * MAX_ROMS), 0,0,0,0);
    }

    udp_log(G, sendto, log_fd, log_sa, "Clean exit\n");
    ext->status = 0;
    ext->step = 99;
    ext->frame_count = total_frames;
}