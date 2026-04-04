/* ps2_core.c – EmuC PlayStation 2 core (functional stub)
 *
 * ── PlayStation 2 Architecture Overview ────────────────────────────────────
 *
 * EE CPU (Emotion Engine):
 *   MIPS R5900 running at ~294 MHz.  The R5900 extends MIPS III with a 128-bit
 *   SIMD register file: each GP register is 128 bits wide and can hold two
 *   64-bit integers, four 32-bit integers, or four single-precision floats.
 *   This lets the EE execute multimedia operations (pack/unpack, parallel adds,
 *   multiplies) without a separate coprocessor for integer SIMD.
 *
 * VU0 / VU1 (Vector Units):
 *   Two 128-bit floating-point vector processing units.  Each VU has 32×128-bit
 *   VF registers (four floats each) and 16×16-bit VI integer registers.
 *   VU0 is tightly coupled to the EE (accessible as COP2) for vertex
 *   transformation.  VU1 is autonomous and feeds primitives directly into the
 *   GS via a dedicated PATH1 DMA channel.  Both units execute their own
 *   "microcode" programs uploaded by the EE – essentially tiny GPU shader
 *   programs running on VLIW-style hardware.
 *
 * GS (Graphics Synthesizer):
 *   Proprietary Sony chip running at ~147 MHz with an embedded 4 MB eDRAM
 *   frame buffer.  Entirely raster-based: no transform hardware; transforms are
 *   done by the VUs.  The GS receives primitive packets (points, lines, tris,
 *   sprites) over three parallel DMA paths (PATH1 from VU1, PATH2 from EE
 *   gifTag DMA, PATH3 from image DMA).  It performs textured rasterisation,
 *   alpha blending, fog, and z-buffering entirely in hardware.  Because its
 *   eDRAM is internal and the rendering pipeline is deeply parallel, accurately
 *   emulating it at the pixel level is extremely expensive.
 *
 * IOP (Input/Output Processor):
 *   A MIPS R3000A (the PS1 CPU) running at 36 MHz inside the PS2 for backwards
 *   compatibility and all I/O work (controller, memory card, CD/DVD, USB,
 *   FireWire).  The IOP runs its own OS (BIOS IOPs) and communicates with the
 *   EE over the SIF (Sub-system Interface) bus.
 *
 * SPU2 (Sound Processing Unit 2):
 *   48 hardware ADPCM channels with effects (reverb, chorus) running at
 *   48 000 Hz stereo output.  Connected to the IOP via DMA.
 *
 * Why emulation is hard:
 *   1. VU microcode: game-uploaded programs must be JIT-compiled or interpreted
 *      at high speed; incorrect cycle timing causes visual glitches.
 *   2. GS parallel rendering: PATH1/2/3 can transfer simultaneously; arbitration
 *      and ordering must match hardware exactly.
 *   3. EE 128-bit SIMD: many titles use MMI (Multimedia Instructions) heavily;
 *      every game-specific SIMD sequence needs correct emulation.
 *   4. Memory bandwidth: the GS eDRAM bus runs at ~48 GB/s internally; host
 *      DRAM emulation can bottleneck on modern CPUs.
 *   5. Timing-sensitive DMA: EE, VIF, GIF, SPU2 all run concurrently; games
 *      depend on precise inter-unit timing.
 *
 * Development Roadmap:
 *   Phase 1 – EE CPU: MIPS R5900 interpreter (GP + MMI SIMD + COP0/COP1/COP2
 *             stub), EE RAM (32 MB), scratchpad (16 KB), basic DMA controller.
 *   Phase 2 – GS: software rasteriser, GIF packet parser, PATH2 DMA, enough to
 *             render 2-D frames from simple homebrew.
 *   Phase 3 – VU0/VU1: microprogram interpreter; VIF1 unpackers; PATH1 feed.
 *   Phase 4 – IOP: R3000A interpreter, SIF, basic CDVD and pad drivers.
 *   Phase 5 – SPU2: ADPCM decoder, 48-channel mixer, reverb DSP.
 *   Phase 6 – Accuracy pass: HLE BIOS, timing fixes, game-specific hacks.
 */

#include "../src/core.h"
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

/* ── Core entry point – MUST be first bytes of binary ─────────────────── */
/* The frontend maps this binary as executable, then calls byte 0 as:
 *   void _core_start(u64 mapped_base, struct core_header *out)
 * This fills *out with offsets relative to mapped_base.
 */
