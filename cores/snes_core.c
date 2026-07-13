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


/* ═══════════════════════════════════════════════════════════════════════════
 * SPC700 PORT I/O
 * ═══════════════════════════════════════════════════════════════════════════ */
static u8 spc_port_read(SNES *s, u8 port) {
    return s->spc.port_out[port & 3];
}
static void spc_port_write(SNES *s, u8 port, u8 val) {
    s->spc.port_in[port & 3] = val;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PPU READ / WRITE
 * ═══════════════════════════════════════════════════════════════════════════ */
static u8 ppu_read(SNES *s, u8 reg) {
    PPU *p = &s->ppu;
    switch (reg) {
    case 0x34: return (u8)((u32)p->m7a * (u32)((s8)p->m7b));
    case 0x35: return (u8)(((u32)p->m7a * (u32)((s8)p->m7b)) >> 8);
    case 0x36: return (u8)(((u32)p->m7a * (u32)((s8)p->m7b)) >> 16);
    case 0x38: { /* RDOAM */
        u8 v = p->oam[p->oam_addr & 0x21F];
        p->oam_addr = (p->oam_addr + 1) & 0x21F;
        return v;
    }
    case 0x39: { /* RDVRAML */
        u8 v = p->vram_prefetch_lo;
        if (!p->vram_inc_high) {
            p->vram_prefetch_lo = p->vram[p->vram_addr & 0x7FFF];
            p->vram_prefetch_hi = p->vram[p->vram_addr & 0x7FFF] >> 8;
            p->vram_addr += p->vram_inc_step;
        }
        return v;
    }
    case 0x3A: { /* RDVRAMH */
        u8 v = p->vram_prefetch_hi;
        if (p->vram_inc_high) {
            p->vram_prefetch_lo = p->vram[p->vram_addr & 0x7FFF];
            p->vram_prefetch_hi = p->vram[p->vram_addr & 0x7FFF] >> 8;
            p->vram_addr += p->vram_inc_step;
        }
        return v;
    }
    case 0x3B: { /* RDCGRAM */
        u16 w = p->cgram[p->cgram_addr >> 1];
        u8 v = (p->cgram_flip) ? (u8)(w >> 8) : (u8)w;
        p->cgram_flip ^= 1;
        if (!p->cgram_flip) p->cgram_addr++;
        return v;
    }
    case 0x3E: return 0x01; /* STAT77 */
    case 0x3F: {
        u8 v = (p->scanline >= SNES_VBLANK_START) ? 0x80 : 0x00;
        v |= p->field;
        return v;
    }
    default: return 0;
    }
}

static void ppu_write(SNES *s, u8 reg, u8 val) {
    PPU *p = &s->ppu;
    switch (reg) {
    case 0x00: p->inidisp = val; p->force_blank = (val>>7)&1; p->brightness = val&0xF; break;
    case 0x01: p->obj_base=(val&7); p->obj_namesel=((val>>3)&3)+1; p->obj_size=(val>>5)&7; break;
    case 0x02: p->oam_addr = (u16)(val << 1); p->oam_priority = 0; p->oam_flip = 0; break;
    case 0x03: p->oam_addr = (p->oam_addr & 0x1FE) | ((val&1)<<8); p->oam_priority = (val>>7)&1; break;
    case 0x04: { /* OAMDATA */
        if (p->oam_addr & 0x100) {
            p->oam[0x200 + ((p->oam_addr-0x100) & 0x1F)] = val;
        } else if (p->oam_flip) {
            p->oam[(p->oam_addr-1) & 0xFF] = p->oam_latch;
            p->oam[p->oam_addr & 0xFF] = val;
        } else {
            p->oam_latch = val;
        }
        p->oam_flip ^= 1;
        if (!p->oam_flip) p->oam_addr = (p->oam_addr + 2) & 0x21F;
        break;
    }
    case 0x05: p->bg_mode = val & 7; p->bg3_priority = (val>>3)&1; break;
    case 0x06: p->mosaic_size = (val>>4)&0xF; p->mosaic_en = val&0xF; break;
    case 0x07: p->bg_tilemap_addr[0] = (u16)((val>>2)&0x1F)*0x400; p->bg_tilemap_w[0]=(val>>0)&1; p->bg_tilemap_h[0]=(val>>1)&1; break;
    case 0x08: p->bg_tilemap_addr[1] = (u16)((val>>2)&0x1F)*0x400; p->bg_tilemap_w[1]=(val>>0)&1; p->bg_tilemap_h[1]=(val>>1)&1; break;
    case 0x09: p->bg_tilemap_addr[2] = (u16)((val>>2)&0x1F)*0x400; p->bg_tilemap_w[2]=(val>>0)&1; p->bg_tilemap_h[2]=(val>>1)&1; break;
    case 0x0A: p->bg_tilemap_addr[3] = (u16)((val>>2)&0x1F)*0x400; p->bg_tilemap_w[3]=(val>>0)&1; p->bg_tilemap_h[3]=(val>>1)&1; break;
    case 0x0B: p->bg_chrbase[0]=(u16)(val&0xF)*0x1000; p->bg_chrbase[1]=(u16)((val>>4)&0xF)*0x1000; break;
    case 0x0C: p->bg_chrbase[2]=(u16)(val&0xF)*0x1000; p->bg_chrbase[3]=(u16)((val>>4)&0xF)*0x1000; break;
    case 0x0D: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_hofs[0]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x0E: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_vofs[0]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x0F: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_hofs[1]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x10: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_vofs[1]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x11: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_hofs[2]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x12: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_vofs[2]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x13: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_hofs[3]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x14: { u16 v=(u16)((p->bg_prev_latch<<8)|val); p->bg_vofs[3]=v&0x3FF; p->bg_prev_latch=val; break; }
    case 0x15: { /* VMAIN */
        u8 step_tab[4]={1,32,128,128};
        p->vram_inc_high = (val>>7)&1;
        p->vram_remap    = (val>>2)&3;
        p->vram_inc_step = step_tab[val&3];
        break;
    }
    case 0x16: p->vram_addr = (p->vram_addr & 0x7F00) | val; break;
    case 0x17: p->vram_addr = (p->vram_addr & 0x00FF) | (((u16)val & 0x7F) << 8); break;
    case 0x18: { /* VMDATAL */
        u8 *vh = &p->vram[(p->vram_addr & 0x7FFF) * 2];
        vh[0] = val;
        if (!p->vram_inc_high) p->vram_addr += p->vram_inc_step;
        break;
    }
    case 0x19: { /* VMDATAH */
        u8 *vh = &p->vram[(p->vram_addr & 0x7FFF) * 2 + 1];
        vh[0] = val;
        if (p->vram_inc_high) p->vram_addr += p->vram_inc_step;
        break;
    }
    case 0x1A: p->m7sel = val; break;
    case 0x1B: p->m7a = (s16)((p->m7_prev) | ((u16)val << 8)); p->m7_prev = val; break;
    case 0x1C: p->m7b = (s16)((p->m7_prev) | ((u16)val << 8)); p->m7_prev = val; break;
    case 0x1D: p->m7c = (s16)((p->m7_prev) | ((u16)val << 8)); p->m7_prev = val; break;
    case 0x1E: p->m7d = (s16)((p->m7_prev) | ((u16)val << 8)); p->m7_prev = val; break;
    case 0x1F: p->m7x = (s16)((p->m7_prev) | ((u16)val << 8)); p->m7_prev = val; break;
    case 0x20: p->m7y = (s16)((p->m7_prev) | ((u16)val << 8)); p->m7_prev = val; break;
    case 0x21: p->cgram_addr = (u8)(val * 2); p->cgram_flip = 0; break;
    case 0x22: { /* CGDATA */
        if (p->cgram_flip) {
            p->cgram[p->cgram_addr >> 1] = p->cgram_lo | ((u16)(val & 0x7F) << 8);
            p->cgram_addr++;
        } else {
            p->cgram_lo = val;
        }
        p->cgram_flip ^= 1;
        break;
    }
    case 0x23: p->w1l = (val>>0)&0xF; p->w1r = (val>>4)&0xF; break;
    case 0x24: p->w2l = (val>>0)&0xF; p->w2r = (val>>4)&0xF; break;
    case 0x25: break;
    case 0x2C: p->tmain = val; break;
    case 0x2D: p->tsub  = val; break;
    case 0x2E: p->w_tmain = val; break;
    case 0x2F: p->w_tsub  = val; break;
    case 0x30: p->cgwsel  = val; break;
    case 0x31: p->cgadsub = val; break;
    case 0x32: p->fixed_r=(val&0x1F); p->fixed_g=((val>>5)&0x1F); break;
    case 0x33: break; /* SETINI */
    default: break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DMA
 * ═══════════════════════════════════════════════════════════════════════════ */
static u8 dma_read(SNES *s, u8 ch, u8 reg) {
    DMAChannel *d = &s->dma[ch & 7];
    switch(reg & 0xF) {
    case 0: return d->params;
    case 1: return d->dst_reg;
    case 2: return (u8)(d->src_addr & 0xFF);
    case 3: return (u8)((d->src_addr >> 8) & 0xFF);
    case 4: return (u8)((d->src_addr >> 16) & 0xFF);
    case 5: return (u8)(d->size & 0xFF);
    case 6: return (u8)(d->size >> 8);
    default: return 0;
    }
}
static void dma_write(SNES *s, u8 ch, u8 reg, u8 val) {
    DMAChannel *d = &s->dma[ch & 7];
    switch(reg & 0xF) {
    case 0: d->params = val; break;
    case 1: d->dst_reg = val; break;
    case 2: d->src_addr = (d->src_addr & 0xFFFF00) | val; break;
    case 3: d->src_addr = (d->src_addr & 0xFF00FF) | ((u32)val << 8); break;
    case 4: d->src_addr = (d->src_addr & 0x00FFFF) | ((u32)val << 16); break;
    case 5: d->size = (d->size & 0xFF00) | val; break;
    case 6: d->size = (d->size & 0x00FF) | ((u16)val << 8); break;
    default: break;
    }
}
/* Byte patterns for each DMA transfer mode */
static const u8 dma_pat[8][4] = {
    {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,1,2,3},{0,1,2,3},{0,1,0,1},{0,0,0,0},{0,0,0,0}
};
static const u8 dma_pat_len[8] = {1,2,2,4,4,4,2,4};
static void dma_run(SNES *s, u8 channels) {
    for (int ch = 0; ch < 8; ch++) {
        if (!(channels & (1 << ch))) continue;
        DMAChannel *d = &s->dma[ch];
        u8 mode  = d->params & 7;
        int B2A  = (d->params >> 7) & 1;
        int decr = (d->params >> 4) & 1;
        u8 pat_n = dma_pat_len[mode];
        u32 src  = d->src_addr;
        u16 count = d->size ? d->size : 0x10000;
        for (u32 i = 0; i < count; i++) {
            u8 off = dma_pat[mode][i % pat_n];
            if (!B2A) {
                u8 v = cpu_read(s, src);
                ppu_write(s, (u8)(d->dst_reg + off), v);
            } else {
                u8 v = ppu_read(s, (u8)(d->dst_reg + off));
                cpu_write(s, src, v);
            }
            if (decr) src = ((src - 1) & 0xFFFF) | (src & 0xFF0000);
            else      src = ((src + 1) & 0xFFFF) | (src & 0xFF0000);
        }
        d->size = 0;
    }
}
static void hdma_init(SNES *s) {
    for (int ch=0; ch<8; ch++) {
        if (!(s->hdma_en & (1<<ch))) continue;
        DMAChannel *d = &s->dma[ch];
        d->hdma_table_addr = (u16)(d->src_addr & 0xFFFF);
        d->hdma_table_bank = (u8)(d->src_addr >> 16);
        d->hdma_line_counter = 0;
        d->hdma_completed = 0;
    }
}
static void hdma_run(SNES *s) {
    for (int ch=0; ch<8; ch++) {
        if (!(s->hdma_en & (1<<ch))) continue;
        DMAChannel *d = &s->dma[ch];
        if (d->hdma_completed) continue;
        if (d->hdma_line_counter == 0) {
            d->hdma_line_counter = cpu_read(s, ((u32)d->hdma_table_bank<<16)|d->hdma_table_addr);
            d->hdma_table_addr++;
            if (!d->hdma_line_counter) { d->hdma_completed = 1; continue; }
        }
        /* One transfer per scanline */
        u8 v = cpu_read(s, ((u32)d->hdma_table_bank<<16)|d->hdma_table_addr);
        ppu_write(s, d->dst_reg, v);
        d->hdma_table_addr++;
        d->hdma_line_counter--;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PPU SCANLINE RENDERER
 * ═══════════════════════════════════════════════════════════════════════════ */
static u32 snes_color(u16 c) {
    u32 r=(c&0x1F)<<3, g=((c>>5)&0x1F)<<3, b=((c>>10)&0x1F)<<3;
    return (r<<16)|(g<<8)|b;
}

/* Lookup one BG pixel at (x, y) screen coordinates.
 * Returns palette index (0 = transparent). */
static u8 ppu_bg_pixel(SNES *s, int bg_idx, int x, int y) {
    PPU *p = &s->ppu;
    static const u8 bpp_mode[8][4] = {
        {2,2,2,2},{4,4,2,0},{4,4,0,0},{8,4,0,0},
        {8,2,0,0},{4,2,0,0},{4,0,0,0},{8,0,0,0}
    };
    u8 bpp = bpp_mode[p->bg_mode & 7][bg_idx];
    if (!bpp) return 0;
    int px = (x + p->bg_hofs[bg_idx]) & 0x3FF;
    int py = (y + p->bg_vofs[bg_idx]) & 0x3FF;
    int tile_x = px >> 3, tile_y = py >> 3;
    int bx = px & 7, by = py & 7;
    /* Tilemap is at bg_tilemap_addr[] in VRAM half-words */
    u16 map_w = 32 << p->bg_tilemap_w[bg_idx];
    u16 map_base = p->bg_tilemap_addr[bg_idx] >> 1; /* words */
    u32 map_off = (u32)(tile_y % 32) * 32 + (tile_x % 32);
    if (tile_x >= 32) map_off += (p->bg_tilemap_w[bg_idx] ? 32*32 : 0);
    if (tile_y >= 32) map_off += (p->bg_tilemap_h[bg_idx] ? map_w*32 : 0);
    u16 entry = p->vram[(map_base + map_off) & 0x7FFF];
    u16 tile  = entry & 0x3FF;
    int hf = (entry>>14)&1, vf=(entry>>15)&1;
    if (hf) bx = 7-bx;
    if (vf) by = 7-by;
    u32 chr_base = p->bg_chrbase[bg_idx] >> 1;
    u32 tile_off = chr_base + tile * (bpp * 4) + by;
    u8 col = 0;
    u16 w0 = p->vram[tile_off & 0x7FFF];
    u8 bit = (u8)(7 - bx);
    col = ((w0 >> bit) & 1) | (((w0 >> (8+bit)) & 1) << 1);
    if (bpp >= 4) {
        u16 w1 = p->vram[(tile_off + 8) & 0x7FFF];
        col |= (((w1 >> bit) & 1) << 2) | (((w1 >> (8+bit)) & 1) << 3);
    }
    if (bpp >= 8) {
        u16 w2 = p->vram[(tile_off + 16) & 0x7FFF];
        u16 w3 = p->vram[(tile_off + 24) & 0x7FFF];
        col |= (((w2 >> bit) & 1) << 4) | (((w2 >> (8+bit)) & 1) << 5);
        col |= (((w3 >> bit) & 1) << 6) | (((w3 >> (8+bit)) & 1) << 7);
    }
    if (!col) return 0;
    /* Palette: palette group from tile attribute */
    u8 pal_off = (bpp < 8) ? ((entry >> 10) & 7) * (1 << bpp) : 0;
    return pal_off + col;
}

static void ppu_run_scanline(SNES *s) {
    PPU *p = &s->ppu;
    int y = p->scanline;
    if (y < 0 || y >= SNES_SCREEN_H) return;
    if (p->force_blank) {
        u32 *row = &p->framebuf[y * SNES_SCREEN_W];
        for (int x = 0; x < SNES_SCREEN_W; x++) row[x] = 0;
        return;
    }
    u32 *row = &p->framebuf[y * SNES_SCREEN_W];
    u8 mode = p->bg_mode & 7;
    hdma_run(s);
    if (mode == 7) {
        /* Mode 7 affine background */
        for (int x = 0; x < SNES_SCREEN_W; x++) {
            s32 sx = x - 128, sy = y - 128;
            s32 tx = ((s32)p->m7a * sx + (s32)p->m7b * sy) >> 8;
            s32 ty = ((s32)p->m7c * sx + (s32)p->m7d * sy) >> 8;
            tx += (s32)p->m7x; ty += (s32)p->m7y;
            tx &= 0xFF; ty &= 0xFF;
            /* Tile index at (tx/8, ty/8) */
            u16 tile_idx = (u16)(p->vram[((ty>>3)*128 + (tx>>3)) & 0x7FFF] & 0xFF);
            /* Pixel within tile */
            u8 pidx = (u8)(p->vram[(tile_idx*64 + (ty&7)*8 + (tx&7)) & 0x7FFF] >> 8);
            row[x] = snes_color(p->cgram[pidx & 0xFF]);
        }
        return;
    }
    for (int x = 0; x < SNES_SCREEN_W; x++) {
        u32 pixel = snes_color(p->cgram[0]); /* backdrop */
        /* BG layers: iterate from lowest priority to highest so later overwrites */
        for (int bg = 3; bg >= 0; bg--) {
            if (!(p->tmain & (1 << bg))) continue;
            u8 col = ppu_bg_pixel(s, bg, x, y);
            if (col) {
                pixel = snes_color(p->cgram[col & 0xFF]);
                break; /* higher priority layer wins */
            }
        }
        row[x] = pixel;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 65816 CPU INSTRUCTION STEP
 * ═══════════════════════════════════════════════════════════════════════════ */
static int cpu_step(SNES *s) {
    CPU816 *c = &s->cpu;
    if (c->wai) {
        if (c->nmi_pending || c->irq_pending) c->wai = 0;
        else return 3;
    }
    if (c->stp) return 3;

    /* Handle NMI */
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        if (!c->e) cpu_push8(s, c->pbr);
        cpu_push16(s, c->pc);
        cpu_push8(s, cpu_get_p(c));
        c->i = 1; c->pbr = 0;
        u16 vec = c->e ? 0xFFFA : 0xFFEA;
        c->pc = cpu_read16(s, vec);
        return 8;
    }
    /* Handle IRQ */
    if (c->irq_pending && !c->i) {
        c->irq_pending = 0;
        if (!c->e) cpu_push8(s, c->pbr);
        cpu_push16(s, c->pc);
        cpu_push8(s, cpu_get_p(c));
        c->i = 1; c->pbr = 0;
        u16 vec = c->e ? 0xFFFE : 0xFFEE;
        c->pc = cpu_read16(s, vec);
        return 8;
    }

    u8 op = cpu_fetch(s);
    u8 m8 = c->m, x8 = c->xf;

/* Macros for common patterns */
#define LD_IMM8(reg)  do{ u8  v=cpu_fetch(s); (reg)=((reg)&0xFF00)|v; cpu_nz8(c,(u8)(reg)); }while(0)
#define LD_IMM16(reg) do{ u16 v=cpu_fetch16(s); (reg)=v; cpu_nz16(c,v); }while(0)
#define LD_EA8(reg,ea) do{ u8 v=ea_read8(s,ea);  (reg)=((reg)&0xFF00)|v; cpu_nz8(c,v); }while(0)
#define LD_EA16(reg,ea) do{ u16 v=ea_read16(s,ea); (reg)=v; cpu_nz16(c,v); }while(0)
#define ST8(ea,reg) ea_write8(s,ea,(u8)(reg))
#define ST16(ea,reg) ea_write16(s,ea,(u16)(reg))
#define BRANCH(cond) do{ s8 off=(s8)cpu_fetch(s); if(cond){c->pc+=(s16)off; return 3;} return 2; }while(0)

    switch(op) {
    /* ── Load / Store ─────────────────────────────────────────────────── */
    case 0xA9: if(m8){LD_IMM8(c->a);}else{LD_IMM16(c->a);} return m8?2:3; /* LDA # */
    case 0xA5: {u32 ea=ea_dp(s);   if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?3:5;} /* LDA dp */
    case 0xB5: {u32 ea=ea_dpx(s);  if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?4:6;} /* LDA dp,X */
    case 0xAD: {u32 ea=ea_abs(s);  if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?4:5;} /* LDA abs */
    case 0xBD: {u32 ea=ea_absx(s); if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?4:5;} /* LDA abs,X */
    case 0xB9: {u32 ea=ea_absy(s); if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?4:5;} /* LDA abs,Y */
    case 0xAF: {u32 ea=ea_long(s); if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?5:6;} /* LDA long */
    case 0xBF: {u32 ea=ea_longx(s);if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?5:6;} /* LDA long,X */
    case 0xB1: {u32 ptr=ea_dp(s);u32 ea=((u32)c->dbr<<16)|(cpu_read16(s,ptr)+c->y); if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?5:7;} /* LDA (dp),Y */
    case 0xA1: {u32 ea=ea_ind_dpx(s); if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?6:7;} /* LDA (dp,X) */
    case 0xB2: {u32 ea=ea_ind_dp(s);  if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?5:6;} /* LDA (dp) */
    case 0xA3: {u32 ea=ea_sr(s);       if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?4:5;} /* LDA sr,S */
    case 0xB3: {u32 ea=ea_sry(s);      if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?7:8;} /* LDA (sr,S),Y */
    case 0xA7: {u32 ea=ea_ind_long_dp(s); if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?6:7;} /* LDA [dp] */
    case 0xB7: {u32 ea=ea_ind_long_dpy(s);if(m8){LD_EA8(c->a,ea);}else{LD_EA16(c->a,ea);} return m8?6:7;} /* LDA [dp],Y */

    case 0x85: {u32 ea=ea_dp(s);   if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?3:5;} /* STA dp */
    case 0x95: {u32 ea=ea_dpx(s);  if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?4:6;} /* STA dp,X */
    case 0x8D: {u32 ea=ea_abs(s);  if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?4:5;} /* STA abs */
    case 0x9D: {u32 ea=ea_absx(s); if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?5:6;} /* STA abs,X */
    case 0x99: {u32 ea=ea_absy(s); if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?5:6;} /* STA abs,Y */
    case 0x8F: {u32 ea=ea_long(s); if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?5:6;} /* STA long */
    case 0x9F: {u32 ea=ea_longx(s);if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?5:6;} /* STA long,X */
    case 0x91: {u32 ptr=ea_dp(s);u32 ea=((u32)c->dbr<<16)|(cpu_read16(s,ptr)+c->y); if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?6:7;} /* STA (dp),Y */
    case 0x81: {u32 ea=ea_ind_dpx(s); if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?6:7;} /* STA (dp,X) */
    case 0x92: {u32 ea=ea_ind_dp(s);  if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?5:6;} /* STA (dp) */
    case 0x83: {u32 ea=ea_sr(s);      if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?4:5;} /* STA sr,S */
    case 0x93: {u32 ea=ea_sry(s);     if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?7:8;} /* STA (sr,S),Y */
    case 0x87: {u32 ea=ea_ind_long_dp(s); if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?6:7;} /* STA [dp] */
    case 0x97: {u32 ea=ea_ind_long_dpy(s);if(m8){ST8(ea,c->a);}else{ST16(ea,c->a);} return m8?6:7;} /* STA [dp],Y */

    case 0xA2: if(x8){LD_IMM8(c->x);}else{LD_IMM16(c->x);} return x8?2:3; /* LDX # */
    case 0xA6: {u32 ea=ea_dp(s);  if(x8){LD_EA8(c->x,ea);}else{LD_EA16(c->x,ea);} return x8?3:5;} /* LDX dp */
    case 0xAE: {u32 ea=ea_abs(s); if(x8){LD_EA8(c->x,ea);}else{LD_EA16(c->x,ea);} return x8?4:5;} /* LDX abs */
    case 0xB6: {u32 ea=ea_dpy(s); if(x8){LD_EA8(c->x,ea);}else{LD_EA16(c->x,ea);} return x8?4:6;} /* LDX dp,Y */
    case 0xBE: {u32 ea=ea_absy(s);if(x8){LD_EA8(c->x,ea);}else{LD_EA16(c->x,ea);} return x8?4:5;} /* LDX abs,Y */
    case 0x86: {u32 ea=ea_dp(s);  if(x8){ST8(ea,c->x);}else{ST16(ea,c->x);} return x8?3:5;} /* STX dp */
    case 0x8E: {u32 ea=ea_abs(s); if(x8){ST8(ea,c->x);}else{ST16(ea,c->x);} return x8?4:5;} /* STX abs */
    case 0x96: {u32 ea=ea_dpy(s); if(x8){ST8(ea,c->x);}else{ST16(ea,c->x);} return x8?4:6;} /* STX dp,Y */

    case 0xA0: if(x8){LD_IMM8(c->y);}else{LD_IMM16(c->y);} return x8?2:3; /* LDY # */
    case 0xA4: {u32 ea=ea_dp(s);  if(x8){LD_EA8(c->y,ea);}else{LD_EA16(c->y,ea);} return x8?3:5;} /* LDY dp */
    case 0xAC: {u32 ea=ea_abs(s); if(x8){LD_EA8(c->y,ea);}else{LD_EA16(c->y,ea);} return x8?4:5;} /* LDY abs */
    case 0xB4: {u32 ea=ea_dpx(s); if(x8){LD_EA8(c->y,ea);}else{LD_EA16(c->y,ea);} return x8?4:6;} /* LDY dp,X */
    case 0xBC: {u32 ea=ea_absx(s);if(x8){LD_EA8(c->y,ea);}else{LD_EA16(c->y,ea);} return x8?4:5;} /* LDY abs,X */
    case 0x84: {u32 ea=ea_dp(s);  if(x8){ST8(ea,c->y);}else{ST16(ea,c->y);} return x8?3:5;} /* STY dp */
    case 0x8C: {u32 ea=ea_abs(s); if(x8){ST8(ea,c->y);}else{ST16(ea,c->y);} return x8?4:5;} /* STY abs */
    case 0x94: {u32 ea=ea_dpx(s); if(x8){ST8(ea,c->y);}else{ST16(ea,c->y);} return x8?4:6;} /* STY dp,X */

    /* STZ */
    case 0x64: {u32 ea=ea_dp(s);   if(m8){ST8(ea,0);}else{ST16(ea,0);} return m8?3:5;}
    case 0x74: {u32 ea=ea_dpx(s);  if(m8){ST8(ea,0);}else{ST16(ea,0);} return m8?4:6;}
    case 0x9C: {u32 ea=ea_abs(s);  if(m8){ST8(ea,0);}else{ST16(ea,0);} return m8?4:5;}
    case 0x9E: {u32 ea=ea_absx(s); if(m8){ST8(ea,0);}else{ST16(ea,0);} return m8?5:6;}

    /* ── Transfers ────────────────────────────────────────────────────── */
    case 0xAA: if(x8){c->x=(c->x&0xFF00)|(c->a&0xFF);cpu_nz8(c,(u8)c->x);}else{c->x=c->a;cpu_nz16(c,c->x);} return 2; /* TAX */
    case 0xA8: if(x8){c->y=(c->y&0xFF00)|(c->a&0xFF);cpu_nz8(c,(u8)c->y);}else{c->y=c->a;cpu_nz16(c,c->y);} return 2; /* TAY */
    case 0x8A: if(m8){c->a=(c->a&0xFF00)|(c->x&0xFF);cpu_nz8(c,(u8)c->a);}else{c->a=c->x;cpu_nz16(c,c->a);} return 2; /* TXA */
    case 0x98: if(m8){c->a=(c->a&0xFF00)|(c->y&0xFF);cpu_nz8(c,(u8)c->a);}else{c->a=c->y;cpu_nz16(c,c->a);} return 2; /* TYA */
    case 0xBA: if(x8){c->x=(c->x&0xFF00)|(c->sp&0xFF);cpu_nz8(c,(u8)c->x);}else{c->x=c->sp;cpu_nz16(c,c->x);} return 2; /* TSX */
    case 0x9A: c->sp = x8 ? (0x0100|(c->x&0xFF)) : c->x; return 2; /* TXS */
    case 0x9B: c->y = c->x; cpu_nz16(c,c->y); return 2; /* TXY */
    case 0xBB: c->x = c->y; cpu_nz16(c,c->x); return 2; /* TYX */
    case 0x5B: c->d = c->a; cpu_nz16(c,c->d); return 2; /* TCD */
    case 0x7B: c->a = c->d; cpu_nz16(c,c->a); return 2; /* TDC */
    case 0x1B: c->sp= c->a; return 2; /* TCS */
    case 0x3B: c->a = c->sp; cpu_nz16(c,c->a); return 2; /* TSC */

    /* ── ALU ──────────────────────────────────────────────────────────── */
#define ALU_OP(name8,name16,ea) do{ \
    if(m8){u8 v=ea_read8(s,ea);c->a=(c->a&0xFF00)|name8(c,(u8)c->a,v);} \
    else  {u16 v=ea_read16(s,ea);c->a=name16(c,c->a,v);} }while(0)
    case 0x69: if(m8){u8 v=cpu_fetch(s);c->a=(c->a&0xFF00)|alu_adc8(c,(u8)c->a,v);}else{u16 v=cpu_fetch16(s);c->a=alu_adc16(c,c->a,v);} return m8?2:3;
    case 0x65: {u32 ea=ea_dp(s);   ALU_OP(alu_adc8,alu_adc16,ea); return m8?3:5;}
    case 0x6D: {u32 ea=ea_abs(s);  ALU_OP(alu_adc8,alu_adc16,ea); return m8?4:5;}
    case 0x75: {u32 ea=ea_dpx(s);  ALU_OP(alu_adc8,alu_adc16,ea); return m8?4:6;}
    case 0x7D: {u32 ea=ea_absx(s); ALU_OP(alu_adc8,alu_adc16,ea); return m8?4:5;}
    case 0x79: {u32 ea=ea_absy(s); ALU_OP(alu_adc8,alu_adc16,ea); return m8?4:5;}
    case 0x6F: {u32 ea=ea_long(s); ALU_OP(alu_adc8,alu_adc16,ea); return m8?5:6;}
    case 0x7F: {u32 ea=ea_longx(s);ALU_OP(alu_adc8,alu_adc16,ea); return m8?5:6;}
    case 0x61: {u32 ea=ea_ind_dpx(s);ALU_OP(alu_adc8,alu_adc16,ea); return m8?6:7;}
    case 0x72: {u32 ea=ea_ind_dp(s); ALU_OP(alu_adc8,alu_adc16,ea); return m8?5:6;}
    case 0x71: {u32 ptr=ea_dp(s);u32 ea=((u32)c->dbr<<16)|(cpu_read16(s,ptr)+c->y);ALU_OP(alu_adc8,alu_adc16,ea);return m8?5:7;}
    case 0x67: {u32 ea=ea_ind_long_dp(s); ALU_OP(alu_adc8,alu_adc16,ea); return m8?6:7;}
    case 0x77: {u32 ea=ea_ind_long_dpy(s);ALU_OP(alu_adc8,alu_adc16,ea); return m8?6:7;}
    case 0x63: {u32 ea=ea_sr(s);  ALU_OP(alu_adc8,alu_adc16,ea); return m8?4:5;}
    case 0x73: {u32 ea=ea_sry(s); ALU_OP(alu_adc8,alu_adc16,ea); return m8?7:8;}
    case 0xE9: if(m8){u8 v=cpu_fetch(s);c->a=(c->a&0xFF00)|alu_sbc8(c,(u8)c->a,v);}else{u16 v=cpu_fetch16(s);c->a=alu_sbc16(c,c->a,v);} return m8?2:3;
    case 0xE5: {u32 ea=ea_dp(s);   ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?3:5;}
    case 0xED: {u32 ea=ea_abs(s);  ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?4:5;}
    case 0xF5: {u32 ea=ea_dpx(s);  ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?4:6;}
    case 0xFD: {u32 ea=ea_absx(s); ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?4:5;}
    case 0xF9: {u32 ea=ea_absy(s); ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?4:5;}
    case 0xEF: {u32 ea=ea_long(s); ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?5:6;}
    case 0xFF: {u32 ea=ea_longx(s);ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?5:6;}
    case 0xF2: {u32 ea=ea_ind_dp(s); ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?5:6;}
    case 0xE3: {u32 ea=ea_sr(s);   ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?4:5;}
    case 0xF3: {u32 ea=ea_sry(s);  ALU_OP(alu_sbc8,alu_sbc16,ea); return m8?7:8;}

    /* AND */
    case 0x29: if(m8){c->a=(c->a&0xFF00)|((c->a&cpu_fetch(s)&0xFF));cpu_nz8(c,(u8)c->a);}else{c->a&=cpu_fetch16(s);cpu_nz16(c,c->a);} return m8?2:3;
    case 0x25: {u32 ea=ea_dp(s);  if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?3:5;}
    case 0x2D: {u32 ea=ea_abs(s); if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:5;}
    case 0x35: {u32 ea=ea_dpx(s); if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:6;}
    case 0x3D: {u32 ea=ea_absx(s);if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:5;}
    case 0x39: {u32 ea=ea_absy(s);if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:5;}
    case 0x2F: {u32 ea=ea_long(s);if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?5:6;}
    case 0x32: {u32 ea=ea_ind_dp(s);if(m8){c->a&=(0xFF00|ea_read8(s,ea));cpu_nz8(c,(u8)c->a);}else{c->a&=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?5:6;}
    /* ORA */
    case 0x09: if(m8){c->a|=cpu_fetch(s);cpu_nz8(c,(u8)c->a);}else{c->a|=cpu_fetch16(s);cpu_nz16(c,c->a);} return m8?2:3;
    case 0x05: {u32 ea=ea_dp(s);  if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?3:5;}
    case 0x0D: {u32 ea=ea_abs(s); if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:5;}
    case 0x15: {u32 ea=ea_dpx(s); if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:6;}
    case 0x1D: {u32 ea=ea_absx(s);if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:5;}
    case 0x19: {u32 ea=ea_absy(s);if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?4:5;}
    case 0x0F: {u32 ea=ea_long(s);if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);} return m8?5:6;}
    case 0x12: {u32 ea=ea_ind_dp(s);if(m8){c->a|=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a|=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?5:6;}
    /* EOR */
    case 0x49: if(m8){c->a^=cpu_fetch(s);cpu_nz8(c,(u8)c->a);}else{c->a^=cpu_fetch16(s);cpu_nz16(c,c->a);} return m8?2:3;
    case 0x45: {u32 ea=ea_dp(s);  if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?3:5;}
    case 0x4D: {u32 ea=ea_abs(s); if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?4:5;}
    case 0x55: {u32 ea=ea_dpx(s); if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?4:6;}
    case 0x5D: {u32 ea=ea_absx(s);if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?4:5;}
    case 0x59: {u32 ea=ea_absy(s);if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?4:5;}
    case 0x4F: {u32 ea=ea_long(s);if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?5:6;}
    case 0x52: {u32 ea=ea_ind_dp(s);if(m8){c->a^=ea_read8(s,ea);cpu_nz8(c,(u8)c->a);}else{c->a^=ea_read16(s,ea);cpu_nz16(c,c->a);}return m8?5:6;}
    /* CMP */
    case 0xC9: if(m8)alu_cmp8(c,(u8)c->a,cpu_fetch(s));else alu_cmp16(c,c->a,cpu_fetch16(s)); return m8?2:3;
    case 0xC5: {u32 ea=ea_dp(s);  if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea)); return m8?3:5;}
    case 0xCD: {u32 ea=ea_abs(s); if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea)); return m8?4:5;}
    case 0xD5: {u32 ea=ea_dpx(s); if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea)); return m8?4:6;}
    case 0xDD: {u32 ea=ea_absx(s);if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea)); return m8?4:5;}
    case 0xD9: {u32 ea=ea_absy(s);if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea)); return m8?4:5;}
    case 0xCF: {u32 ea=ea_long(s);if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea)); return m8?5:6;}
    case 0xD2: {u32 ea=ea_ind_dp(s);if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea));return m8?5:6;}
    case 0xD1: {u32 ptr=ea_dp(s);u32 ea=((u32)c->dbr<<16)|(cpu_read16(s,ptr)+c->y);if(m8)alu_cmp8(c,(u8)c->a,ea_read8(s,ea));else alu_cmp16(c,c->a,ea_read16(s,ea));return m8?5:7;}
    /* CPX / CPY */
    case 0xE0: if(x8)alu_cmp8(c,(u8)c->x,cpu_fetch(s));else alu_cmp16(c,c->x,cpu_fetch16(s)); return x8?2:3;
    case 0xE4: {u32 ea=ea_dp(s);  if(x8)alu_cmp8(c,(u8)c->x,ea_read8(s,ea));else alu_cmp16(c,c->x,ea_read16(s,ea)); return x8?3:5;}
    case 0xEC: {u32 ea=ea_abs(s); if(x8)alu_cmp8(c,(u8)c->x,ea_read8(s,ea));else alu_cmp16(c,c->x,ea_read16(s,ea)); return x8?4:5;}
    case 0xC0: if(x8)alu_cmp8(c,(u8)c->y,cpu_fetch(s));else alu_cmp16(c,c->y,cpu_fetch16(s)); return x8?2:3;
    case 0xC4: {u32 ea=ea_dp(s);  if(x8)alu_cmp8(c,(u8)c->y,ea_read8(s,ea));else alu_cmp16(c,c->y,ea_read16(s,ea)); return x8?3:5;}
    case 0xCC: {u32 ea=ea_abs(s); if(x8)alu_cmp8(c,(u8)c->y,ea_read8(s,ea));else alu_cmp16(c,c->y,ea_read16(s,ea)); return x8?4:5;}

    /* ── INC/DEC ────────────────────────────────────────────────────── */
    case 0x1A: if(m8){u8 v=(u8)c->a+1;c->a=(c->a&0xFF00)|v;cpu_nz8(c,v);}else{c->a++;cpu_nz16(c,c->a);} return 2; /* INC A */
    case 0x3A: if(m8){u8 v=(u8)c->a-1;c->a=(c->a&0xFF00)|v;cpu_nz8(c,v);}else{c->a--;cpu_nz16(c,c->a);} return 2; /* DEC A */
    case 0xE6: {u32 ea=ea_dp(s);  if(m8){u8 v=ea_read8(s,ea)+1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)+1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?5:7;} /* INC dp */
    case 0xEE: {u32 ea=ea_abs(s); if(m8){u8 v=ea_read8(s,ea)+1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)+1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?6:8;} /* INC abs */
    case 0xF6: {u32 ea=ea_dpx(s); if(m8){u8 v=ea_read8(s,ea)+1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)+1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?6:8;} /* INC dp,X */
    case 0xFE: {u32 ea=ea_absx(s);if(m8){u8 v=ea_read8(s,ea)+1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)+1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?7:9;} /* INC abs,X */
    case 0xC6: {u32 ea=ea_dp(s);  if(m8){u8 v=ea_read8(s,ea)-1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)-1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?5:7;} /* DEC dp */
    case 0xCE: {u32 ea=ea_abs(s); if(m8){u8 v=ea_read8(s,ea)-1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)-1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?6:8;} /* DEC abs */
    case 0xD6: {u32 ea=ea_dpx(s); if(m8){u8 v=ea_read8(s,ea)-1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)-1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?6:8;} /* DEC dp,X */
    case 0xDE: {u32 ea=ea_absx(s);if(m8){u8 v=ea_read8(s,ea)-1;ea_write8(s,ea,v);cpu_nz8(c,v);}else{u16 v=ea_read16(s,ea)-1;ea_write16(s,ea,v);cpu_nz16(c,v);} return m8?7:9;} /* DEC abs,X */
    case 0xE8: if(x8){c->x=(c->x&0xFF00)|(((c->x+1)&0xFF));cpu_nz8(c,(u8)c->x);}else{c->x++;cpu_nz16(c,c->x);} return 2; /* INX */
    case 0xCA: if(x8){c->x=(c->x&0xFF00)|(((c->x-1)&0xFF));cpu_nz8(c,(u8)c->x);}else{c->x--;cpu_nz16(c,c->x);} return 2; /* DEX */
    case 0xC8: if(x8){c->y=(c->y&0xFF00)|(((c->y+1)&0xFF));cpu_nz8(c,(u8)c->y);}else{c->y++;cpu_nz16(c,c->y);} return 2; /* INY */
    case 0x88: if(x8){c->y=(c->y&0xFF00)|(((c->y-1)&0xFF));cpu_nz8(c,(u8)c->y);}else{c->y--;cpu_nz16(c,c->y);} return 2; /* DEY */

    /* ── Shifts ────────────────────────────────────────────────────── */
