/* nes_core.c – PS5 libretro NES core
 * Wraps the EmuC NES emulator (cpu/ppu/apu/bus) behind the libretro API.
 * Compiled as a flat position-independent binary; the struct core_header at
 * byte 0 of the binary holds byte-offsets of every API function so the
 * frontend can call them after mapping this binary into executable memory.
 */

#include "../src/core.h"
#include "../src/nes.h"
#include "../src/tables.h"
#include "../src/libretro.h"

/* ── Forward declarations ──────────────────────────────────────────────── */
static void core_retro_init(void);
static void core_retro_deinit(void);
static void core_retro_set_environment(retro_environment_t cb);
static void core_retro_set_video_refresh(retro_video_refresh_t cb);
static void core_retro_set_audio_sample(retro_audio_sample_t cb);
static void core_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
static void core_retro_set_input_poll(retro_input_poll_t cb);
static void core_retro_set_input_state(retro_input_state_t cb);
static void core_retro_get_system_info(struct retro_system_info *info);
static void core_retro_get_system_av_info(struct retro_system_av_info *info);
static int  core_retro_load_game(const struct retro_game_info *game);
static void core_retro_unload_game(void);
static void core_retro_run(void);
static void core_retro_reset(void);

/* ── Core header – MUST be first in .text._core_start ─────────────────── */
/* All _off fields are absolute addresses in a binary linked at base 0,
 * which equals the byte offset of each function from binary start. */
__attribute__((section(".text._core_start")))
const struct core_header CORE_HDR = {
    .magic                          = CORE_MAGIC,
    .version                        = CORE_VERSION,
    .retro_init_off                 = (u32)(u64)core_retro_init,
    .retro_deinit_off               = (u32)(u64)core_retro_deinit,
    .retro_set_environment_off      = (u32)(u64)core_retro_set_environment,
    .retro_set_video_refresh_off    = (u32)(u64)core_retro_set_video_refresh,
    .retro_set_audio_sample_off     = (u32)(u64)core_retro_set_audio_sample,
    .retro_set_audio_sample_batch_off = (u32)(u64)core_retro_set_audio_sample_batch,
    .retro_set_input_poll_off       = (u32)(u64)core_retro_set_input_poll,
    .retro_set_input_state_off      = (u32)(u64)core_retro_set_input_state,
    .retro_get_system_info_off      = (u32)(u64)core_retro_get_system_info,
    .retro_get_system_av_info_off   = (u32)(u64)core_retro_get_system_av_info,
    .retro_load_game_off            = (u32)(u64)core_retro_load_game,
    .retro_unload_game_off          = (u32)(u64)core_retro_unload_game,
    .retro_run_off                  = (u32)(u64)core_retro_run,
    .retro_reset_off                = (u32)(u64)core_retro_reset,
};

/* ── Static state ──────────────────────────────────────────────────────── */
static struct NES g_nes;
static u8  g_rom_buf[0xC0000];
static u8  g_chr_ram[0x2000];
static int g_loaded;
static int g_is_pal;

static retro_environment_t        g_env_cb;
static retro_video_refresh_t      g_video_cb;
static retro_audio_sample_t       g_audio_cb;
static retro_audio_sample_batch_t g_audio_batch_cb;
static retro_input_poll_t         g_input_poll_cb;
static retro_input_state_t        g_input_state_cb;

/* ── NES palette (XRGB8888) ────────────────────────────────────────────── */
static const u32 NES_PAL[64] = {
    0xFF545454,0xFF001E74,0xFF081090,0xFF300088,0xFF440064,0xFF5C0030,0xFF540400,0xFF3C1800,
    0xFF202A00,0xFF083A00,0xFF004000,0xFF003C00,0xFF00323C,0xFF000000,0xFF000000,0xFF000000,
    0xFF989698,0xFF084CC4,0xFF3032EC,0xFF5C1EE4,0xFF8814B0,0xFFA01464,0xFF982220,0xFF783C00,
    0xFF545A00,0xFF287200,0xFF087C00,0xFF007628,0xFF006678,0xFF000000,0xFF000000,0xFF000000,
    0xFFECEEEC,0xFF4C9AEC,0xFF787CEC,0xFFB062EC,0xFFE454EC,0xFFEC58B4,0xFFEC6A64,0xFFD48820,
    0xFFA0AA00,0xFF74C400,0xFF4CD020,0xFF38CC6C,0xFF38B4CC,0xFF3C3C3C,0xFF000000,0xFF000000,
    0xFFECEEEC,0xFFA8CCEC,0xFFBCBCEC,0xFFD4B2EC,0xFFECAEEC,0xFFECAED4,0xFFECB4B0,0xFFE4C490,
    0xFFCCD278,0xFFB4DE78,0xFFA8E290,0xFF98E2B4,0xFFA0D6E4,0xFFA0A2A0,0xFF000000,0xFF000000,
};

