/* snes_core.c - PS5 libretro SNES core (EmuC)
 * Self-contained SNES emulator: WDC 65816 CPU, SPC700 audio CPU,
 * S-DSP, PPU (modes 0-7), DMA/HDMA, LoROM/HiROM mapper.
 * No libc. Position-independent. _core_start at .text._core_start.
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

/* ── Core entry point ──────────────────────────────────────────────────── */
__attribute__((section(".text._core_start")))
void _core_start(u64 base, struct core_header *out) {
    out->magic   = CORE_MAGIC;
    out->version = CORE_VERSION;
#define OFF(fn) (u32)((u64)(fn) - base)
    out->retro_init_off                    = OFF(core_retro_init);
    out->retro_deinit_off                  = OFF(core_retro_deinit);
    out->retro_set_environment_off         = OFF(core_retro_set_environment);
    out->retro_set_video_refresh_off       = OFF(core_retro_set_video_refresh);
    out->retro_set_audio_sample_off        = OFF(core_retro_set_audio_sample);
    out->retro_set_audio_sample_batch_off  = OFF(core_retro_set_audio_sample_batch);
    out->retro_set_input_poll_off          = OFF(core_retro_set_input_poll);
    out->retro_set_input_state_off         = OFF(core_retro_set_input_state);
    out->retro_get_system_info_off         = OFF(core_retro_get_system_info);
    out->retro_get_system_av_info_off      = OFF(core_retro_get_system_av_info);
    out->retro_load_game_off               = OFF(core_retro_load_game);
    out->retro_unload_game_off             = OFF(core_retro_unload_game);
    out->retro_run_off                     = OFF(core_retro_run);
    out->retro_reset_off                   = OFF(core_retro_reset);
#undef OFF
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CONSTANTS & SIZES
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SNES_ROM_MAX    (8 * 1024 * 1024)
#define SNES_WRAM_SIZE  (128 * 1024)
#define SNES_VRAM_SIZE  (64 * 1024)
#define SNES_SRAM_MAX   (512 * 1024)
#define SNES_ARAM_SIZE  (64 * 1024)
#define SNES_SCREEN_W   256
#define SNES_SCREEN_H   224
#define SNES_FPS_NUM    60098
#define SNES_FPS_DEN    1000
#define SNES_SAMPLE_RATE 32000
/* Master clock ~21.477 MHz NTSC; CPU divides by 6 (fast) or 8 (slow) */
/* We track timing in master cycles */
#define SNES_MCLK_PER_DOT   4
#define SNES_DOTS_PER_LINE  341  /* 340 visible + HBlank */
#define SNES_LINES_PER_FRAME 262 /* NTSC */
#define SNES_VBLANK_START   225

/* ── Helpers (no libc) ────────────────────────────────────────────────── */
static void smem_set(u8 *p, u8 v, u32 n) { while(n--) *p++ = v; }
static void smem_cpy(u8 *d, const u8 *s, u32 n) { while(n--) *d++ = *s++; }
static s32 sabs(s32 x) { return x < 0 ? -x : x; }
static s32 sclamp(s32 v, s32 lo, s32 hi) { return v<lo?lo:v>hi?hi:v; }

/* ── u16/u32 helpers ──────────────────────────────────────────────────── */
static u16 read16le(const u8 *p) { return (u16)(p[0] | (p[1]<<8)); }
static u32 read24le(const u8 *p) { return (u32)(p[0] | (p[1]<<8) | (p[2]<<16)); }

/* ═══════════════════════════════════════════════════════════════════════════
 * STATE STRUCTURES
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── SPC700 (Sony audio CPU) ──────────────────────────────────────────── */
typedef struct {
    u8  a, x, y, sp;
    u16 pc;
    /* PSW bits: N V P B H I Z C */
    u8  psw;
    u8  ram[SNES_ARAM_SIZE];
    /* DSP registers (128 bytes) */
    u8  dsp_regs[128];
    u8  dsp_addr;        /* selected DSP register */
    /* Communication ports CPU<->SPC700 */
    u8  port_in[4];      /* written by SNES CPU, read by SPC */
    u8  port_out[4];     /* written by SPC, read by SNES CPU */
    /* Timers */
    u8  timer_en;        /* bits 0-2 */
    u8  timer_target[3];
    u8  timer_counter[3];/* internal divider */
    u8  timer_out[3];    /* 4-bit output counters */
    u32 timer_cycle[3];  /* cycle accumulator */
    u32 cycles;
    /* Boot ROM enabled */
    u8  boot_rom_en;
} SPC700;

/* ── DSP voice ─────────────────────────────────────────────────────────── */
typedef struct {
    s32  env_level;      /* current envelope level 0..0x7FF */
    u8   env_state;      /* 0=attack 1=decay 2=sustain 3=release */
    u16  pitch;          /* 14-bit pitch counter increment */
    u16  pitch_frac;     /* fractional part */
    u32  brr_addr;       /* current BRR block address in ARAM */
    u8   brr_header;
    u8   brr_pos;        /* sample offset within block 0..15 */
    s16  buf[16];        /* decoded sample ring buffer */
    u8   buf_pos;
    s16  prev1, prev2;   /* BRR filter history */
    s16  out_l, out_r;
    u8   key_on_delay;
} DSPVoice;

/* ── S-DSP ────────────────────────────────────────────────────────────── */
typedef struct {
    DSPVoice voice[8];
    s32  echo_buf[0x8000]; /* echo ring buffer (stereo interleaved) */
    u32  echo_pos;
    u32  echo_len;
    u32  sample_counter;   /* for 32 kHz output rate */
    s16  audio_buf[2048];  /* output buffer */
    u32  audio_write;
    u32  audio_read;
} SDSP;

/* ── PPU registers (internal) ─────────────────────────────────────────── */
typedef struct {
    /* VRAM */
    u8   vram[SNES_VRAM_SIZE];
    u16  vram_addr;
    u8   vram_inc_high;  /* 0=inc after low, 1=inc after high */
    u8   vram_inc_step;  /* 1, 32, or 128 words */
    u8   vram_remap;     /* address remapping mode */
    u8   vram_prefetch_lo, vram_prefetch_hi;

    /* CGRAM (512 bytes = 256 * 15bpp palette entries) */
    u16  cgram[256];
    u8   cgram_addr;
    u8   cgram_lo;       /* low byte latch */
    u8   cgram_flip;

    /* OAM (544 bytes: 512 main + 32 extended) */
    u8   oam[544];
    u16  oam_addr;       /* 10-bit */
    u8   oam_priority;   /* OAM rotation */
    u8   oam_flip;
    u8   oam_latch;

    /* BG control */
    u8   bg_mode;        /* BGMODE register (bits 0-2) */
    u8   bg3_priority;   /* BG3 high priority in mode 1 */
    u8   mosaic_size;
    u8   mosaic_en;

    /* BG scrolling (for 4 BGs) */
    u16  bg_hofs[4];
    u16  bg_vofs[4];
    u8   bg_hofs_latch[4];  /* write latch */
    u8   bg_prev_latch;     /* M7SEL / BG scroll write latch */

    /* BG tilebase / character base */
    u16  bg_tilemap_addr[4]; /* in VRAM words */
    u8   bg_tilemap_w[4];    /* 0=32 1=64 columns */
    u8   bg_tilemap_h[4];    /* 0=32 1=64 rows */
    u16  bg_chrbase[4];      /* character base in VRAM words */
    u8   bg_bpp[4];          /* bits per pixel for each BG */

    /* Mode 7 */
    s16  m7a, m7b, m7c, m7d;
    s16  m7x, m7y;
    u8   m7sel;
    u8   m7_latch;
    s16  m7_prev;

    /* Color math */
    u8   cgwsel;    /* color window select */
    u8   cgadsub;   /* add/subtract select */
    u8   coldata;   /* fixed color */
    u8   fixed_r, fixed_g, fixed_b;

    /* Window */
    u8   w1l, w1r, w2l, w2r; /* window positions */
    u8   wbglog[2];   /* window BG logic (OR/AND/XOR/XNOR) W1234LOG */
    u8   wobjlog;
    u8   w1sel_bg, w2sel_bg, w1sel_obj;
    u8   tmain, tsub;    /* main/sub screen designations */
    u8   w_tmain, w_tsub; /* window masks for main/sub */

    /* Display control */
    u8   inidisp;    /* initial display (force blank / brightness) */
    u8   brightness; /* 0-15 */
    u8   force_blank;

    /* Sprite control */
    u8   obj_base;   /* OAM tile base (bits 14:13) */
    u8   obj_namesel;/* name selection */
    u8   obj_size;   /* object size bits */

    /* HDMA */
    u8   hdma_en;

    /* Scanline counter */
    s32  scanline;
    s32  dot;
    u8   field;      /* 0/1 interlace */

    /* Latch */
    u16  ophct, opvct;
    u8   stat78;

    /* Output framebuffer XRGB8888 */
    u32  framebuf[SNES_SCREEN_W * 240];

    /* Line buffers */
    u16  main_buf[SNES_SCREEN_W];  /* BGR555 */
    u16  sub_buf[SNES_SCREEN_W];
    u8   main_pri[SNES_SCREEN_W];
    u8   sub_pri[SNES_SCREEN_W];
    u8   main_src[SNES_SCREEN_W];  /* source layer for color math */
} PPU;

/* ── DMA channel ──────────────────────────────────────────────────────── */
typedef struct {
    u8  params;      /* DMA parameters byte */
    u8  dst_reg;     /* B-bus register (0x21xx) */
    u32 src_addr;    /* A-bus source (bank<<16 | addr) */
    u16 size;        /* transfer size in bytes */
    u8  hdma_table_bank;
    u16 hdma_table_addr;
    u8  hdma_line_counter;
    u8  hdma_do_transfer;
    u32 hdma_indirect_addr;
    u8  hdma_completed;
} DMAChannel;

/* ── CPU (WDC 65816) ──────────────────────────────────────────────────── */
typedef struct {
    /* Registers */
    u16 a;   /* accumulator (8 or 16-bit based on M flag) */
    u16 x;   /* X index */
    u16 y;   /* Y index */
    u16 sp;  /* stack pointer */
    u16 pc;  /* program counter */
    u8  pbr; /* program bank register */
    u8  dbr; /* data bank register */
    u16 d;   /* direct page register */

    /* Status flags (P register) */
    u8  n, v, m, xf, df, i, z, c;
    /* Emulation mode flag (E) */
    u8  e;

    /* IRQ/NMI pending */
    u8  nmi_pending;
    u8  irq_pending;
    u8  wai;  /* waiting for interrupt (WAI) */
    u8  stp;  /* stopped (STP) */

    /* Open bus */
    u8  open_bus;

    /* Cycle counter */
    u32 cycles;
    u32 master_cycles;

    /* IRQ sources */
    u8  nmi_occurred;
    u8  irq_hv;
    u8  htime;    /* H-count for IRQ */
    u16 hcount;
    u16 vcount;
    u8  nmitimen; /* NMI/IRQ enable */
    u8  memsel;   /* FastROM select */

    /* Multiplication / division */
    u16 wrmpya, wrmpyb;
    u32 rdmpy;
    u16 wrdivl, wrdivh;
    u16 rddivl;

    /* Joypad auto-read */
    u16 joy_latch[2];  /* latched joypad state */
    u16 joy_pos[2];    /* serial read position */
    u8  joy_strobe;
    u16 joy_data[2];   /* final read registers */

    /* VBlank flag */
    u8  in_vblank;
    u8  nmi_enabled;

    /* H/V IRQ mode: 0=none 1=H 2=V 3=HV */
    u8  hv_irq_mode;
} CPU816;

/* ── Full SNES state ──────────────────────────────────────────────────── */
typedef struct {
    CPU816 cpu;
    SPC700 spc;
    SDSP   dsp;
    PPU    ppu;
    DMAChannel dma[8];

    /* Memory */
    u8  wram[SNES_WRAM_SIZE];
    u8  rom[SNES_ROM_MAX];
    u8  sram[SNES_SRAM_MAX];
    u32 rom_size;
    u32 sram_size;

    /* Cart type */
    u8  is_hirom;
    u8  has_sram;
    u8  fast_rom;

    /* WRAM access port */
    u32 wmadd;  /* 24-bit */

    /* Misc I/O */
    u8  dma_en;   /* MDMAEN */
    u8  hdma_en;  /* HDMAEN */

    /* Frame timing */
    u32 master_clock;
    u32 frame_count;

    /* Audio output */
    s16 audio_out[2048];
    u32 audio_frames;

    int loaded;
} SNES;

/* ═══════════════════════════════════════════════════════════════════════════
 * GLOBAL STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
static SNES g_snes;

static retro_environment_t        g_env_cb;
static retro_video_refresh_t      g_video_cb;
static retro_audio_sample_t       g_audio_cb;
static retro_audio_sample_batch_t g_audio_batch_cb;
static retro_input_poll_t         g_input_poll_cb;
static retro_input_state_t        g_input_state_cb;

/* ═══════════════════════════════════════════════════════════════════════════
 * SPC700 BOOT ROM (standard IPL)
 * ═══════════════════════════════════════════════════════════════════════════ */
static const u8 SPC_BOOT_ROM[64] = {
    0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
    0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
    0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
    0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF
};

/* ═══════════════════════════════════════════════════════════════════════════
 * MEMORY BUS
 * ═══════════════════════════════════════════════════════════════════════════ */
/* forward */
static u8 ppu_read(SNES *s, u8 reg);
static void ppu_write(SNES *s, u8 reg, u8 val);
static u8 dma_read(SNES *s, u8 ch, u8 reg);
static void dma_write(SNES *s, u8 ch, u8 reg, u8 val);
static void dma_run(SNES *s, u8 channels);
static void hdma_init(SNES *s);
static void hdma_run(SNES *s);
static u8 spc_port_read(SNES *s, u8 port);
static void spc_port_write(SNES *s, u8 port, u8 val);
static void ppu_run_scanline(SNES *s);

static u32 lorom_addr(SNES *s, u32 addr) {
    u8  bank = (addr >> 16) & 0xFF;
    u16 off  = addr & 0xFFFF;
    /* Map bank to ROM */
    u32 rom_bank = (bank & 0x7F);
    if (rom_bank >= 0x40) rom_bank -= 0x40; /* mirrors 80-BF -> 00-3F */
    u32 rom_off  = (u32)rom_bank * 0x8000 + (off - 0x8000);
    if (rom_off >= s->rom_size) rom_off %= (s->rom_size ? s->rom_size : 1);
    return rom_off;
}

static u32 hirom_addr(SNES *s, u32 addr) {
    u8  bank = (addr >> 16) & 0xFF;
    u16 off  = addr & 0xFFFF;
    u32 rom_bank;
    if (bank >= 0xC0) rom_bank = bank - 0xC0;
    else if (bank >= 0x80) rom_bank = bank - 0x80;
    else if (bank >= 0x40) rom_bank = bank - 0x40;
    else rom_bank = bank;
    u32 rom_off = (u32)rom_bank * 0x10000 + off;
    if (rom_off >= s->rom_size) rom_off %= (s->rom_size ? s->rom_size : 1);
    return rom_off;
}

static u8 cpu_read(SNES *s, u32 addr) {
    u8  bank = (addr >> 16) & 0xFF;
    u16 off  = addr & 0xFFFF;

    /* WRAM: banks 7E-7F */
    if (bank == 0x7E || bank == 0x7F)
        return s->wram[((u32)(bank - 0x7E) << 16) | off];

    /* System area: bank 00-3F and 80-BF, address 0000-7FFF */
    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && off < 0x8000) {
        /* WRAM mirror at 0000-1FFF */
        if (off < 0x2000)
            return s->wram[off];

        /* I/O: 2100-21FF PPU, SPC700 */
        if (off >= 0x2100 && off <= 0x21FF) {
            u8 r = off & 0xFF;
            /* SPC700 ports */
            if (r >= 0x40 && r <= 0x43)
                return spc_port_read(s, r - 0x40);
            return ppu_read(s, r);
        }

        /* CPU registers 4200-42FF */
        if (off >= 0x4200 && off <= 0x42FF) {
            CPU816 *c = &s->cpu;
            switch(off) {
                case 0x4210: { u8 v = (c->nmi_occurred ? 0x80 : 0) | 0x02; c->nmi_occurred = 0; s->cpu.open_bus = v; return v; }
                case 0x4211: { u8 v = (c->irq_hv ? 0x80 : 0); c->irq_hv = 0; return v; }
                case 0x4212: { u8 v = 0; if (s->ppu.scanline >= SNES_VBLANK_START) v |= 0x80; if (s->ppu.dot < 4 || s->ppu.dot >= 278) v |= 0x40; v |= 0x01; /* auto joypad ready */ return v; }
                case 0x4214: return (u8)(c->rddivl & 0xFF);
                case 0x4215: return (u8)(c->rddivl >> 8);
                case 0x4216: return (u8)(c->rdmpy & 0xFF);
                case 0x4217: return (u8)((c->rdmpy >> 8) & 0xFF);
                case 0x4218: return (u8)(c->joy_data[0] & 0xFF);
                case 0x4219: return (u8)(c->joy_data[0] >> 8);
                case 0x421A: return (u8)(c->joy_data[1] & 0xFF);
                case 0x421B: return (u8)(c->joy_data[1] >> 8);
                default: return c->open_bus;
            }
        }

        /* DMA: 4300-43FF */
        if (off >= 0x4300 && off <= 0x43FF) {
            u8 ch  = (off >> 4) & 0x7;
            u8 reg = off & 0xF;
            return dma_read(s, ch, reg);
        }

        /* SRAM: banks 70-7D (LoROM), offsets 0000-7FFF */
        if (s->is_hirom == 0 && bank >= 0x70 && bank <= 0x7D && s->has_sram) {
            u32 sa = (u32)(bank - 0x70) * 0x8000 + off;
            if (sa < s->sram_size) return s->sram[sa];
        }

        return s->cpu.open_bus;
    }

    /* ROM reads */
    if (!s->is_hirom) {
        /* LoROM: 8000-FFFF in banks 00-7D and 80-FF */
        if (off >= 0x8000) {
            u32 ro = lorom_addr(s, addr);
            u8 v = s->rom[ro];
            s->cpu.open_bus = v;
            return v;
        }
        /* SRAM banks 70-7D upper (already handled above for low half) */
        if (bank >= 0x70 && bank <= 0x7D && s->has_sram) {
            u32 sa = (u32)(bank - 0x70) * 0x8000 + (off & 0x7FFF);
            if (sa < s->sram_size) return s->sram[sa];
        }
        return s->cpu.open_bus;
    } else {
        /* HiROM: banks 40-7D and C0-FF full 64KB */
        if (bank >= 0x40) {
            u32 ro = hirom_addr(s, addr);
            u8 v = s->rom[ro];
            s->cpu.open_bus = v;
            return v;
        }
        /* HiROM lower banks (00-3F) upper half */
        if (off >= 0x8000) {
            u32 ro = hirom_addr(s, addr);
            u8 v = s->rom[ro];
            s->cpu.open_bus = v;
            return v;
        }
        /* SRAM at 20-3F:6000-7FFF */
        if (bank >= 0x20 && bank <= 0x3F && off >= 0x6000 && off <= 0x7FFF && s->has_sram) {
            u32 sa = (u32)(bank - 0x20) * 0x2000 + (off - 0x6000);
            if (sa < s->sram_size) return s->sram[sa];
        }
        return s->cpu.open_bus;
    }
}