#define ASL8(ea)  do{u8 v=ea_read8(s,ea);c->c=v>>7;v<<=1;ea_write8(s,ea,v);cpu_nz8(c,v);}while(0)
#define ASL16(ea) do{u16 v=ea_read16(s,ea);c->c=v>>15;v<<=1;ea_write16(s,ea,v);cpu_nz16(c,v);}while(0)
#define LSR8(ea)  do{u8 v=ea_read8(s,ea);c->c=v&1;v>>=1;ea_write8(s,ea,v);cpu_nz8(c,v);}while(0)
#define LSR16(ea) do{u16 v=ea_read16(s,ea);c->c=v&1;v>>=1;ea_write16(s,ea,v);cpu_nz16(c,v);}while(0)
#define ROL8(ea)  do{u8 v=ea_read8(s,ea);u8 nc=v>>7;v=(v<<1)|c->c;c->c=nc;ea_write8(s,ea,v);cpu_nz8(c,v);}while(0)
#define ROL16(ea) do{u16 v=ea_read16(s,ea);u8 nc=v>>15;v=(v<<1)|c->c;c->c=nc;ea_write16(s,ea,v);cpu_nz16(c,v);}while(0)
#define ROR8(ea)  do{u8 v=ea_read8(s,ea);u8 nc=v&1;v=(v>>1)|(c->c<<7);c->c=nc;ea_write8(s,ea,v);cpu_nz8(c,v);}while(0)
#define ROR16(ea) do{u16 v=ea_read16(s,ea);u8 nc=v&1;v=(v>>1)|(c->c<<15);c->c=nc;ea_write16(s,ea,v);cpu_nz16(c,v);}while(0)

    case 0x0A: if(m8){u8 nc=(u8)c->a>>7;c->a=(c->a&0xFF00)|(((u8)c->a<<1)&0xFF);c->c=nc;cpu_nz8(c,(u8)c->a);}else{c->c=c->a>>15;c->a<<=1;cpu_nz16(c,c->a);} return 2; /* ASL A */
    case 0x06: {u32 ea=ea_dp(s);  if(m8){ASL8(ea);}else{ASL16(ea);} return m8?5:7;} /* ASL dp */
    case 0x0E: {u32 ea=ea_abs(s); if(m8){ASL8(ea);}else{ASL16(ea);} return m8?6:8;} /* ASL abs */
    case 0x16: {u32 ea=ea_dpx(s); if(m8){ASL8(ea);}else{ASL16(ea);} return m8?6:8;} /* ASL dp,X */
    case 0x1E: {u32 ea=ea_absx(s);if(m8){ASL8(ea);}else{ASL16(ea);} return m8?7:9;} /* ASL abs,X */
    case 0x4A: if(m8){c->c=(u8)c->a&1;c->a=(c->a&0xFF00)|(((u8)c->a)>>1);cpu_nz8(c,(u8)c->a);}else{c->c=c->a&1;c->a>>=1;cpu_nz16(c,c->a);} return 2; /* LSR A */
    case 0x46: {u32 ea=ea_dp(s);  if(m8){LSR8(ea);}else{LSR16(ea);} return m8?5:7;}
    case 0x4E: {u32 ea=ea_abs(s); if(m8){LSR8(ea);}else{LSR16(ea);} return m8?6:8;}
    case 0x56: {u32 ea=ea_dpx(s); if(m8){LSR8(ea);}else{LSR16(ea);} return m8?6:8;}
    case 0x5E: {u32 ea=ea_absx(s);if(m8){LSR8(ea);}else{LSR16(ea);} return m8?7:9;}
    case 0x2A: if(m8){u8 nc=(u8)c->a>>7;c->a=(c->a&0xFF00)|(((u8)c->a<<1)|c->c);c->c=nc;cpu_nz8(c,(u8)c->a);}else{u8 nc=c->a>>15;c->a=(c->a<<1)|c->c;c->c=nc;cpu_nz16(c,c->a);} return 2; /* ROL A */
    case 0x26: {u32 ea=ea_dp(s);  if(m8){ROL8(ea);}else{ROL16(ea);} return m8?5:7;}
    case 0x2E: {u32 ea=ea_abs(s); if(m8){ROL8(ea);}else{ROL16(ea);} return m8?6:8;}
    case 0x36: {u32 ea=ea_dpx(s); if(m8){ROL8(ea);}else{ROL16(ea);} return m8?6:8;}
    case 0x3E: {u32 ea=ea_absx(s);if(m8){ROL8(ea);}else{ROL16(ea);} return m8?7:9;}
    case 0x6A: if(m8){u8 nc=(u8)c->a&1;c->a=(c->a&0xFF00)|(((u8)c->a>>1)|(c->c<<7));c->c=nc;cpu_nz8(c,(u8)c->a);}else{u8 nc=c->a&1;c->a=(c->a>>1)|(c->c<<15);c->c=nc;cpu_nz16(c,c->a);} return 2; /* ROR A */
    case 0x66: {u32 ea=ea_dp(s);  if(m8){ROR8(ea);}else{ROR16(ea);} return m8?5:7;}
    case 0x6E: {u32 ea=ea_abs(s); if(m8){ROR8(ea);}else{ROR16(ea);} return m8?6:8;}
    case 0x76: {u32 ea=ea_dpx(s); if(m8){ROR8(ea);}else{ROR16(ea);} return m8?6:8;}
    case 0x7E: {u32 ea=ea_absx(s);if(m8){ROR8(ea);}else{ROR16(ea);} return m8?7:9;}

    /* ── BIT test ───────────────────────────────────────────────────── */
    case 0x89: if(m8){c->z=!((u8)c->a&cpu_fetch(s));}else{c->z=!(c->a&cpu_fetch16(s));} return m8?2:3; /* BIT # */
    case 0x24: {u32 ea=ea_dp(s);  if(m8)alu_bit8(c,(u8)c->a,ea_read8(s,ea));else alu_bit16(c,c->a,ea_read16(s,ea)); return m8?3:5;} /* BIT dp */
    case 0x2C: {u32 ea=ea_abs(s); if(m8)alu_bit8(c,(u8)c->a,ea_read8(s,ea));else alu_bit16(c,c->a,ea_read16(s,ea)); return m8?4:5;} /* BIT abs */
    case 0x34: {u32 ea=ea_dpx(s); if(m8)alu_bit8(c,(u8)c->a,ea_read8(s,ea));else alu_bit16(c,c->a,ea_read16(s,ea)); return m8?4:6;} /* BIT dp,X */
    case 0x3C: {u32 ea=ea_absx(s);if(m8)alu_bit8(c,(u8)c->a,ea_read8(s,ea));else alu_bit16(c,c->a,ea_read16(s,ea)); return m8?4:5;} /* BIT abs,X */

    /* ── Branches ───────────────────────────────────────────────────── */
    case 0x90: BRANCH(!c->c); /* BCC */
    case 0xB0: BRANCH(c->c);  /* BCS */
    case 0xF0: BRANCH(c->z);  /* BEQ */
    case 0xD0: BRANCH(!c->z); /* BNE */
    case 0x30: BRANCH(c->n);  /* BMI */
    case 0x10: BRANCH(!c->n); /* BPL */
    case 0x70: BRANCH(c->v);  /* BVS */
    case 0x50: BRANCH(!c->v); /* BVC */
    case 0x80: BRANCH(1);     /* BRA */
    case 0x82: { s16 off=(s16)cpu_fetch16(s); c->pc+=(u16)off; return 4; } /* BRL */

    /* ── Jumps ──────────────────────────────────────────────────────── */
    case 0x4C: c->pc = cpu_fetch16(s); return 3; /* JMP abs */
    case 0x5C: { u32 t=cpu_fetch24(s); c->pbr=(u8)(t>>16); c->pc=(u16)t; return 4; } /* JML long */
    case 0x6C: c->pc = (u16)ea_ind_abs(s); return 5; /* JMP (abs) */
    case 0x7C: c->pc = (u16)ea_ind_absx(s); return 6; /* JMP (abs,X) */
    case 0xDC: { u32 ea=ea_ind_long_dp(s); c->pbr=(u8)(ea>>16); c->pc=(u16)ea; return 6; } /* JML [dp] */

    /* ── Subroutines ────────────────────────────────────────────────── */
    case 0x20: { u16 ea=cpu_fetch16(s); cpu_push16(s,(u16)(c->pc-1)); c->pc=ea; return 6; } /* JSR abs */
    case 0x22: { u32 ea=cpu_fetch24(s); cpu_push8(s,c->pbr); cpu_push16(s,(u16)(c->pc-1)); c->pbr=(u8)(ea>>16); c->pc=(u16)ea; return 8; } /* JSL long */
    case 0xFC: { u16 ea=(u16)ea_ind_absx(s); cpu_push16(s,(u16)(c->pc-1)); c->pc=ea; return 8; } /* JSR (abs,X) */
    case 0x60: c->pc = cpu_pop16(s) + 1; return 6; /* RTS */
    case 0x6B: { c->pc = cpu_pop16(s)+1; c->pbr = cpu_pop8(s); return 6; } /* RTL */
    case 0x40: { /* RTI */
        cpu_set_p(c, cpu_pop8(s));
        c->pc = cpu_pop16(s);
        if (!c->e) c->pbr = cpu_pop8(s);
        return 7;
    }

    /* ── Push / Pull ────────────────────────────────────────────────── */
    case 0x48: if(m8)cpu_push8(s,(u8)c->a);else cpu_push16(s,c->a); return m8?3:4; /* PHA */
    case 0x68: if(m8){c->a=(c->a&0xFF00)|cpu_pop8(s);cpu_nz8(c,(u8)c->a);}else{c->a=cpu_pop16(s);cpu_nz16(c,c->a);} return m8?4:5; /* PLA */
    case 0xDA: if(x8)cpu_push8(s,(u8)c->x);else cpu_push16(s,c->x); return x8?3:4; /* PHX */
    case 0xFA: if(x8){c->x=(c->x&0xFF00)|cpu_pop8(s);cpu_nz8(c,(u8)c->x);}else{c->x=cpu_pop16(s);cpu_nz16(c,c->x);} return x8?4:5; /* PLX */
    case 0x5A: if(x8)cpu_push8(s,(u8)c->y);else cpu_push16(s,c->y); return x8?3:4; /* PHY */
    case 0x7A: if(x8){c->y=(c->y&0xFF00)|cpu_pop8(s);cpu_nz8(c,(u8)c->y);}else{c->y=cpu_pop16(s);cpu_nz16(c,c->y);} return x8?4:5; /* PLY */
    case 0x08: cpu_push8(s,cpu_get_p(c)); return 3; /* PHP */
    case 0x28: cpu_set_p(c,cpu_pop8(s)); return 4; /* PLP */
    case 0x8B: cpu_push8(s,c->dbr); return 3; /* PHB */
    case 0xAB: { c->dbr=cpu_pop8(s); cpu_nz8(c,c->dbr); return 4; } /* PLB */
    case 0x4B: cpu_push8(s,c->pbr); return 3; /* PHK */
    case 0x0B: cpu_push16(s,c->d); return 4; /* PHD */
    case 0x2B: { c->d=cpu_pop16(s); cpu_nz16(c,c->d); return 5; } /* PLD */

    /* ── Flag changes ───────────────────────────────────────────────── */
    case 0x18: c->c=0; return 2; /* CLC */
    case 0x38: c->c=1; return 2; /* SEC */
    case 0x58: c->i=0; return 2; /* CLI */
    case 0x78: c->i=1; return 2; /* SEI */
    case 0xB8: c->v=0; return 2; /* CLV */
    case 0xD8: c->df=0; return 2; /* CLD */
    case 0xF8: c->df=1; return 2; /* SED */
    case 0xFB: { u8 tc=c->c; c->c=c->e; c->e=tc; /* XCE - swap C and E */
        if (c->e) { c->m=1; c->xf=1; c->x&=0xFF; c->y&=0xFF; c->sp=0x0100|(c->sp&0xFF); }
        return 2;
    }
    case 0xC2: { /* REP */ u8 p=cpu_fetch(s); cpu_set_p(c,(u8)(cpu_get_p(c)&~p)); return 3; }
    case 0xE2: { /* SEP */ u8 p=cpu_fetch(s); cpu_set_p(c,(u8)(cpu_get_p(c)|p)); return 3; }

    /* ── Misc ───────────────────────────────────────────────────────── */
    case 0xEA: return 2; /* NOP */
    case 0xCB: c->wai=1; return 3; /* WAI */
    case 0xDB: c->stp=1; return 3; /* STP */
    case 0x00: { /* BRK */
        cpu_fetch(s); /* skip signature byte */
        if (!c->e) cpu_push8(s,c->pbr);
        cpu_push16(s,c->pc);
        cpu_push8(s,(u8)(cpu_get_p(c)|(c->e?0x30:0x30)));
        c->i=1; c->df=0; c->pbr=0;
        c->pc = c->e ? cpu_read16(s,0xFFFE) : cpu_read16(s,0xFFE6);
        return 8;
    }
    case 0x02: { /* COP */
        cpu_fetch(s);
        if (!c->e) cpu_push8(s,c->pbr);
        cpu_push16(s,c->pc);
        cpu_push8(s,cpu_get_p(c));
        c->i=1; c->df=0; c->pbr=0;
        c->pc = c->e ? cpu_read16(s,0xFFF4) : cpu_read16(s,0xFFE4);
        return 8;
    }
    case 0x44: { /* MVP */ u8 dst=cpu_fetch(s),src=cpu_fetch(s); /* simplified */
        c->dbr=dst;
        u8 v=cpu_read(s,((u32)src<<16)|(c->x));
        cpu_write(s,((u32)dst<<16)|(c->y),v);
        if(c->xf){c->x=(c->x-1)&0xFF;c->y=(c->y-1)&0xFF;}else{c->x--;c->y--;}
        if(c->a!=0){if(c->m)c->a=(c->a&0xFF00)|((c->a-1)&0xFF);else c->a--;c->pc-=3;}
        return 7;
    }
    case 0x54: { /* MVN */ u8 dst=cpu_fetch(s),src=cpu_fetch(s);
        c->dbr=dst;
        u8 v=cpu_read(s,((u32)src<<16)|(c->x));
        cpu_write(s,((u32)dst<<16)|(c->y),v);
        if(c->xf){c->x=(c->x+1)&0xFF;c->y=(c->y+1)&0xFF;}else{c->x++;c->y++;}
        if(c->a!=0){if(c->m)c->a=(c->a&0xFF00)|((c->a-1)&0xFF);else c->a--;c->pc-=3;}
        return 7;
    }
    /* TSB / TRB */
    case 0x04: {u32 ea=ea_dp(s);  if(m8){u8 v=ea_read8(s,ea);c->z=!((u8)c->a&v);ea_write8(s,ea,v|(u8)c->a);}else{u16 v=ea_read16(s,ea);c->z=!(c->a&v);ea_write16(s,ea,v|c->a);} return m8?5:7;} /* TSB dp */
    case 0x0C: {u32 ea=ea_abs(s); if(m8){u8 v=ea_read8(s,ea);c->z=!((u8)c->a&v);ea_write8(s,ea,v|(u8)c->a);}else{u16 v=ea_read16(s,ea);c->z=!(c->a&v);ea_write16(s,ea,v|c->a);} return m8?6:8;} /* TSB abs */
    case 0x14: {u32 ea=ea_dp(s);  if(m8){u8 v=ea_read8(s,ea);c->z=!((u8)c->a&v);ea_write8(s,ea,v&~(u8)c->a);}else{u16 v=ea_read16(s,ea);c->z=!(c->a&v);ea_write16(s,ea,v&~c->a);} return m8?5:7;} /* TRB dp */
    case 0x1C: {u32 ea=ea_abs(s); if(m8){u8 v=ea_read8(s,ea);c->z=!((u8)c->a&v);ea_write8(s,ea,v&~(u8)c->a);}else{u16 v=ea_read16(s,ea);c->z=!(c->a&v);ea_write16(s,ea,v&~c->a);} return m8?6:8;} /* TRB abs */

    default: return 2; /* unknown: treat as 2-byte NOP */
    }