__attribute__((section(".text._core_start")))
void _core_start(u64 base, struct core_header *out) {
    out->magic   = CORE_MAGIC;
    out->version = CORE_VERSION;
#define OFF(fn) (u32)((u64)(fn) - base)
    out->retro_init_off                   = OFF(core_retro_init);
    out->retro_deinit_off                 = OFF(core_retro_deinit);
    out->retro_set_environment_off        = OFF(core_retro_set_environment);
    out->retro_set_video_refresh_off      = OFF(core_retro_set_video_refresh);
    out->retro_set_audio_sample_off       = OFF(core_retro_set_audio_sample);
    out->retro_set_audio_sample_batch_off = OFF(core_retro_set_audio_sample_batch);
    out->retro_set_input_poll_off         = OFF(core_retro_set_input_poll);
    out->retro_set_input_state_off        = OFF(core_retro_set_input_state);
    out->retro_get_system_info_off        = OFF(core_retro_get_system_info);
    out->retro_get_system_av_info_off     = OFF(core_retro_get_system_av_info);
    out->retro_load_game_off              = OFF(core_retro_load_game);
    out->retro_unload_game_off            = OFF(core_retro_unload_game);
    out->retro_run_off                    = OFF(core_retro_run);
    out->retro_reset_off                  = OFF(core_retro_reset);
#undef OFF
}

/* ── Constants ─────────────────────────────────────────────────────────── */
#define PS2_W  640
#define PS2_H  448

/* ── Static state ──────────────────────────────────────────────────────── */
static const void *g_iso_data;
static u64         g_iso_size;
static int         g_loaded;

static retro_environment_t        g_env_cb;
static retro_video_refresh_t      g_video_cb;
static retro_audio_sample_t       g_audio_cb;
static retro_audio_sample_batch_t g_audio_batch_cb;
static retro_input_poll_t         g_input_poll_cb;
static retro_input_state_t        g_input_state_cb;

/* Frame buffer: 640×448 XRGB8888 – about 1.1 MB; fine as a static array. */
static u32 g_frame_buf[PS2_W * PS2_H];

/* Silence buffer: 48000 Hz / ~59.94 fps ≈ 801 stereo frames per video frame.
 * 802 samples × 2 channels = 1604 s16 values – use 1024 to keep it simple;
 * the batch callback is called multiple times if needed.               */
static s16 g_silence[1024 * 2];   /* pre-zeroed by .bss */

/* ── Minimal helpers (no libc) ─────────────────────────────────────────── */
static void mem_set_u32(u32 *p, u32 v, u32 n) {
    for (u32 i = 0; i < n; i++) p[i] = v;
}

/* ── 5×7 bitmap font (digits 0-9, A-Z, space, slash, colon, hyphen, dot) ── */
/* Each glyph is 5 columns × 7 rows, stored as 7 bytes (one per row,
 * bit 4 = leftmost pixel of that row).                                  */

#define FONT_W  5
#define FONT_H  7
#define FONT_CHARS 40   /* space(0), A-Z(1-26), 0-9(27-36), /(37), :(38), -(39) */

static const u8 FONT[FONT_CHARS][FONT_H] = {
    /* 0: space */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* 1: A */
    { 0x04, 0x0A, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    /* 2: B */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    /* 3: C */
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    /* 4: D */
    { 0x1E, 0x09, 0x09, 0x09, 0x09, 0x09, 0x1E },
    /* 5: E */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    /* 6: F */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    /* 7: G */
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
    /* 8: H */
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    /* 9: I */
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* 10: J */
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    /* 11: K */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    /* 12: L */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    /* 13: M */
    { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11 },
    /* 14: N */
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    /* 15: O */
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* 16: P */
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    /* 17: Q */
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    /* 18: R */
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    /* 19: S */
    { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E },
    /* 20: T */
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    /* 21: U */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* 22: V */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    /* 23: W */
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
    /* 24: X */
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    /* 25: Y */
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    /* 26: Z */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    /* 27: 0 */
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    /* 28: 1 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* 29: 2 */
    { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F },
    /* 30: 3 */
    { 0x1F, 0x02, 0x04, 0x06, 0x01, 0x11, 0x0E },
    /* 31: 4 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    /* 32: 5 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    /* 33: 6 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    /* 34: 7 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    /* 35: 8 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    /* 36: 9 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    /* 37: / */
    { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 },
    /* 38: : */
    { 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00 },
    /* 39: - */
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
};

/* Map an ASCII character to a FONT[] index. */
static int char_to_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32); /* to upper */
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == '/') return 37;
    if (c == ':') return 38;
    if (c == '-') return 39;
    return 0; /* space / unknown */
}

/* Compute pixel-width of a string at a given scale. */
static int str_pixel_width(const char *s, int scale) {
    int n = 0;
    while (*s++) n++;
    return n * (FONT_W + 1) * scale;
}

/* Draw a single glyph into g_frame_buf at pixel (px, py) with given scale
 * and colour.  Clips to frame buffer bounds.                             */
static void draw_glyph(int px, int py, int glyph_idx, int scale, u32 colour) {
    const u8 *glyph = FONT[glyph_idx];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x10 >> col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int x = px + col * scale + sx;
                        int y = py + row * scale + sy;
                        if (x >= 0 && x < PS2_W && y >= 0 && y < PS2_H)
                            g_frame_buf[y * PS2_W + x] = colour;
                    }
                }
            }
        }
    }
}

/* Draw a null-terminated ASCII string centred horizontally at y=py. */
static void draw_string_centred(const char *s, int py, int scale, u32 colour) {
    int total_w = str_pixel_width(s, scale);
    int px = (PS2_W - total_w) / 2;
    while (*s) {
        draw_glyph(px, py, char_to_glyph(*s), scale, colour);
        px += (FONT_W + 1) * scale;
        s++;
    }
}