static void cpu_write(SNES *s, u32 addr, u8 val) {
    u8  bank = (addr >> 16) & 0xFF;
    u16 off  = addr & 0xFFFF;
    CPU816 *c = &s->cpu;

    /* WRAM */
    if (bank == 0x7E || bank == 0x7F) {
        s->wram[((u32)(bank - 0x7E) << 16) | off] = val;
        return;
    }

    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && off < 0x8000) {
        if (off < 0x2000) { s->wram[off] = val; return; }

        if (off >= 0x2100 && off <= 0x21FF) {
            u8 r = off & 0xFF;
            if (r >= 0x40 && r <= 0x43) { spc_port_write(s, r - 0x40, val); return; }
            ppu_write(s, r, val);
            return;
        }

        if (off >= 0x4200 && off <= 0x42FF) {
            switch(off) {
                case 0x4200: c->nmitimen = val;
                    c->nmi_enabled  = (val >> 7) & 1;
                    c->hv_irq_mode  = (val >> 4) & 3;
                    break;
                case 0x4201: /* WRIO - joypad output (not emulated deeply) */ break;
                case 0x4202: c->wrmpya = val; break;
                case 0x4203: {
                    c->wrmpyb = val;
                    c->rdmpy  = (u32)c->wrmpya * (u32)c->wrmpyb;
                    break;
                }
                case 0x4204: c->wrdivl = (c->wrdivl & 0xFF00) | val; break;
                case 0x4205: c->wrdivl = (c->wrdivl & 0x00FF) | ((u16)val << 8); break;
                case 0x4206: {
                    c->wrdivh = val;
                    if (val == 0) { c->rddivl = 0xFFFF; c->rdmpy = c->wrdivl; }
                    else { c->rddivl = c->wrdivl / val; c->rdmpy = c->wrdivl % val; }
                    break;
                }
                case 0x4207: c->hcount = (c->hcount & 0x100) | val; break;
                case 0x4208: c->hcount = (c->hcount & 0x0FF) | ((u16)(val & 1) << 8); break;
                case 0x4209: c->vcount = (c->vcount & 0x100) | val; break;
                case 0x420A: c->vcount = (c->vcount & 0x0FF) | ((u16)(val & 1) << 8); break;
                case 0x420B: dma_run(s, val); break;
                case 0x420C: s->hdma_en = val; break;
                case 0x420D: c->memsel = val & 1; break;
                case 0x4214: case 0x4215: case 0x4216: case 0x4217: break;
                /* WMADD */
                case 0x4380: s->wmadd = (s->wmadd & 0xFFFF00) | val; break;
                case 0x4381: s->wmadd = (s->wmadd & 0xFF00FF) | ((u32)val << 8); break;
                case 0x4382: s->wmadd = (s->wmadd & 0x00FFFF) | ((u32)(val & 1) << 16); break;
            }
            return;
        }

        /* WMDATA at 0x2180 */
        if (off == 0x2180) {
            s->wram[s->wmadd & 0x1FFFF] = val;
            s->wmadd = (s->wmadd + 1) & 0x1FFFF;
            return;
        }

        if (off >= 0x4300 && off <= 0x43FF) {
            u8 ch  = (off >> 4) & 0x7;
            u8 reg = off & 0xF;
            dma_write(s, ch, reg, val);
            return;
        }

        /* SRAM LoROM 70-7D */
        if (!s->is_hirom && bank >= 0x70 && bank <= 0x7D && s->has_sram) {
            u32 sa = (u32)(bank - 0x70) * 0x8000 + off;
            if (sa < s->sram_size) s->sram[sa] = val;
        }
        return;
    }

    /* SRAM HiROM 20-3F:6000-7FFF */
    if (s->is_hirom && bank >= 0x20 && bank <= 0x3F && off >= 0x6000 && off <= 0x7FFF && s->has_sram) {
        u32 sa = (u32)(bank - 0x20) * 0x2000 + (off - 0x6000);
        if (sa < s->sram_size) s->sram[sa] = val;
    }
    /* ROM writes ignored */
}

