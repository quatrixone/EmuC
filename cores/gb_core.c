/* gb_core.c - Game Boy / GBC libretro core for PS5 EmuC
 * Self-contained LR35902 CPU, PPU (BG+sprites), MBC1/3/5, timer, joypad.
 * No libc. Position-independent. _core_start at .text._core_start.
 */
#include "../src/core.h"
#include "../src/libretro.h"

/* ── Forward declarations ──────────────────────────────────────────────── */
static void gb_init(void);
static void gb_deinit(void);
static void gb_set_env(retro_environment_t cb);
static void gb_set_video(retro_video_refresh_t cb);
static void gb_set_audio(retro_audio_sample_t cb);
static void gb_set_audio_batch(retro_audio_sample_batch_t cb);
static void gb_set_input_poll(retro_input_poll_t cb);
static void gb_set_input_state(retro_input_state_t cb);
static void gb_get_sysinfo(struct retro_system_info *i);
static void gb_get_avinfo(struct retro_system_av_info *i);
static int  gb_load_game(const struct retro_game_info *g);
static void gb_unload(void);
static void gb_run(void);
static void gb_reset(void);

/* ── Core entry point ──────────────────────────────────────────────────── */
__attribute__((section(".text._core_start")))
void _core_start(u64 base, struct core_header *out) {
    out->magic = CORE_MAGIC; out->version = CORE_VERSION;
#define OFF(fn) (u32)((u64)(fn) - base)
    out->retro_init_off                  = OFF(gb_init);
    out->retro_deinit_off                = OFF(gb_deinit);
    out->retro_set_environment_off       = OFF(gb_set_env);
    out->retro_set_video_refresh_off     = OFF(gb_set_video);
    out->retro_set_audio_sample_off      = OFF(gb_set_audio);
    out->retro_set_audio_sample_batch_off = OFF(gb_set_audio_batch);
    out->retro_set_input_poll_off        = OFF(gb_set_input_poll);
    out->retro_set_input_state_off       = OFF(gb_set_input_state);
    out->retro_get_system_info_off       = OFF(gb_get_sysinfo);
    out->retro_get_system_av_info_off    = OFF(gb_get_avinfo);
    out->retro_load_game_off             = OFF(gb_load_game);
    out->retro_unload_game_off           = OFF(gb_unload);
    out->retro_run_off                   = OFF(gb_run);
    out->retro_reset_off                 = OFF(gb_reset);
#undef OFF
}

/* ═══════════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════ */
#define GB_W        160
#define GB_H        144
#define GB_FPS_NUM  4194304   /* ~59.7275 fps (CPU clock / 70224 cycles) */
#define GB_FPS_DEN  70224
#define GB_SR       44100

/* DMG palette (XRGB8888) */
static const u32 DMG_PAL[4] = { 0xE0F8D0, 0x88C070, 0x346856, 0x081820 };

/* Memory region sizes */
#define WRAM_SIZE  0x2000
#define VRAM_SIZE  0x2000
#define OAM_SIZE   0xA0
#define HRAM_SIZE  0x7F
#define ROM_MAX    (8*1024*1024)

/* ═══════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════ */
static retro_video_refresh_t    g_video_cb;
static retro_audio_sample_t     g_audio_cb;
static retro_audio_sample_batch_t g_abatch_cb;
static retro_input_poll_t       g_input_poll_cb;
static retro_input_state_t      g_input_state_cb;
static retro_environment_t      g_env_cb;

/* ROM / RAM */
static const u8 *g_rom;
static u64       g_rom_size;
static u8        g_eram[0x8000];  /* up to 32 KB external RAM */
static u8        g_wram[WRAM_SIZE];
static u8        g_vram[VRAM_SIZE];
static u8        g_oam[OAM_SIZE];
static u8        g_hram[HRAM_SIZE];
static u8        g_ie;

/* MBC state */
static int g_mbc;          /* 0=none, 1=MBC1, 3=MBC3, 5=MBC5 */
static u8  g_rom_bank;
static u8  g_ram_bank;
static int g_ram_enable;

/* CPU */
typedef struct {
    u8  a, f, b, c, d, e, h, l;
    u16 sp, pc;
    int ime;       /* interrupt master enable */
    int halted;
    int stopped;
} LR35902;
static LR35902 g_cpu;