/* ── NES screen → video callback ───────────────────────────────────────── */
static u32 g_frame_buf[256 * 240];

static void flush_video(void) {
    if (!g_video_cb) return;
    const u8 *pal = g_nes.palette;
    const u8 *scr = g_nes.screen;
    for (int i = 0; i < 256 * 240; i++)
        g_frame_buf[i] = NES_PAL[pal[scr[i] & 0x3F] & 0x3F];
    g_video_cb(g_frame_buf, 256, 240, 256 * 4);
}

/* ── Audio output hook (called from apu.c via nes->audio_out_fn) ───────── */
/* apu_flush() calls: NC(G, nes->audio_out_fn, handle, buf, samples, 0,0,0)
 * We intercept at the audio_out_fn pointer by replacing it with a stub
 * that routes audio to g_audio_batch_cb.                                    */
static void audio_out_stub(u64 handle, u64 buf_addr, u64 samples,
                            u64 a4, u64 a5, u64 a6) {
    (void)handle; (void)a4; (void)a5; (void)a6;
    const s16 *buf = (const s16 *)(u64)buf_addr;
    if (g_audio_batch_cb)
        g_audio_batch_cb(buf, (u64)samples);
    else if (g_audio_cb) {
        for (u64 i = 0; i < (u64)samples; i++)
            g_audio_cb(buf[i * 2], buf[i * 2 + 1]);
    }
}

/* ── Helpers (inline, no libc) ─────────────────────────────────────────── */
static void mem_set(u8 *p, u8 v, u64 n) { for (u64 i=0;i<n;i++) p[i]=v; }
static int  mem_cmp(const u8 *a, const u8 *b, int n) {
    for(int i=0;i<n;i++) if(a[i]!=b[i]) return a[i]-b[i]; return 0;
}

/* ── libretro API implementation ───────────────────────────────────────── */
static void core_retro_init(void) {
    mem_set((u8*)&g_nes, 0, sizeof(g_nes));
    mem_set(g_chr_ram, 0, sizeof(g_chr_ram));
    g_loaded = 0;
}

static void core_retro_deinit(void) {
    g_loaded = 0;
}

static void core_retro_set_environment(retro_environment_t cb) {
    g_env_cb = cb;
    if (!cb) return;
    /* Advertise XRGB8888 pixel format */
    int fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

static void core_retro_set_video_refresh(retro_video_refresh_t cb)      { g_video_cb = cb; }
static void core_retro_set_audio_sample(retro_audio_sample_t cb)        { g_audio_cb = cb; }
static void core_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio_batch_cb = cb; }
static void core_retro_set_input_poll(retro_input_poll_t cb)             { g_input_poll_cb = cb; }
static void core_retro_set_input_state(retro_input_state_t cb)           { g_input_state_cb = cb; }

static void core_retro_get_system_info(struct retro_system_info *info) {
    info->library_name     = "EmuC NES";
    info->library_version  = "0.4";
    info->valid_extensions = "nes|rom";
    info->need_fullpath    = 0;
    info->block_extract    = 0;
}

static void core_retro_get_system_av_info(struct retro_system_av_info *info) {
    info->geometry.base_width   = 256;
    info->geometry.base_height  = 240;
    info->geometry.max_width    = 256;
    info->geometry.max_height   = 240;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = g_is_pal ? 50.0 : 60.0;
    info->timing.sample_rate    = 48000.0;
}