/* 16-bit and 24-bit reads */
static u16 cpu_read16(SNES *s, u32 addr) {
    return (u16)(cpu_read(s, addr) | ((u16)cpu_read(s, addr + 1) << 8));
}

static u32 cpu_read24(SNES *s, u32 addr) {
    return (u32)cpu_read(s, addr) | ((u32)cpu_read(s, addr+1) << 8) | ((u32)cpu_read(s, addr+2) << 16);
}

static void cpu_write16(SNES *s, u32 addr, u16 val) {
    cpu_write(s, addr, (u8)val);
    cpu_write(s, addr + 1, (u8)(val >> 8));
}

/* Stack helpers */
static void cpu_push8(SNES *s, u8 v) {
    CPU816 *c = &s->cpu;
    cpu_write(s, 0x000000 | c->sp, v);
    if (c->e) c->sp = (c->sp & 0xFF00) | (u16)((c->sp - 1) & 0xFF);
    else c->sp--;
}
static void cpu_push16(SNES *s, u16 v) {
    cpu_push8(s, (u8)(v >> 8));
    cpu_push8(s, (u8)v);
}
static u8 cpu_pop8(SNES *s) {
    CPU816 *c = &s->cpu;
    if (c->e) c->sp = (c->sp & 0xFF00) | (u16)((c->sp + 1) & 0xFF);
    else c->sp++;
    return cpu_read(s, 0x000000 | c->sp);
}
static u16 cpu_pop16(SNES *s) {
    u8 lo = cpu_pop8(s);
    u8 hi = cpu_pop8(s);
    return (u16)(lo | ((u16)hi << 8));
}