/* Flag helpers */
#define F_Z  0x80
#define F_N  0x40
#define F_H  0x20
#define F_C  0x10
#define SET_Z(v) do { if(v) g_cpu.f|=F_Z; else g_cpu.f&=~F_Z; } while(0)
#define SET_N(v) do { if(v) g_cpu.f|=F_N; else g_cpu.f&=~F_N; } while(0)
#define SET_H(v) do { if(v) g_cpu.f|=F_H; else g_cpu.f&=~F_H; } while(0)
#define SET_C(v) do { if(v) g_cpu.f|=F_C; else g_cpu.f&=~F_C; } while(0)
#define GET_Z  ((g_cpu.f>>7)&1)
#define GET_N  ((g_cpu.f>>6)&1)
#define GET_H  ((g_cpu.f>>5)&1)
#define GET_C  ((g_cpu.f>>4)&1)
#define HL() ((u16)(((u16)g_cpu.h<<8)|g_cpu.l))
#define BC() ((u16)(((u16)g_cpu.b<<8)|g_cpu.c))
#define DE() ((u16)(((u16)g_cpu.d<<8)|g_cpu.e))
#define SET_HL(v) do{g_cpu.h=(u8)((v)>>8);g_cpu.l=(u8)(v);}while(0)
#define SET_BC(v) do{g_cpu.b=(u8)((v)>>8);g_cpu.c=(u8)(v);}while(0)
#define SET_DE(v) do{g_cpu.d=(u8)((v)>>8);g_cpu.e=(u8)(v);}while(0)

/* PPU */
static u8  g_lcdc, g_stat, g_scy, g_scx, g_ly, g_lyc;
static u8  g_bgp, g_obp0, g_obp1, g_wy, g_wx;
static int g_ppu_dots;    /* dots in current scanline */
static u32 g_fb[GB_W * GB_H];

/* Timer */
static u8  g_div_reg, g_tima, g_tma, g_tac;
static int g_div_cycles, g_timer_cycles;

/* Interrupt flags */
static u8 g_if;  /* interrupt flags at 0xFF0F */

/* Joypad */
static u8 g_joy_sel;  /* 0xFF00 */

/* Audio simple synthesis */
static int g_audio_cycles;
static int g_audio_phase;

/* Pixel framebuffer for output (XRGB8888) */
static u32 g_out_fb[GB_W * GB_H];

/* ═══════════════════════════════════════════════════════════════════════
 * MEMORY BUS
 * ═══════════════════════════════════════════════════════════════════════ */
static u8 mem_r(u16 addr) {
    if (addr < 0x4000) {
        return g_rom ? g_rom[addr] : 0xFF;
    }
    if (addr < 0x8000) {
        u32 off = (u32)(g_rom_bank ? g_rom_bank : 1) * 0x4000 + (addr - 0x4000);
        if (g_rom && off < g_rom_size) return g_rom[off];
        return 0xFF;
    }
    if (addr < 0xA000) return g_vram[addr - 0x8000];
    if (addr < 0xC000) {
        if (!g_ram_enable) return 0xFF;
        u32 off = (u32)g_ram_bank * 0x2000 + (addr - 0xA000);
        if (off < sizeof(g_eram)) return g_eram[off];
        return 0xFF;
    }
    if (addr < 0xE000) return g_wram[addr - 0xC000];
    if (addr < 0xFE00) return g_wram[(addr - 0xE000) & (WRAM_SIZE-1)];
    if (addr < 0xFEA0) return g_oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;
    /* I/O */
    switch (addr) {
    case 0xFF00: {
        u8 r = 0xCF;
        /* read buttons */
        if (!(g_joy_sel & 0x10)) { /* direction keys */
            u8 d = 0xF;
            if (g_input_state_cb) {
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT)) d&=~1;
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT))  d&=~2;
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP))    d&=~4;
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN))  d&=~8;
            }
            r = (r & 0xF0) | (d & 0xF);
        }
        if (!(g_joy_sel & 0x20)) { /* buttons */
            u8 b = 0xF;
            if (g_input_state_cb) {
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A))      b&=~1;
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B))      b&=~2;
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT)) b&=~4;
                if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START))  b&=~8;
            }
            r = (r & 0xF0) | (b & 0xF);
        }
        return r;
    }
    case 0xFF04: return g_div_reg;
    case 0xFF05: return g_tima;
    case 0xFF06: return g_tma;
    case 0xFF07: return g_tac;
    case 0xFF0F: return g_if;
    case 0xFF40: return g_lcdc;
    case 0xFF41: return g_stat;
    case 0xFF42: return g_scy;
    case 0xFF43: return g_scx;
    case 0xFF44: return g_ly;
    case 0xFF45: return g_lyc;
    case 0xFF47: return g_bgp;
    case 0xFF48: return g_obp0;
    case 0xFF49: return g_obp1;
    case 0xFF4A: return g_wy;
    case 0xFF4B: return g_wx;
    case 0xFFFF: return g_ie;
    default:
        if (addr >= 0xFF80 && addr < 0xFFFF)
            return g_hram[addr - 0xFF80];
        return 0xFF;
    }
}