#undef BRANCH
#undef LD_IMM8
#undef LD_IMM16
#undef LD_EA8
#undef LD_EA16
#undef ST8
#undef ST16
#undef ASL8
#undef ASL16
#undef LSR8
#undef LSR16
#undef ROL8
#undef ROL16
#undef ROR8
#undef ROR16
#undef ALU_OP
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SNES RESET
 * ═══════════════════════════════════════════════════════════════════════════ */
static void _snes_mem_zero(void *p, u32 n) { u8 *b=p; while(n--)*b++=0; }

static void snes_reset(SNES *s) {
    _snes_mem_zero(&s->cpu, sizeof s->cpu);
    _snes_mem_zero(&s->ppu, sizeof s->ppu);
    _snes_mem_zero(&s->spc, sizeof s->spc);
    _snes_mem_zero(s->wram, SNES_WRAM_SIZE);
    _snes_mem_zero(s->sram, SNES_SRAM_MAX);
    /* 65816 reset state */
    s->cpu.e = 1; s->cpu.m = 1; s->cpu.xf = 1; s->cpu.i = 1;
    s->cpu.sp = 0x01FF;
    /* Read reset vector */
    if (s->rom_size >= 0x8000) {
        s->cpu.pc = (u16)(s->rom[0x7FFC] | ((u16)s->rom[0x7FFD] << 8));
    } else {
        s->cpu.pc = 0x8000;
    }
    /* PPU defaults */
    s->ppu.inidisp = 0x8F;
    s->ppu.vram_inc_step = 1;
    s->ppu.force_blank = 1;
    s->ppu.brightness = 0;
    s->ppu.scanline = 0;
    s->ppu.stat78 = 0x01;
    /* SPC boot ROM enabled */
    s->spc.boot_rom_en = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LIBRETRO API
 * ═══════════════════════════════════════════════════════════════════════════ */
static void core_retro_init(void) {}
static void core_retro_deinit(void) {}
static void core_retro_set_environment(retro_environment_t cb) {
    g_env_cb = cb;
    u32 fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
static void core_retro_set_video_refresh(retro_video_refresh_t cb)      { g_video_cb = cb; }
static void core_retro_set_audio_sample(retro_audio_sample_t cb)        { g_audio_cb = cb; }
static void core_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio_batch_cb = cb; }
static void core_retro_set_input_poll(retro_input_poll_t cb)    { g_input_poll_cb = cb; }
static void core_retro_set_input_state(retro_input_state_t cb)  { g_input_state_cb = cb; }

static void core_retro_get_system_info(struct retro_system_info *info) {
    info->library_name    = "SNES EmuC";
    info->library_version = "1.0";
    info->valid_extensions = "sfc|smc|fig";
    info->need_fullpath   = 0;
    info->block_extract   = 0;
}
static void core_retro_get_system_av_info(struct retro_system_av_info *info) {
    info->geometry.base_width   = SNES_SCREEN_W;
    info->geometry.base_height  = SNES_SCREEN_H;
    info->geometry.max_width    = SNES_SCREEN_W;
    info->geometry.max_height   = SNES_SCREEN_H;
    info->geometry.aspect_ratio = (float)SNES_SCREEN_W / SNES_SCREEN_H;
    info->timing.fps            = (double)SNES_FPS_NUM / SNES_FPS_DEN;
    info->timing.sample_rate    = SNES_SAMPLE_RATE;
}

static int core_retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->data || game->size < 0x8000) return 0;
    SNES *s = &g_snes;
    const u8 *data = (const u8 *)game->data;
    u64 size = game->size;
    /* Strip 512-byte copier header */
    if ((size & 0x7FFF) == 0x200 && size > 0x8200) { data += 0x200; size -= 0x200; }
    if (size > SNES_ROM_MAX) size = SNES_ROM_MAX;
    smem_cpy(s->rom, data, (u32)size);
    s->rom_size = (u32)size;
    /* Detect HiROM by checking internal header checksum complement */
    u8 map_lo = (size > 0x7FD5) ? s->rom[0x7FD5] : 0;
    u8 map_hi = (size > 0xFFD5) ? s->rom[0xFFD5] : 0;
    s->is_hirom = (map_hi & 1) && !(map_lo & 1);
    /* Check for SRAM */
    u8 sram_sz = s->is_hirom ? s->rom[0xFFD8] : s->rom[0x7FD8];
    s->has_sram = (sram_sz > 0 && sram_sz < 8);
    s->sram_size = s->has_sram ? (1u << (sram_sz + 10)) : 0;
    snes_reset(s);
    return 1;
}
static void core_retro_unload_game(void) { g_snes.rom_size = 0; }
static void core_retro_reset(void) { snes_reset(&g_snes); }