/* Status register helpers */
static u8 cpu_get_p(CPU816 *c) {
    return (u8)((c->n << 7) | (c->v << 6) | (c->m << 5) | (c->xf << 4) |
                (c->df << 3) | (c->i << 2) | (c->z << 1) | c->c);
}
static void cpu_set_p(CPU816 *c, u8 p) {
    c->n  = (p >> 7) & 1;
    c->v  = (p >> 6) & 1;
    c->m  = (p >> 5) & 1;
    c->xf = (p >> 4) & 1;
    c->df = (p >> 3) & 1;
    c->i  = (p >> 2) & 1;
    c->z  = (p >> 1) & 1;
    c->c  =  p       & 1;
    if (c->xf) { c->x &= 0xFF; c->y &= 0xFF; }
}
/* Set NZ flags for 8-bit result */
static void cpu_nz8(CPU816 *c, u8 v)  { c->n = v >> 7; c->z = (v == 0); }
/* Set NZ flags for 16-bit result */
static void cpu_nz16(CPU816 *c, u16 v){ c->n = v >> 15; c->z = (v == 0); }

/* ═══════════════════════════════════════════════════════════════════════════
 * WDC 65816 CPU – ADDRESS MODES & INSTRUCTION EXECUTION
 * ═══════════════════════════════════════════════════════════════════════════ */