static void mem_w(u16 addr, u8 v) {
    if (addr < 0x2000) {
        g_ram_enable = ((v & 0xF) == 0xA);
        return;
    }
    if (addr < 0x4000) {
        if (g_mbc == 1) g_rom_bank = v & 0x1F;
        else if (g_mbc == 3) g_rom_bank = v & 0x7F;
        else if (g_mbc == 5) g_rom_bank = v & 0xFF;
        if (!g_rom_bank) g_rom_bank = 1;
        return;
    }
    if (addr < 0x6000) {
        if (g_mbc == 1) g_ram_bank = v & 3;
        else if (g_mbc == 3) g_ram_bank = v & 3;
        return;
    }
    if (addr < 0x8000) return;
    if (addr < 0xA000) { g_vram[addr - 0x8000] = v; return; }
    if (addr < 0xC000) {
        if (!g_ram_enable) return;
        u32 off = (u32)g_ram_bank * 0x2000 + (addr - 0xA000);
        if (off < sizeof(g_eram)) g_eram[off] = v;
        return;
    }
    if (addr < 0xE000) { g_wram[addr - 0xC000] = v; return; }
    if (addr < 0xFE00) { g_wram[(addr-0xE000)&(WRAM_SIZE-1)] = v; return; }
    if (addr < 0xFEA0) { g_oam[addr - 0xFE00] = v; return; }
    if (addr < 0xFF00) return;
    /* I/O */
    switch (addr) {
    case 0xFF00: g_joy_sel = v; return;
    case 0xFF04: g_div_reg = 0; g_div_cycles = 0; return;
    case 0xFF05: g_tima = v; return;
    case 0xFF06: g_tma = v; return;
    case 0xFF07: g_tac = v; return;
    case 0xFF0F: g_if = v; return;
    case 0xFF40: g_lcdc = v; return;
    case 0xFF41: g_stat = (g_stat & 0x7) | (v & 0xF8); return;
    case 0xFF42: g_scy = v; return;
    case 0xFF43: g_scx = v; return;
    case 0xFF44: g_ly = 0; return;
    case 0xFF45: g_lyc = v; return;
    case 0xFF46: { /* DMA */
        u16 src = (u16)v << 8;
        for (int i = 0; i < 0xA0; i++) g_oam[i] = mem_r(src + i);
        return;
    }
    case 0xFF47: g_bgp = v; return;
    case 0xFF48: g_obp0 = v; return;
    case 0xFF49: g_obp1 = v; return;
    case 0xFF4A: g_wy = v; return;
    case 0xFF4B: g_wx = v; return;
    case 0xFFFF: g_ie = v; return;
    default:
        if (addr >= 0xFF80 && addr < 0xFFFF)
            g_hram[addr - 0xFF80] = v;
        return;
    }
}

static u8  fetch8(void)  { return mem_r(g_cpu.pc++); }
static u16 fetch16(void) { u16 l=fetch8(); return l|(((u16)fetch8())<<8); }

static void push16(u16 v) {
    g_cpu.sp--; mem_w(g_cpu.sp, (u8)(v>>8));
    g_cpu.sp--; mem_w(g_cpu.sp, (u8)v);
}
static u16 pop16(void) {
    u16 l = mem_r(g_cpu.sp++);
    return l | ((u16)mem_r(g_cpu.sp++) << 8);
}

/* ═══════════════════════════════════════════════════════════════════════
 * PPU – scanline renderer
 * ═══════════════════════════════════════════════════════════════════════ */
static u32 dmg_color(u8 pal, u8 idx) {
    return DMG_PAL[(pal >> (idx * 2)) & 3];
}