#define SNES_AUDIO_BUF 512
static s16 g_snes_abuf[SNES_AUDIO_BUF * 2];

static void core_retro_run(void) {
    SNES *s = &g_snes;
    if (!s->rom_size) return;
    if (g_input_poll_cb) g_input_poll_cb();
    /* Latch joypad state into cpu registers */
    if (g_input_state_cb) {
        u16 j = 0;
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B))      j|=(1<<15);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y))      j|=(1<<14);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT)) j|=(1<<13);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START))  j|=(1<<12);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP))     j|=(1<<11);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN))   j|=(1<<10);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT))   j|=(1<<9);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT))  j|=(1<<8);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A))      j|=(1<<7);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X))      j|=(1<<6);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L))      j|=(1<<5);
        if(g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R))      j|=(1<<4);
        s->cpu.joy_data[0] = j;
    }
    /* Frame: 262 scanlines, ~1364 master cycles each */
    int cyc_scanline = 1364 / 6; /* in CPU cycles (6 master per CPU) */
    int total_lines  = SNES_LINES_PER_FRAME;
    int audio_div    = (total_lines * cyc_scanline) / (SNES_SAMPLE_RATE / SNES_AUDIO_BUF);
    int audio_acc    = 0;
    s->ppu.scanline  = 0;
    s->ppu.dot       = 0;
    hdma_init(s);
    for (int line = 0; line < total_lines; line++) {
        s->ppu.scanline = line;
        /* VBlank logic */
        if (line == SNES_VBLANK_START) {
            s->cpu.in_vblank = 1;
            s->cpu.nmi_occurred = 1;
            if (s->cpu.nmi_enabled) s->cpu.nmi_pending = 1;
            if (g_video_cb)
                g_video_cb(s->ppu.framebuf, SNES_SCREEN_W, SNES_SCREEN_H,
                           (u64)(SNES_SCREEN_W * 4));
        }
        if (line == 0) s->cpu.in_vblank = 0;
        if (line < SNES_SCREEN_H) ppu_run_scanline(s);
        /* CPU for one scanline */
        int cyc = 0;
        while (cyc < cyc_scanline) {
            cyc += cpu_step(s);
            audio_acc++;
            if (audio_acc >= audio_div) {
                audio_acc -= audio_div;
                for (int i=0;i<SNES_AUDIO_BUF;i++)
                    g_snes_abuf[i*2]=g_snes_abuf[i*2+1]=0;
                if (g_audio_batch_cb) g_audio_batch_cb(g_snes_abuf, SNES_AUDIO_BUF);
            }
        }
    }
    s->frame_count++;
}