/* All effective address calculations return a 32-bit "long address"
 * (bank<<16 | addr16).  For stack-relative, direct page etc. we use bank 0. */

/* Fetch byte at PBR:PC and advance PC */
static u8 cpu_fetch(SNES *s) {
    CPU816 *c = &s->cpu;
    u8 v = cpu_read(s, ((u32)c->pbr << 16) | c->pc);
    c->pc++;
    c->cycles++;
    return v;
}
static u16 cpu_fetch16(SNES *s) {
    u8 lo = cpu_fetch(s);
    u8 hi = cpu_fetch(s);
    return (u16)(lo | ((u16)hi << 8));
}
static u32 cpu_fetch24(SNES *s) {
    u8 b0 = cpu_fetch(s); u8 b1 = cpu_fetch(s); u8 b2 = cpu_fetch(s);
    return (u32)(b0 | ((u32)b1<<8) | ((u32)b2<<16));
}

/* Operand byte from A accumulator (low) */
static u8  cpu_A8(CPU816 *c)  { return (u8)(c->a & 0xFF); }
static u16 cpu_A16(CPU816 *c) { return c->a; }

/* ── Addressing modes (return effective address) ──────────────────────── */
/* imm8/imm16: return 0 (caller uses fetch directly) */

static u32 ea_dp(SNES *s) {          /* Direct Page */
    CPU816 *c = &s->cpu;
    u8 dp_off = cpu_fetch(s);
    if ((c->d & 0xFF) != 0) c->cycles++; /* extra cycle if DL != 0 */
    return (u32)((c->d + dp_off) & 0xFFFF);
}
static u32 ea_dpx(SNES *s) {         /* Direct Page, X */
    CPU816 *c = &s->cpu;
    u8 dp_off = cpu_fetch(s);
    if ((c->d & 0xFF) != 0) c->cycles++;
    u16 eff = c->e ? (u16)((c->d & 0xFF00) | ((c->d + dp_off + c->x) & 0xFF))
                   : (u16)(c->d + dp_off + c->x);
    return (u32)eff;
}
static u32 ea_dpy(SNES *s) {         /* Direct Page, Y */
    CPU816 *c = &s->cpu;
    u8 dp_off = cpu_fetch(s);
    if ((c->d & 0xFF) != 0) c->cycles++;
    u16 eff = c->e ? (u16)((c->d & 0xFF00) | ((c->d + dp_off + c->y) & 0xFF))
                   : (u16)(c->d + dp_off + c->y);
    return (u32)eff;
}
static u32 ea_abs(SNES *s) {         /* Absolute (DBR bank) */
    CPU816 *c = &s->cpu;
    u16 a = cpu_fetch16(s);
    return ((u32)c->dbr << 16) | a;
}
static u32 ea_absx(SNES *s) {        /* Absolute, X */
    CPU816 *c = &s->cpu;
    u16 base = cpu_fetch16(s);
    u16 eff  = base + c->x;
    if ((base >> 8) != (eff >> 8)) c->cycles++; /* page cross */
    return ((u32)c->dbr << 16) | eff;
}
static u32 ea_absy(SNES *s) {        /* Absolute, Y */
    CPU816 *c = &s->cpu;
    u16 base = cpu_fetch16(s);
    u16 eff  = base + c->y;
    if ((base >> 8) != (eff >> 8)) c->cycles++;
    return ((u32)c->dbr << 16) | eff;
}
static u32 ea_long(SNES *s) {        /* Absolute Long */
    u32 a = cpu_fetch24(s);
    return a;
}
static u32 ea_longx(SNES *s) {       /* Absolute Long, X */
    CPU816 *c = &s->cpu;
    u32 a = cpu_fetch24(s);
    return (a + c->x) & 0xFFFFFF;
}
static u32 ea_ind_dp(SNES *s) {      /* (dp) */
    CPU816 *c = &s->cpu;
    u32 ptr = ea_dp(s);
    return ((u32)c->dbr << 16) | cpu_read16(s, ptr);
}
static u32 ea_ind_dpx(SNES *s) {     /* (dp,X) */
    CPU816 *c = &s->cpu;
    u32 ptr = ea_dpx(s);
    return ((u32)c->dbr << 16) | cpu_read16(s, ptr);
}
static u32 ea_ind_dpy(SNES *s) {     /* (dp),Y */
    CPU816 *c = &s->cpu;
    u8  dp_off = cpu_fetch(s); (void)dp_off;
    /* recalculate ptr without X/Y */
    u16 dp_base = (u16)(c->d + (u8)(*(s->rom))); /* dummy; we do it right: */
    /* Redo properly */
    u8  off8 = *(u8*)(s->rom); /* not right; must re-derive from fetched byte */
    (void)dp_base; (void)off8;
    /* The fetch already happened above; reconstruct */
    /* Actually fetch is already done; we need to inline this more carefully.
     * Let's just implement it inline using the last fetched byte.
     * We can't easily un-fetch. So rewrite this properly: */
    return 0; /* placeholder; see inline opcode dispatch below */
}