static int core_retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->data || game->size < 16) return 0;

    const u8 *rom = (const u8 *)game->data;
    if (mem_cmp(rom, (const u8*)"NES\x1A", 4) != 0) return 0;

    /* Parse iNES header */
    u32 prg_size = (u32)rom[4] * 0x4000;
    u32 chr_size = (u32)rom[5] * 0x2000;
    if (prg_size > sizeof(g_rom_buf)) return 0;

    /* Copy PRG */
    const u8 *src = rom + 16;
    for (u32 i = 0; i < prg_size && i < sizeof(g_rom_buf); i++)
        g_rom_buf[i] = src[i];

    /* Copy CHR */
    if (chr_size > 0) {
        src += prg_size;
        for (u32 i = 0; i < chr_size && (prg_size + i) < sizeof(g_rom_buf); i++)
            g_rom_buf[prg_size + i] = src[i];
    }

    /* Reset NES state */
    mem_set((u8*)&g_nes, 0, sizeof(g_nes));
    /* Timing */
    if (prg_size >= 0x200 && g_rom_buf[0x0108] == 0x11) {
        g_is_pal = 1;
        g_nes.is_pal     = 1;
        g_nes.cpu_freq   = 1662607;
        g_nes.num_scanlines = 312;
        g_nes.fc_step[0][0]=8313; g_nes.fc_step[0][1]=16627;
        g_nes.fc_step[0][2]=24939;g_nes.fc_step[0][3]=33252;
        g_nes.fc_step[0][4]=33253;g_nes.fc_step[0][5]=33254;
        g_nes.fc_step[1][0]=8313; g_nes.fc_step[1][1]=16627;
        g_nes.fc_step[1][2]=24939;g_nes.fc_step[1][3]=33253;
        g_nes.fc_step[1][4]=41565;g_nes.fc_step[1][5]=41566;
    } else {
        g_is_pal = 0;
        g_nes.is_pal     = 0;
        g_nes.cpu_freq   = 1789773;
        g_nes.num_scanlines = 262;
        g_nes.fc_step[0][0]=7457; g_nes.fc_step[0][1]=14913;
        g_nes.fc_step[0][2]=22371;g_nes.fc_step[0][3]=29828;
        g_nes.fc_step[0][4]=29829;g_nes.fc_step[0][5]=29830;
        g_nes.fc_step[1][0]=7457; g_nes.fc_step[1][1]=14913;
        g_nes.fc_step[1][2]=22371;g_nes.fc_step[1][3]=29829;
        g_nes.fc_step[1][4]=37281;g_nes.fc_step[1][5]=37282;
    }

    /* Cartridge */
    g_nes.prg_size  = (s32)prg_size;
    g_nes.chr_size  = (s32)(chr_size ? chr_size : 0x2000);
    g_nes.chr_banks = (s32)rom[5];
    g_nes.prg_banks = (s32)rom[4];
    g_nes.mirror    = rom[6] & 1;
    g_nes.mapper    = (rom[7] & 0xF0) | ((rom[6] >> 4) & 0x0F);
    g_nes.prg       = g_rom_buf;
    g_nes.chr       = chr_size ? (g_rom_buf + prg_size) : g_chr_ram;
    g_nes.chr_is_ram = (chr_size == 0);

    if (g_nes.mapper == 1)  g_nes.mmc1_ctrl = 0x0C;
    else if (g_nes.mapper == 69) {
        for (int i = 0; i < 8; i++) g_nes.fme7_chr[i] = i;
        int last = (s32)(prg_size / 0x2000) - 1;
        g_nes.fme7_prg[2] = last - 1;
        g_nes.fme7_prg[3] = last;
    }

    g_nes.noise.shift_reg = 1;
    g_nes.sp    = 0xFD;
    g_nes.flags = 0x04 | 0x20; /* I | U */
    g_nes.prev_irq_inhibit = 0x04;
    g_nes.pc    = cpu_read16(&g_nes, 0xFFFC);

    /* Route audio through our stub */
    g_nes.gadget       = 0;   /* not used for audio_out_fn path in core mode */
    g_nes.audio_out_fn = (void*)audio_out_stub;
    g_nes.audio_handle = 0;
    g_nes.rom_loaded   = 1;

    g_loaded = 1;
    return 1;
}

static void core_retro_unload_game(void) {
    g_loaded = 0;
}

static void core_retro_run(void) {
    if (!g_loaded) return;

    /* Build NES pad state from libretro input */
    if (g_input_poll_cb) g_input_poll_cb();
    u8 pad = 0;
    if (g_input_state_cb) {
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))      pad |= 0x01;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))      pad |= 0x02;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) pad |= 0x04;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  pad |= 0x08;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))     pad |= 0x10;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   pad |= 0x20;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   pad |= 0x40;
        if (g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  pad |= 0x80;
    }
    g_nes.pad_state = pad;

    /* Emulate one frame */
    run_frame(&g_nes);

    /* Push video */
    flush_video();

    /* Audio is flushed inside run_frame → apu_flush → audio_out_stub */
}

static void core_retro_reset(void) {
    if (!g_loaded) return;
    g_nes.sp    = 0xFD;
    g_nes.flags = 0x04 | 0x20;
    g_nes.prev_irq_inhibit = 0x04;
    g_nes.pc    = cpu_read16(&g_nes, 0xFFFC);
}