/* ── Splash screen renderer ─────────────────────────────────────────────── */
static void render_splash(void) {
    /* Background: deep navy blue */
    mem_set_u32(g_frame_buf, 0xFF000833u, PS2_W * PS2_H);

    /* Horizontal divider lines for decoration */
    for (int x = 40; x < PS2_W - 40; x++) {
        g_frame_buf[140 * PS2_W + x] = 0xFF0033AAu;
        g_frame_buf[141 * PS2_W + x] = 0xFF0033AAu;
        g_frame_buf[296 * PS2_W + x] = 0xFF0033AAu;
        g_frame_buf[297 * PS2_W + x] = 0xFF0033AAu;
    }

    /* Line 1: "PlayStation 2" – large text (scale 3), bright white, centred */
    /* y centred around 190, glyph height = 7*3=21, top at 190-10=180 */
    draw_string_centred("PlayStation 2", 156, 3, 0xFFEEEEFFu);

    /* Line 2: "EmuC PS2 Core - Work in Progress" – scale 2, light blue */
    draw_string_centred("EmuC PS2 Core - Work in Progress", 226, 2, 0xFF88AAFFu);

    /* Line 3: "Full EE/VU emulation coming soon" – scale 1, dim grey */
    draw_string_centred("Full EE/VU emulation coming soon", 316, 1, 0xFF667788u);
}

/* ── libretro API implementation ───────────────────────────────────────── */

static void core_retro_init(void) {
    g_iso_data = (void *)0;
    g_iso_size = 0;
    g_loaded   = 0;
}

static void core_retro_deinit(void) {
    g_loaded = 0;
}

static void core_retro_set_environment(retro_environment_t cb) {
    g_env_cb = cb;
    if (!cb) return;
    int fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

static void core_retro_set_video_refresh(retro_video_refresh_t cb)           { g_video_cb = cb; }
static void core_retro_set_audio_sample(retro_audio_sample_t cb)             { g_audio_cb = cb; }
static void core_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio_batch_cb = cb; }
static void core_retro_set_input_poll(retro_input_poll_t cb)                 { g_input_poll_cb = cb; }
static void core_retro_set_input_state(retro_input_state_t cb)               { g_input_state_cb = cb; }

static void core_retro_get_system_info(struct retro_system_info *info) {
    info->library_name     = "EmuC PS2";
    info->library_version  = "0.1-stub";
    info->valid_extensions = "iso|bin|img";
    info->need_fullpath    = 0;
    info->block_extract    = 0;
}

static void core_retro_get_system_av_info(struct retro_system_av_info *info) {
    info->geometry.base_width   = PS2_W;
    info->geometry.base_height  = PS2_H;
    info->geometry.max_width    = PS2_W;
    info->geometry.max_height   = PS2_H;
    info->geometry.aspect_ratio = (float)PS2_W / (float)PS2_H; /* ~1.4286 */
    info->timing.fps            = 59.94;
    info->timing.sample_rate    = 48000.0;
}

static int core_retro_load_game(const struct retro_game_info *game) {
    if (!game) return 0;

    /* Store pointer to ISO data (may be NULL if need_fullpath were set). */
    g_iso_data = game->data;
    g_iso_size = game->size;

    /* Signal "PS2 COMING SOON" via the environment message if available.
     * RETRO_ENVIRONMENT_SET_MESSAGE (id=6) expects a pointer to:
     *   struct { const char *msg; u32 frames; }
     * We construct it inline here.                                      */
    struct {
        const char *msg;
        u32 frames;
    } retro_msg;
    retro_msg.msg    = "PS2 COMING SOON";
    retro_msg.frames = 180; /* ~3 seconds at 60 fps */
    if (g_env_cb)
        g_env_cb(6 /* RETRO_ENVIRONMENT_SET_MESSAGE */, &retro_msg);

    g_loaded = 1;
    return 1;
}

static void core_retro_unload_game(void) {
    g_iso_data = (void *)0;
    g_iso_size = 0;
    g_loaded   = 0;
}

static void core_retro_run(void) {
    /* Poll input (required by the libretro spec even if we ignore it). */
    if (g_input_poll_cb) g_input_poll_cb();

    /* Render the splash screen into the static frame buffer. */
    render_splash();

    /* Push video frame. */
    if (g_video_cb)
        g_video_cb(g_frame_buf, PS2_W, PS2_H, (u64)(PS2_W * 4));

    /* Output silence: 48000/59.94 ≈ 801 stereo frames; send 800 to be safe.
     * g_silence[] is a .bss zero-array so it is already all-zeros (silence). */
    if (g_audio_batch_cb)
        g_audio_batch_cb(g_silence, 800);
    else if (g_audio_cb) {
        for (int i = 0; i < 800; i++)
            g_audio_cb(0, 0);
    }
}

static void core_retro_reset(void) {
    /* Nothing to reset in the stub – splash will just keep rendering. */
    (void)g_loaded;
}