/* We'll implement the indirect-indexed addressing inline in the opcode
 * dispatcher rather than via these helper functions to avoid re-fetch issues.
 * Define a macro for the common patterns instead. */

static u32 ea_sr(SNES *s) {          /* Stack Relative */
    CPU816 *c = &s->cpu;
    u8 off = cpu_fetch(s);
    return (u32)((c->sp + off) & 0xFFFF);
}
static u32 ea_sry(SNES *s) {         /* (Stack Relative),Y - returns pointer */
    CPU816 *c = &s->cpu;
    u8 off = cpu_fetch(s);
    u16 ptr_addr = (u16)(c->sp + off);
    u16 ptr_val  = cpu_read16(s, (u32)ptr_addr);
    return ((u32)c->dbr << 16) | (u16)(ptr_val + c->y);
}
static u32 ea_ind_long_dp(SNES *s) { /* [dp] */
    u32 ptr = ea_dp(s);
    return cpu_read24(s, ptr);
}
static u32 ea_ind_long_dpy(SNES *s) { /* [dp],Y */
    CPU816 *c = &s->cpu;
    u32 base = cpu_read24(s, ea_dp(s));
    return (base + c->y) & 0xFFFFFF;
}
static u32 ea_ind_abs(SNES *s) {     /* (abs) for JMP */
    u16 a = cpu_fetch16(s);
    return ((u32)0 << 16) | cpu_read16(s, (u32)a);
}
static u32 ea_ind_absx(SNES *s) {    /* (abs,X) for JMP/JSR */
    CPU816 *c = &s->cpu;
    u16 a = cpu_fetch16(s);
    return ((u32)c->pbr << 16) | cpu_read16(s, ((u32)c->pbr << 16) | (u16)(a + c->x));
}