static void ppu_render_line(void) {
    if (g_ly >= GB_H) return;
    u32 *row = &g_out_fb[g_ly * GB_W];

    /* Background */
    if (g_lcdc & 0x01) {
        u16 map_base  = (g_lcdc & 0x08) ? 0x9C00 : 0x9800;
        u16 tile_base = (g_lcdc & 0x10) ? 0x8000 : 0x9000;
        int signed_tiles = !(g_lcdc & 0x10);
        u8  py = (u8)(g_ly + g_scy);
        for (int x = 0; x < GB_W; x++) {
            u8 px = (u8)(x + g_scx);
            u16 tile_addr = map_base + (py/8)*32 + (px/8);
            u8  tile_idx  = g_vram[tile_addr - 0x8000];
            u16 tdata;
            if (signed_tiles) {
                tdata = (u16)((int)tile_base + (s8)tile_idx * 16);
            } else {
                tdata = tile_base + tile_idx * 16;
            }
            u16 row_addr = tdata + (py & 7) * 2 - 0x8000;
            u8 lo = g_vram[row_addr], hi = g_vram[row_addr+1];
            u8 bit = 7 - (px & 7);
            u8 col = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            row[x] = dmg_color(g_bgp, col);
        }
    } else {
        for (int x = 0; x < GB_W; x++) row[x] = DMG_PAL[0];
    }

    /* Sprites */
    if (g_lcdc & 0x02) {
        int sh = (g_lcdc & 0x04) ? 16 : 8;
        for (int s = 39; s >= 0; s--) {
            int oy = (int)g_oam[s*4+0] - 16;
            int ox = (int)g_oam[s*4+1] - 8;
            u8  ti = g_oam[s*4+2];
            u8  at = g_oam[s*4+3];
            if (g_ly < oy || g_ly >= oy + sh) continue;
            if (ox < -7 || ox >= GB_W) continue;
            int row_off = g_ly - oy;
            if (at & 0x40) row_off = (sh - 1) - row_off;
            if (sh == 16) ti &= 0xFE;
            u16 ta = 0x8000 + ti * 16 + row_off * 2 - 0x8000;
            u8 lo = g_vram[ta], hi = g_vram[ta+1];
            u8 pal = (at & 0x10) ? g_obp1 : g_obp0;
            for (int px = 0; px < 8; px++) {
                int sx = ox + ((at & 0x20) ? px : (7 - px));
                if (sx < 0 || sx >= GB_W) continue;
                u8 col = ((lo >> (7-px)) & 1) | (((hi >> (7-px)) & 1) << 1);
                if (col == 0) continue;
                row[sx] = dmg_color(pal, col);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * CPU – LR35902 instruction decode
 * ═══════════════════════════════════════════════════════════════════════ */
static u8 *reg8[8];  /* B C D E H L (HL) A */

static void init_reg_table(void) {
    reg8[0] = &g_cpu.b; reg8[1] = &g_cpu.c;
    reg8[2] = &g_cpu.d; reg8[3] = &g_cpu.e;
    reg8[4] = &g_cpu.h; reg8[5] = &g_cpu.l;
    reg8[6] = 0;        /* (HL) handled separately */
    reg8[7] = &g_cpu.a;
}

static u8 get_r(int r) {
    if (r == 6) return mem_r(HL()); return *reg8[r];
}
static void set_r(int r, u8 v) {
    if (r == 6) { mem_w(HL(), v); return; } *reg8[r] = v;
}

/* ALU helpers */
static void alu_add(u8 v, int cy) {
    u16 r = g_cpu.a + v + cy;
    SET_H(((g_cpu.a & 0xF) + (v & 0xF) + cy) > 0xF);
    SET_C(r > 0xFF); g_cpu.a = (u8)r;
    SET_Z(!g_cpu.a); SET_N(0);
}
static void alu_sub(u8 v, int cy) {
    int r = (int)g_cpu.a - v - cy;
    SET_H(((int)(g_cpu.a & 0xF) - (v & 0xF) - cy) < 0);
    SET_C(r < 0); g_cpu.a = (u8)r;
    SET_Z(!g_cpu.a); SET_N(1);
}
static void alu_and(u8 v) { g_cpu.a &= v; SET_Z(!g_cpu.a); g_cpu.f=(g_cpu.f&~(F_N|F_C))|F_H; }
static void alu_xor(u8 v) { g_cpu.a ^= v; SET_Z(!g_cpu.a); g_cpu.f &= ~(F_N|F_H|F_C); }
static void alu_or(u8 v)  { g_cpu.a |= v; SET_Z(!g_cpu.a); g_cpu.f &= ~(F_N|F_H|F_C); }
static void alu_cp(u8 v)  { u8 a=g_cpu.a; alu_sub(v,0); g_cpu.a=a; }
static void alu_inc(u8 *p) {
    SET_H((*p & 0xF) == 0xF); (*p)++;
    SET_Z(!*p); SET_N(0);
}
static void alu_dec(u8 *p) {
    SET_H((*p & 0xF) == 0); (*p)--;
    SET_Z(!*p); SET_N(1);
}
static void alu_add_hl(u16 v) {
    u32 r = HL() + v;
    SET_H(((HL()&0xFFF)+(v&0xFFF))>0xFFF);
    SET_C(r > 0xFFFF); SET_N(0);
    SET_HL((u16)r);
}

/* CB prefix */
static int cb_exec(void) {
    u8 op = fetch8(); int r = op & 7; u8 v = get_r(r);
    int bit = (op >> 3) & 7;
    if (op < 0x40) {
        u8 res;
        switch (op >> 3) {
        case 0: /* RLC */ res=(v<<1)|(v>>7); SET_C(v>>7); SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        case 1: /* RRC */ res=(v>>1)|(v<<7); SET_C(v&1);  SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        case 2: /* RL  */ res=(v<<1)|GET_C; SET_C(v>>7); SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        case 3: /* RR  */ res=(v>>1)|(GET_C<<7); SET_C(v&1); SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        case 4: /* SLA */ res=v<<1; SET_C(v>>7); SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        case 5: /* SRA */ res=(v>>1)|(v&0x80); SET_C(v&1); SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        case 6: /* SWAP */ res=(v>>4)|(v<<4); g_cpu.f=0; SET_Z(!res); set_r(r,res); break;
        case 7: /* SRL */ res=v>>1; SET_C(v&1); SET_H(0); SET_N(0); SET_Z(!res); set_r(r,res); break;
        default: break;
        }
    } else if (op < 0x80) { /* BIT */
        SET_Z(!((v>>bit)&1)); SET_N(0); SET_H(1);
    } else if (op < 0xC0) { /* RES */
        set_r(r, v & ~(1<<bit));
    } else { /* SET */
        set_r(r, v | (1<<bit));
    }
    return (r == 6) ? 16 : 8;
}

/* Returns cycles used */
static int cpu_step(void) {
    if (g_cpu.halted) {
        if (g_if & g_ie & 0x1F) g_cpu.halted = 0;
        else return 4;
    }
    /* Handle interrupts */
    if (g_cpu.ime) {
        u8 pending = g_if & g_ie & 0x1F;
        if (pending) {
            g_cpu.ime = 0;
            u8 bit = 0; u8 p = pending;
            while (!(p & 1)) { p >>= 1; bit++; }
            g_if &= ~(1 << bit);
            push16(g_cpu.pc);
            g_cpu.pc = 0x0040 + bit * 8;
            return 20;
        }
    }
    u8 op = fetch8();
    /* Decode */
    if (op == 0x00) return 4; /* NOP */
    if (op == 0xCB) return cb_exec();
    if (op == 0x76) { g_cpu.halted = 1; return 4; }
    if (op == 0x10) { fetch8(); g_cpu.stopped = 1; return 4; }
    if (op == 0xF3) { g_cpu.ime = 0; return 4; }
    if (op == 0xFB) { g_cpu.ime = 1; return 4; }
    if (op == 0xE9) { g_cpu.pc = HL(); return 4; }
    if (op == 0xF9) { g_cpu.sp = HL(); return 8; }
    /* LD r16, d16 */
    if ((op & 0xCF) == 0x01) {
        u16 v = fetch16();
        switch ((op>>4)&3) {
        case 0: SET_BC(v); break; case 1: SET_DE(v); break;
        case 2: SET_HL(v); break; case 3: g_cpu.sp=v; break;
        }
        return 12;
    }
    /* LD (r16), A / LD A, (r16) */
    if (op == 0x02) { mem_w(BC(), g_cpu.a); return 8; }
    if (op == 0x12) { mem_w(DE(), g_cpu.a); return 8; }
    if (op == 0x22) { mem_w(HL(), g_cpu.a); SET_HL(HL()+1); return 8; }
    if (op == 0x32) { mem_w(HL(), g_cpu.a); SET_HL(HL()-1); return 8; }
    if (op == 0x0A) { g_cpu.a = mem_r(BC()); return 8; }
    if (op == 0x1A) { g_cpu.a = mem_r(DE()); return 8; }
    if (op == 0x2A) { g_cpu.a = mem_r(HL()); SET_HL(HL()+1); return 8; }
    if (op == 0x3A) { g_cpu.a = mem_r(HL()); SET_HL(HL()-1); return 8; }
    /* LD (a16), SP */
    if (op == 0x08) { u16 a=fetch16(); mem_w(a,(u8)g_cpu.sp); mem_w(a+1,(u8)(g_cpu.sp>>8)); return 20; }
    /* INC/DEC r16 */
    if ((op&0xCF)==0x03) { u16 rr; switch((op>>4)&3){case 0:rr=BC()+1;SET_BC(rr);break;case 1:rr=DE()+1;SET_DE(rr);break;case 2:rr=HL()+1;SET_HL(rr);break;case 3:g_cpu.sp++;break;} return 8; }
    if ((op&0xCF)==0x0B) { u16 rr; switch((op>>4)&3){case 0:rr=BC()-1;SET_BC(rr);break;case 1:rr=DE()-1;SET_DE(rr);break;case 2:rr=HL()-1;SET_HL(rr);break;case 3:g_cpu.sp--;break;} return 8; }
    /* INC/DEC r8 */
    if ((op&0x7)==0x4 && (op>>3)<8) { int r=(op>>3)&7; if(r==6){u8 v=mem_r(HL());alu_inc(&v);mem_w(HL(),v);}else alu_inc(reg8[r]); return (r==6)?12:4; }
    if ((op&0x7)==0x5 && (op>>3)<8) { int r=(op>>3)&7; if(r==6){u8 v=mem_r(HL());alu_dec(&v);mem_w(HL(),v);}else alu_dec(reg8[r]); return (r==6)?12:4; }
    /* LD r, d8 */
    if ((op&0x7)==0x6) { int r=(op>>3)&7; u8 v=fetch8(); set_r(r,v); return (r==6)?12:8; }
    /* ADD HL, r16 */
    if ((op&0xCF)==0x09) { switch((op>>4)&3){case 0:alu_add_hl(BC());break;case 1:alu_add_hl(DE());break;case 2:alu_add_hl(HL());break;case 3:alu_add_hl(g_cpu.sp);break;} return 8; }
    /* RLCA RRCA RLA RRA */
    if (op==0x07){u8 c=g_cpu.a>>7;g_cpu.a=(g_cpu.a<<1)|c;g_cpu.f=c?F_C:0;return 4;}
    if (op==0x0F){u8 c=g_cpu.a&1;g_cpu.a=(g_cpu.a>>1)|(c<<7);g_cpu.f=c?F_C:0;return 4;}
    if (op==0x17){u8 c=GET_C;u8 nc=g_cpu.a>>7;g_cpu.a=(g_cpu.a<<1)|c;g_cpu.f=nc?F_C:0;return 4;}
    if (op==0x1F){u8 c=GET_C;u8 nc=g_cpu.a&1;g_cpu.a=(g_cpu.a>>1)|(c<<7);g_cpu.f=nc?F_C:0;return 4;}
    /* DAA */
    if (op==0x27){
        int a=g_cpu.a; int n=GET_N, c=GET_C, h=GET_H;
        if(!n){if(h||(a&0xF)>9)a+=6;if(c||a>0x9F)a+=0x60;}
        else{if(h)a-=6;if(c)a-=0x60;}
        SET_C((a&0x100)?1:0); g_cpu.a=(u8)a; SET_Z(!g_cpu.a); SET_H(0); return 4;
    }
    if (op==0x2F){g_cpu.a=~g_cpu.a; SET_N(1); SET_H(1); return 4;}
    if (op==0x37){SET_C(1); SET_N(0); SET_H(0); return 4;}
    if (op==0x3F){SET_C(!GET_C); SET_N(0); SET_H(0); return 4;}
    /* LD r, r block (0x40-0x7F) */
    if (op>=0x40 && op<0x80) { set_r((op>>3)&7, get_r(op&7)); return (((op>>3)&7)==6||(op&7)==6)?8:4; }
    /* ALU block (0x80-0xBF) */
    if (op>=0x80 && op<0xC0) {
        u8 v = get_r(op&7);
        int cyc = (op&7)==6 ? 8 : 4;
        switch ((op>>3)&7) {
        case 0:alu_add(v,0);break; case 1:alu_add(v,GET_C);break;
        case 2:alu_sub(v,0);break; case 3:alu_sub(v,GET_C);break;
        case 4:alu_and(v);break;   case 5:alu_xor(v);break;
        case 6:alu_or(v);break;    case 7:alu_cp(v);break;
        }
        return cyc;
    }
    /* ALU immediate (0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE) */
    if ((op&0x7)==0x6 && (op>>6)==3) {
        u8 v=fetch8();
        switch ((op>>3)&7) {
        case 0:alu_add(v,0);break; case 1:alu_add(v,GET_C);break;
        case 2:alu_sub(v,0);break; case 3:alu_sub(v,GET_C);break;
        case 4:alu_and(v);break;   case 5:alu_xor(v);break;
        case 6:alu_or(v);break;    case 7:alu_cp(v);break;
        }
        return 8;
    }
    /* PUSH / POP */
    if ((op&0xCF)==0xC5){u16 rr; switch((op>>4)&3){case 0:rr=BC();break;case 1:rr=DE();break;case 2:rr=HL();break;default:rr=(((u16)g_cpu.a<<8)|(g_cpu.f&0xF0));break;} push16(rr); return 16;}
    if ((op&0xCF)==0xC1){u16 v=pop16(); switch((op>>4)&3){case 0:SET_BC(v);break;case 1:SET_DE(v);break;case 2:SET_HL(v);break;default:g_cpu.a=(u8)(v>>8);g_cpu.f=(u8)(v&0xF0);break;} return 12;}
    /* JP / JR */
    if (op==0xC3){g_cpu.pc=fetch16();return 16;}
    if (op==0xE9){g_cpu.pc=HL();return 4;}
    if (op==0x18){s8 d=(s8)fetch8();g_cpu.pc+=(s16)d;return 12;}
    /* JR cc */
    if ((op&0xE7)==0x20){s8 d=(s8)fetch8();int t=(op>>3)&3;int cond=t==0?!GET_Z:t==1?GET_Z:t==2?!GET_C:GET_C;if(cond){g_cpu.pc+=(s16)d;return 12;}return 8;}
    /* JP cc */
    if ((op&0xE7)==0xC2){u16 a=fetch16();int t=(op>>3)&3;int cond=t==0?!GET_Z:t==1?GET_Z:t==2?!GET_C:GET_C;if(cond){g_cpu.pc=a;return 16;}return 12;}
    /* CALL */
    if (op==0xCD){u16 a=fetch16();push16(g_cpu.pc);g_cpu.pc=a;return 24;}
    /* CALL cc */
    if ((op&0xE7)==0xC4){u16 a=fetch16();int t=(op>>3)&3;int cond=t==0?!GET_Z:t==1?GET_Z:t==2?!GET_C:GET_C;if(cond){push16(g_cpu.pc);g_cpu.pc=a;return 24;}return 12;}
    /* RET */
    if (op==0xC9){g_cpu.pc=pop16();return 16;}
    if (op==0xD9){g_cpu.pc=pop16();g_cpu.ime=1;return 16;}
    /* RET cc */
    if ((op&0xE7)==0xC0){int t=(op>>3)&3;int cond=t==0?!GET_Z:t==1?GET_Z:t==2?!GET_C:GET_C;if(cond){g_cpu.pc=pop16();return 20;}return 8;}
    /* RST */
    if ((op&0xC7)==0xC7){push16(g_cpu.pc);g_cpu.pc=(op&0x38);return 16;}
    /* LD (C), A / LD A, (C) */
    if (op==0xE2){mem_w(0xFF00+g_cpu.c,g_cpu.a);return 8;}
    if (op==0xF2){g_cpu.a=mem_r(0xFF00+g_cpu.c);return 8;}
    /* LDH (a8), A / LDH A, (a8) */
    if (op==0xE0){u8 d=fetch8();mem_w(0xFF00+d,g_cpu.a);return 12;}
    if (op==0xF0){u8 d=fetch8();g_cpu.a=mem_r(0xFF00+d);return 12;}
    /* LD (a16), A / LD A, (a16) */
    if (op==0xEA){u16 a=fetch16();mem_w(a,g_cpu.a);return 16;}
    if (op==0xFA){u16 a=fetch16();g_cpu.a=mem_r(a);return 16;}
    /* ADD SP, r8 */
    if (op==0xE8){s8 d=(s8)fetch8();u32 r=(u32)g_cpu.sp+(u16)(s16)d;SET_H(((g_cpu.sp^d^r)&0x10)!=0);SET_C(((g_cpu.sp^d^r)&0x100)!=0);SET_Z(0);SET_N(0);g_cpu.sp=(u16)r;return 16;}
    /* LD HL, SP+r8 */
    if (op==0xF8){s8 d=(s8)fetch8();u32 r=(u32)g_cpu.sp+(u16)(s16)d;SET_H(((g_cpu.sp^d^r)&0x10)!=0);SET_C(((g_cpu.sp^d^r)&0x100)!=0);SET_Z(0);SET_N(0);SET_HL((u16)r);return 12;}
    /* Unknown / unused: treat as NOP */
    return 4;
}

/* ═══════════════════════════════════════════════════════════════════════
 * TIMER
 * ═══════════════════════════════════════════════════════════════════════ */
static void timer_tick(int cycles) {
    g_div_cycles += cycles;
    while (g_div_cycles >= 256) { g_div_reg++; g_div_cycles -= 256; }
    if (!(g_tac & 4)) return;
    int thresh;
    switch (g_tac & 3) {
    case 0: thresh=1024; break; case 1: thresh=16;   break;
    case 2: thresh=64;   break; default: thresh=256; break;
    }
    g_timer_cycles += cycles;
    while (g_timer_cycles >= thresh) {
        g_timer_cycles -= thresh;
        g_tima++;
        if (!g_tima) { g_tima = g_tma; g_if |= 4; }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * AUDIO – simple square wave silence (avoids pure-tone artifacts)
 * ═══════════════════════════════════════════════════════════════════════ */
#define AUDIO_BUF 512
static s16 g_abuf[AUDIO_BUF * 2];

static void audio_flush(void) {
    for (int i = 0; i < AUDIO_BUF; i++) {
        g_abuf[i*2] = g_abuf[i*2+1] = 0;
    }
    if (g_abatch_cb) g_abatch_cb(g_abuf, AUDIO_BUF);
}

/* ═══════════════════════════════════════════════════════════════════════
 * STATIC INIT
 * ═══════════════════════════════════════════════════════════════════════ */
static void mem_zero(void *p, u64 n) {
    u8 *b = p; while (n--) *b++ = 0;
}
static void mem_copy(void *d, const void *s, u64 n) {
    u8 *dd=d; const u8 *ss=s; while(n--) *dd++=*ss++;
}

static void gb_reset_state(void) {
    mem_zero(&g_cpu, sizeof g_cpu);
    mem_zero(g_wram, sizeof g_wram);
    mem_zero(g_vram, sizeof g_vram);
    mem_zero(g_oam,  sizeof g_oam);
    mem_zero(g_hram, sizeof g_hram);
    mem_zero(g_eram, sizeof g_eram);
    mem_zero(g_out_fb, sizeof g_out_fb);
    /* Boot state after DMG boot ROM */
    g_cpu.a=0x01; g_cpu.f=0xB0;
    g_cpu.b=0x00; g_cpu.c=0x13;
    g_cpu.d=0x00; g_cpu.e=0xD8;
    g_cpu.h=0x01; g_cpu.l=0x4D;
    g_cpu.sp=0xFFFE; g_cpu.pc=0x0100;
    g_lcdc=0x91; g_bgp=0xFC; g_obp0=0xFF; g_obp1=0xFF;
    g_ly=0; g_scy=0; g_scx=0; g_lyc=0;
    g_stat=0x85;
    g_div_reg=0xAB; g_tima=0; g_tma=0; g_tac=0;
    g_if=0xE1; g_ie=0;
    g_ppu_dots=0;
    g_rom_bank=1; g_ram_bank=0; g_ram_enable=0;
    init_reg_table();
}

/* ═══════════════════════════════════════════════════════════════════════
 * LIBRETRO API
 * ═══════════════════════════════════════════════════════════════════════ */
static void gb_init(void) { init_reg_table(); }
static void gb_deinit(void) {}
static void gb_set_env(retro_environment_t cb) {
    g_env_cb = cb;
    u32 fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
static void gb_set_video(retro_video_refresh_t cb)      { g_video_cb = cb; }
static void gb_set_audio(retro_audio_sample_t cb)       { g_audio_cb = cb; }
static void gb_set_audio_batch(retro_audio_sample_batch_t cb) { g_abatch_cb = cb; }
static void gb_set_input_poll(retro_input_poll_t cb)    { g_input_poll_cb = cb; }
static void gb_set_input_state(retro_input_state_t cb)  { g_input_state_cb = cb; }

static void gb_get_sysinfo(struct retro_system_info *i) {
    i->library_name    = "GB EmuC";
    i->library_version = "1.0";
    i->valid_extensions = "gb|gbc";
    i->need_fullpath   = 0;
    i->block_extract   = 0;
}
static void gb_get_avinfo(struct retro_system_av_info *i) {
    i->geometry.base_width   = GB_W;
    i->geometry.base_height  = GB_H;
    i->geometry.max_width    = GB_W;
    i->geometry.max_height   = GB_H;
    i->geometry.aspect_ratio = (float)GB_W / GB_H;
    i->timing.fps            = (double)GB_FPS_NUM / GB_FPS_DEN;
    i->timing.sample_rate    = GB_SR;
}

static int gb_load_game(const struct retro_game_info *g) {
    if (!g || !g->data || g->size < 0x150) return 0;
    g_rom      = (const u8 *)g->data;
    g_rom_size = g->size;
    /* Detect MBC from cart type byte at 0x147 */
    u8 ct = g_rom[0x147];
    if (ct==0) g_mbc=0;
    else if (ct<=3) g_mbc=1;
    else if (ct>=0x0F && ct<=0x13) g_mbc=3;
    else if (ct>=0x19 && ct<=0x1E) g_mbc=5;
    else g_mbc=1;
    gb_reset_state();
    return 1;
}
static void gb_unload(void) { g_rom=0; g_rom_size=0; }
static void gb_reset(void)  { gb_reset_state(); }

static void gb_run(void) {
    if (g_input_poll_cb) g_input_poll_cb();
    /* Run one frame = 70224 cycles at 4 MHz */
    int frame_cycles = 70224;
    int audio_accum  = 0;
    while (frame_cycles > 0) {
        int c = cpu_step();
        frame_cycles -= c;
        timer_tick(c);
        /* PPU timing: 456 dots per scanline */
        g_ppu_dots += c;
        if (g_ppu_dots >= 456) {
            g_ppu_dots -= 456;
            if (g_ly < GB_H) ppu_render_line();
            g_ly++;
            if (g_ly == GB_H) {
                /* VBlank */
                g_if |= 1;
                g_stat = (g_stat & ~3) | 1;
                if (g_video_cb)
                    g_video_cb(g_out_fb, GB_W, GB_H, (u64)(GB_W * 4));
            } else if (g_ly > 153) {
                g_ly = 0;
                g_stat = (g_stat & ~3) | 2;
            }
        }
        audio_accum += c;
        if (audio_accum >= 4194304 / (GB_SR / AUDIO_BUF)) {
            audio_accum -= 4194304 / (GB_SR / AUDIO_BUF);
            audio_flush();
        }
    }
}