/* ── Read/write via effective address ────────────────────────────────── */
static u8  ea_read8(SNES *s, u32 ea)  { return cpu_read(s, ea); }
static u16 ea_read16(SNES *s, u32 ea) { return cpu_read16(s, ea); }
static void ea_write8(SNES *s, u32 ea, u8 v)  { cpu_write(s, ea, v); }
static void ea_write16(SNES *s, u32 ea, u16 v){ cpu_write16(s, ea, v); }

/* ── ALU helpers ──────────────────────────────────────────────────────── */
/* ADC 8-bit */
static u8 alu_adc8(CPU816 *c, u8 a, u8 b) {
    if (c->df) { /* BCD */
        u32 lo = (a & 0x0F) + (b & 0x0F) + c->c;
        if (lo > 9) lo += 6;
        u32 hi = (a >> 4) + (b >> 4) + (lo >> 4);
        if (hi > 9) hi += 6;
        c->c = (hi > 15) ? 1 : 0;
        u8 r = (u8)((hi << 4) | (lo & 0xF));
        c->v = 0; c->n = r >> 7; c->z = (r == 0);
        return r;
    }
    u32 r = (u32)a + b + c->c;
    c->v = (~(a ^ b) & (a ^ (u8)r) & 0x80) ? 1 : 0;
    c->c = (r > 0xFF) ? 1 : 0;
    cpu_nz8(c, (u8)r);
    return (u8)r;
}
/* ADC 16-bit */
static u16 alu_adc16(CPU816 *c, u16 a, u16 b) {
    if (c->df) {
        u32 lo = (a & 0x000F) + (b & 0x000F) + c->c; if(lo>9)lo+=6;
        u32 m0 = (a & 0x00F0) + (b & 0x00F0) + (lo & 0xF0); if(m0>0x90)m0+=0x60;
        u32 m1 = (a & 0x0F00) + (b & 0x0F00) + (m0 & 0xF00); if(m1>0x900)m1+=0x600;
        u32 hi = (a & 0xF000) + (b & 0xF000) + (m1 & 0xF000); if(hi>0x9000)hi+=0x6000;
        c->c = (hi > 0xFFFF) ? 1 : 0;
        u16 r = (u16)((hi & 0xF000)|(m1 & 0x0F00)|(m0 & 0x00F0)|(lo & 0x000F));
        c->v = 0; cpu_nz16(c, r); return r;
    }
    u32 r = (u32)a + b + c->c;
    c->v = (~(a ^ b) & (a ^ (u16)r) & 0x8000) ? 1 : 0;
    c->c = (r > 0xFFFF) ? 1 : 0;
    cpu_nz16(c, (u16)r);
    return (u16)r;
}
/* SBC 8-bit */
static u8 alu_sbc8(CPU816 *c, u8 a, u8 b) {
    if (c->df) {
        s32 lo = (a & 0xF) - (b & 0xF) - (1 - c->c);
        if (lo < 0) lo = ((lo - 6) & 0xF) - 0x10;
        s32 hi = (a >> 4) - (b >> 4) + (lo >> 4);
        if (hi < 0) hi = ((hi - 6) & 0xF) - 0x10;
        c->c = (hi >= 0) ? 1 : 0;
        u8 r = (u8)((hi << 4) | (lo & 0xF));
        c->v = 0; c->n = r >> 7; c->z = (r == 0);
        return r;
    }
    u32 r = (u32)a - b - (1 - c->c);
    c->v = ((a ^ b) & (a ^ (u8)r) & 0x80) ? 1 : 0;
    c->c = (r <= 0xFF) ? 1 : 0;
    cpu_nz8(c, (u8)r);
    return (u8)r;
}
/* SBC 16-bit */
static u16 alu_sbc16(CPU816 *c, u16 a, u16 b) {
    if (c->df) {
        /* simplified BCD subtract */
        u32 r = (u32)a - b - (1 - c->c);
        c->c = (r <= 0xFFFF) ? 1 : 0;
        cpu_nz16(c, (u16)r); c->v = 0;
        return (u16)r;
    }
    u32 r = (u32)a - b - (1 - c->c);
    c->v = ((a ^ b) & (a ^ (u16)r) & 0x8000) ? 1 : 0;
    c->c = (r <= 0xFFFF) ? 1 : 0;
    cpu_nz16(c, (u16)r);
    return (u16)r;
}
/* CMP 8-bit (sets flags, discards result) */
static void alu_cmp8(CPU816 *c, u8 a, u8 b) {
    u32 r = (u32)a - b; c->c = (r <= 0xFF) ? 1 : 0; cpu_nz8(c, (u8)r);
}
static void alu_cmp16(CPU816 *c, u16 a, u16 b) {
    u32 r = (u32)a - b; c->c = (r <= 0xFFFF) ? 1 : 0; cpu_nz16(c, (u16)r);
}
/* BIT 8-bit */
static void alu_bit8(CPU816 *c, u8 a, u8 b) {
    c->n = b >> 7; c->v = (b >> 6) & 1; c->z = ((a & b) == 0);
}
static void alu_bit16(CPU816 *c, u16 a, u16 b) {
    c->n = b >> 15; c->v = (b >> 14) & 1; c->z = ((a & b) == 0);
}
