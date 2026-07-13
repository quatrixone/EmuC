/* gba_core.c - Game Boy Advance libretro core for PS5 EmuC
 * ARM7TDMI CPU (ARM32 + THUMB), PPU modes 0/3/4, DMA, timers.
 * No libc. Position-independent. _core_start at .text._core_start.
 */
#include "../src/core.h"
#include "../src/libretro.h"

/* ── Forward declarations ──────────────────────────────────────────────── */
static void gba_init(void);
static void gba_deinit(void);
static void gba_set_env(retro_environment_t cb);
static void gba_set_video(retro_video_refresh_t cb);
static void gba_set_audio(retro_audio_sample_t cb);
static void gba_set_audio_batch(retro_audio_sample_batch_t cb);
static void gba_set_input_poll(retro_input_poll_t cb);
static void gba_set_input_state(retro_input_state_t cb);
static void gba_get_sysinfo(struct retro_system_info *i);
static void gba_get_avinfo(struct retro_system_av_info *i);
static int  gba_load_game(const struct retro_game_info *g);
static void gba_unload(void);
static void gba_run(void);
static void gba_reset(void);

/* ── Core entry point ──────────────────────────────────────────────────── */
__attribute__((section(".text._core_start")))
void _core_start(u64 base, struct core_header *out) {
    out->magic = CORE_MAGIC; out->version = CORE_VERSION;
#define OFF(fn) (u32)((u64)(fn) - base)
    out->retro_init_off                   = OFF(gba_init);
    out->retro_deinit_off                 = OFF(gba_deinit);
    out->retro_set_environment_off        = OFF(gba_set_env);
    out->retro_set_video_refresh_off      = OFF(gba_set_video);
    out->retro_set_audio_sample_off       = OFF(gba_set_audio);
    out->retro_set_audio_sample_batch_off = OFF(gba_set_audio_batch);
    out->retro_set_input_poll_off         = OFF(gba_set_input_poll);
    out->retro_set_input_state_off        = OFF(gba_set_input_state);
    out->retro_get_system_info_off        = OFF(gba_get_sysinfo);
    out->retro_get_system_av_info_off     = OFF(gba_get_avinfo);
    out->retro_load_game_off              = OFF(gba_load_game);
    out->retro_unload_game_off            = OFF(gba_unload);
    out->retro_run_off                    = OFF(gba_run);
    out->retro_reset_off                  = OFF(gba_reset);
#undef OFF
}

/* ═══════════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════ */
#define GBA_W   240
#define GBA_H   160
#define GBA_SR  32768

/* Memory layout */
#define BIOS_SIZE  0x4000
#define EWRAM_SIZE 0x40000
#define IWRAM_SIZE 0x8000
#define VRAM_SIZE  0x18000
#define OAM_SIZE   0x400
#define PAL_SIZE   0x400
#define ROM_MAX    (32*1024*1024)

/* I/O register offsets (from 0x04000000) */
#define IO_DISPCNT   0x000
#define IO_DISPSTAT  0x004
#define IO_VCOUNT    0x006
#define IO_BG0CNT    0x008
#define IO_BG1CNT    0x00A
#define IO_BG2CNT    0x00C
#define IO_BG3CNT    0x00E
#define IO_BG0HOFS   0x010
#define IO_BG0VOFS   0x012
#define IO_BG2PA     0x020
#define IO_BG2PB     0x022
#define IO_BG2PC     0x024
#define IO_BG2PD     0x026
#define IO_BG2X_L    0x028
#define IO_BG2X_H    0x02A
#define IO_BG2Y_L    0x02C
#define IO_BG2Y_H    0x02E
#define IO_WIN0H     0x040
#define IO_WIN0V     0x044
#define IO_WININ     0x048
#define IO_WINOUT    0x04A
#define IO_BLDCNT    0x050
#define IO_BLDALPHA  0x052
#define IO_BLDY      0x054
#define IO_SOUNDCNT_X 0x084
#define IO_DMA0SAD   0x0B0
#define IO_DMA0DAD   0x0B4
#define IO_DMA0CNT_L 0x0B8
#define IO_DMA0CNT_H 0x0BA
#define IO_TM0CNT_L  0x100
#define IO_TM0CNT_H  0x102
#define IO_KEYINPUT  0x130
#define IO_IE        0x200
#define IO_IF        0x202
#define IO_WAITCNT   0x204
#define IO_IME       0x208
#define IO_HALTCNT   0x300

/* ═══════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════ */
static retro_video_refresh_t       g_video_cb;
static retro_audio_sample_t        g_audio_cb;
static retro_audio_sample_batch_t  g_abatch_cb;
static retro_input_poll_t          g_input_poll_cb;
static retro_input_state_t         g_input_state_cb;
static retro_environment_t         g_env_cb;

static const u8 *g_rom;
static u64       g_rom_size;

static u8  g_bios[BIOS_SIZE];
static u8  g_ewram[EWRAM_SIZE];
static u8  g_iwram[IWRAM_SIZE];
static u8  g_vram[VRAM_SIZE];
static u8  g_oam[OAM_SIZE];
static u8  g_pal[PAL_SIZE];
static u8  g_io[0x400];   /* I/O space 0x04000000-0x040003FF */

/* Framebuffer */
static u32 g_fb[GBA_W * GBA_H];

/* CPU */
typedef struct {
    u32 r[16];    /* r0-r15 (r13=SP, r14=LR, r15=PC) */
    u32 cpsr;
    u32 spsr;
    /* Banked regs (FIQ, SVC, ABT, IRQ, UND) */
    u32 r_svc[2], spsr_svc;
    u32 r_irq[2], spsr_irq;
    u32 r_fiq[7], spsr_fiq;
    int halted;
} ARM7;
static ARM7 g_cpu;

/* CPSR flags */
#define CPSR_N (1u<<31)
#define CPSR_Z (1u<<30)
#define CPSR_C (1u<<29)
#define CPSR_V (1u<<28)
#define CPSR_T (1u<<5)   /* THUMB bit */
#define CPSR_I (1u<<7)   /* IRQ disable */
#define CPSR_M 0x1Fu
#define MODE_USR 0x10u
#define MODE_FIQ 0x11u
#define MODE_IRQ 0x12u
#define MODE_SVC 0x13u
#define MODE_ABT 0x17u
#define MODE_UND 0x1Bu
#define MODE_SYS 0x1Fu

#define GN(v) ((v)>>31)
#define GZ(v) ((v)==0)
#define PC    g_cpu.r[15]
#define SP    g_cpu.r[13]
#define LR    g_cpu.r[14]

/* Helper: set CPSR N/Z/C/V from a 32-bit result */
static void setNZ(u32 v) {
    g_cpu.cpsr = (g_cpu.cpsr & ~(CPSR_N|CPSR_Z))
               | (v ? 0 : CPSR_Z)
               | (v & 0x80000000u ? CPSR_N : 0);
}
static void setNZCV_add(u32 a, u32 b, u64 res) {
    u32 r = (u32)res;
    g_cpu.cpsr = (g_cpu.cpsr & ~(CPSR_N|CPSR_Z|CPSR_C|CPSR_V))
               | (r ? 0 : CPSR_Z) | (r & 0x80000000u ? CPSR_N : 0)
               | (res > 0xFFFFFFFFu ? CPSR_C : 0)
               | (((~(a^b)) & (a^r)) >> 3 & CPSR_V);
}
static void setNZCV_sub(u32 a, u32 b, u64 res) {
    u32 r = (u32)res;
    g_cpu.cpsr = (g_cpu.cpsr & ~(CPSR_N|CPSR_Z|CPSR_C|CPSR_V))
               | (r ? 0 : CPSR_Z) | (r & 0x80000000u ? CPSR_N : 0)
               | (a >= b ? CPSR_C : 0)
               | (((a^b) & (a^r)) >> 3 & CPSR_V);
}

/* ═══════════════════════════════════════════════════════════════════════
 * MEMORY BUS
 * ═══════════════════════════════════════════════════════════════════════ */
static u8 io_r8(u32 off) { if (off < sizeof g_io) return g_io[off]; return 0; }
static u16 io_r16(u32 off) {
    if (off == IO_KEYINPUT) {
        /* Active-low: bit set = not pressed */
        u16 k = 0x3FF;
        if (g_input_state_cb) {
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A))      k &= ~(1<<0);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B))      k &= ~(1<<1);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT)) k &= ~(1<<2);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START))  k &= ~(1<<3);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT))  k &= ~(1<<4);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT))   k &= ~(1<<5);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP))     k &= ~(1<<6);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN))   k &= ~(1<<7);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R))      k &= ~(1<<8);
            if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L))      k &= ~(1<<9);
        }
        return k;
    }
    if (off + 1 < sizeof g_io)
        return (u16)g_io[off] | ((u16)g_io[off+1] << 8);
    return 0;
}
static void io_w8(u32 off, u8 v)  { if (off < sizeof g_io) g_io[off] = v; }
static void io_w16(u32 off, u16 v){ io_w8(off,(u8)v); io_w8(off+1,(u8)(v>>8)); }
static void io_w32(u32 off, u32 v){ io_w16(off,(u16)v); io_w16(off+2,(u16)(v>>16)); }

static u32 bus_r32(u32 addr) {
    addr &= ~3u;
    u32 seg = addr >> 24;
    u32 off = addr & 0xFFFFFF;
    switch (seg) {
    case 0x00: { u32 o=off&(BIOS_SIZE-1); return (u32)g_bios[o]|((u32)g_bios[o+1]<<8)|((u32)g_bios[o+2]<<16)|((u32)g_bios[o+3]<<24); }
    case 0x02: { u32 o=off&(EWRAM_SIZE-1); return (u32)g_ewram[o]|((u32)g_ewram[o+1]<<8)|((u32)g_ewram[o+2]<<16)|((u32)g_ewram[o+3]<<24); }
    case 0x03: { u32 o=off&(IWRAM_SIZE-1); return (u32)g_iwram[o]|((u32)g_iwram[o+1]<<8)|((u32)g_iwram[o+2]<<16)|((u32)g_iwram[o+3]<<24); }
    case 0x04: { u32 io=off&0x3FF; return (u32)io_r16(io)|((u32)io_r16(io+2)<<16); }
    case 0x05: { u32 o=off&(PAL_SIZE-1)&~3u; return (u32)g_pal[o]|((u32)g_pal[o+1]<<8)|((u32)g_pal[o+2]<<16)|((u32)g_pal[o+3]<<24); }
    case 0x06: { u32 o=off&(VRAM_SIZE-1)&~3u; return (u32)g_vram[o]|((u32)g_vram[o+1]<<8)|((u32)g_vram[o+2]<<16)|((u32)g_vram[o+3]<<24); }
    case 0x07: { u32 o=off&(OAM_SIZE-1)&~3u; return (u32)g_oam[o]|((u32)g_oam[o+1]<<8)|((u32)g_oam[o+2]<<16)|((u32)g_oam[o+3]<<24); }
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 ro = addr - 0x08000000;
        if (g_rom && ro+3 < g_rom_size)
            return (u32)g_rom[ro]|((u32)g_rom[ro+1]<<8)|((u32)g_rom[ro+2]<<16)|((u32)g_rom[ro+3]<<24);
        return 0;
    }
    default: return 0;
    }
}
static u16 bus_r16(u32 addr) {
    addr &= ~1u;
    u32 seg = addr >> 24;
    u32 off = addr & 0xFFFFFF;
    switch (seg) {
    case 0x00: { u32 o=off&(BIOS_SIZE-1); return (u16)g_bios[o]|((u16)g_bios[o+1]<<8); }
    case 0x02: { u32 o=off&(EWRAM_SIZE-1); return (u16)g_ewram[o]|((u16)g_ewram[o+1]<<8); }
    case 0x03: { u32 o=off&(IWRAM_SIZE-1); return (u16)g_iwram[o]|((u16)g_iwram[o+1]<<8); }
    case 0x04: return io_r16(off & 0x3FF);
    case 0x05: { u32 o=off&(PAL_SIZE-1)&~1u; return (u16)g_pal[o]|((u16)g_pal[o+1]<<8); }
    case 0x06: { u32 o=off&(VRAM_SIZE-1)&~1u; return (u16)g_vram[o]|((u16)g_vram[o+1]<<8); }
    case 0x07: { u32 o=off&(OAM_SIZE-1)&~1u; return (u16)g_oam[o]|((u16)g_oam[o+1]<<8); }
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 ro=addr-0x08000000;
        if (g_rom && ro+1 < g_rom_size)
            return (u16)g_rom[ro]|((u16)g_rom[ro+1]<<8);
        return 0;
    }
    default: return 0;
    }
}
static u8 bus_r8(u32 addr) {
    u32 seg=addr>>24, off=addr&0xFFFFFF;
    switch(seg) {
    case 0x00: return g_bios[off&(BIOS_SIZE-1)];
    case 0x02: return g_ewram[off&(EWRAM_SIZE-1)];
    case 0x03: return g_iwram[off&(IWRAM_SIZE-1)];
    case 0x04: return io_r8(off&0x3FF);
    case 0x05: return g_pal[off&(PAL_SIZE-1)];
    case 0x06: return g_vram[off&(VRAM_SIZE-1)];
    case 0x07: return g_oam[off&(OAM_SIZE-1)];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 ro=addr-0x08000000;
        if (g_rom && ro < g_rom_size) return g_rom[ro];
        return 0;
    }
    default: return 0;
    }
}
static void bus_w32(u32 addr, u32 v) {
    u32 seg=addr>>24, off=addr&0xFFFFFF;
    u8 *p; u32 mask;
    switch(seg) {
    case 0x02: p=g_ewram; mask=EWRAM_SIZE-1; off&=mask; p[off]=(u8)v; p[off+1]=(u8)(v>>8); p[off+2]=(u8)(v>>16); p[off+3]=(u8)(v>>24); return;
    case 0x03: p=g_iwram; mask=IWRAM_SIZE-1; off&=mask; p[off]=(u8)v; p[off+1]=(u8)(v>>8); p[off+2]=(u8)(v>>16); p[off+3]=(u8)(v>>24); return;
    case 0x04: io_w32(off&0x3FF,v); return;
    case 0x05: { u32 o=off&(PAL_SIZE-1)&~3u; g_pal[o]=(u8)v; g_pal[o+1]=(u8)(v>>8); g_pal[o+2]=(u8)(v>>16); g_pal[o+3]=(u8)(v>>24); return; }
    case 0x06: { u32 o=off&(VRAM_SIZE-1)&~3u; g_vram[o]=(u8)v; g_vram[o+1]=(u8)(v>>8); g_vram[o+2]=(u8)(v>>16); g_vram[o+3]=(u8)(v>>24); return; }
    case 0x07: { u32 o=off&(OAM_SIZE-1)&~3u; g_oam[o]=(u8)v; g_oam[o+1]=(u8)(v>>8); g_oam[o+2]=(u8)(v>>16); g_oam[o+3]=(u8)(v>>24); return; }
    default: return;
    }
}
static void bus_w16(u32 addr, u16 v) {
    u32 seg=addr>>24, off=addr&0xFFFFFF;
    switch(seg) {
    case 0x02: { u32 o=off&(EWRAM_SIZE-1)&~1u; g_ewram[o]=(u8)v; g_ewram[o+1]=(u8)(v>>8); return; }
    case 0x03: { u32 o=off&(IWRAM_SIZE-1)&~1u; g_iwram[o]=(u8)v; g_iwram[o+1]=(u8)(v>>8); return; }
    case 0x04: io_w16(off&0x3FF,v); return;
    case 0x05: { u32 o=off&(PAL_SIZE-1)&~1u; g_pal[o]=(u8)v; g_pal[o+1]=(u8)(v>>8); return; }
    case 0x06: { u32 o=off&(VRAM_SIZE-1)&~1u; g_vram[o]=(u8)v; g_vram[o+1]=(u8)(v>>8); return; }
    case 0x07: { u32 o=off&(OAM_SIZE-1)&~1u; g_oam[o]=(u8)v; g_oam[o+1]=(u8)(v>>8); return; }
    default: return;
    }
}
static void bus_w8(u32 addr, u8 v) {
    u32 seg=addr>>24, off=addr&0xFFFFFF;
    switch(seg) {
    case 0x02: g_ewram[off&(EWRAM_SIZE-1)]=v; return;
    case 0x03: g_iwram[off&(IWRAM_SIZE-1)]=v; return;
    case 0x04: io_w8(off&0x3FF,v); return;
    case 0x05: g_pal[off&(PAL_SIZE-1)]=v; return;
    case 0x06: g_vram[off&(VRAM_SIZE-1)]=v; return;
    case 0x07: g_oam[off&(OAM_SIZE-1)]=v; return;
    default: return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * BARREL SHIFTER
 * ═══════════════════════════════════════════════════════════════════════ */
static u32 shift_op(u32 val, u32 type, u32 amt, int *cout) {
    if (!amt) { *cout = (g_cpu.cpsr>>29)&1; return val; }
    switch (type & 3) {
    case 0: /* LSL */ *cout = amt<32?(val>>(32-amt))&1:0; return amt<32?val<<amt:0;
    case 1: /* LSR */ *cout = amt<32?(val>>(amt-1))&1:(val>>31)&1; return amt<32?val>>amt:0;
    case 2: /* ASR */ *cout = amt<32?(val>>(amt-1))&1:(val>>31)&1; return amt<32?(u32)((s32)val>>amt):(u32)((s32)val>>31);
    case 3: /* ROR */ amt&=31; if(!amt){u32 c=(g_cpu.cpsr>>29)&1;*cout=val&1;return(val>>1)|(c<<31);} *cout=(val>>(amt-1))&1; return(val>>amt)|(val<<(32-amt));
    }
    return val;
}

/* ═══════════════════════════════════════════════════════════════════════
 * ARM MODE EXECUTION
 * ═══════════════════════════════════════════════════════════════════════ */
static int arm_cond(u32 ins) {
    u32 cond = ins >> 28;
    u32 n=(g_cpu.cpsr>>31)&1, z=(g_cpu.cpsr>>30)&1, c=(g_cpu.cpsr>>29)&1, v=(g_cpu.cpsr>>28)&1;
    switch (cond) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 0xA: return n==v;
    case 0xB: return n!=v;
    case 0xC: return !z && (n==v);
    case 0xD: return z  || (n!=v);
    case 0xE: return 1;
    default:  return 0;
    }
}

static int arm_step(void) {
    u32 ins = bus_r32(PC); PC += 4;
    if (!arm_cond(ins)) return 4;
    u32 op = (ins>>25)&7, bit4=(ins>>4)&1, bit7=(ins>>7)&1;
    /* Branch */
    if ((ins & 0x0E000000) == 0x0A000000) {
        s32 off = (s32)(ins << 8) >> 6;
        if (ins & (1<<24)) LR = PC - 4;
        PC += off; PC &= ~3u;
        return 12;
    }
    /* BX */
    if ((ins & 0x0FFFFFF0) == 0x012FFF10) {
        u32 rn = g_cpu.r[ins&0xF];
        if (rn & 1) { g_cpu.cpsr |= CPSR_T; PC = rn & ~1u; }
        else { g_cpu.cpsr &= ~CPSR_T; PC = rn & ~3u; }
        return 8;
    }
    /* Data processing */
    if (op <= 1) {
        u32 opcode = (ins>>21)&0xF;
        int s = (ins>>20)&1;
        u32 rn = g_cpu.r[(ins>>16)&0xF];
        u32 rd_idx = (ins>>12)&0xF;
        u32 op2; int cout = (g_cpu.cpsr>>29)&1;
        if (ins & (1<<25)) {
            u32 imm = ins & 0xFF, rot = ((ins>>8)&0xF)*2;
            op2 = (imm>>rot)|(imm<<(32-rot));
            if (rot) cout = (op2>>31)&1;
        } else {
            u32 rm = g_cpu.r[ins&0xF];
            u32 stype=(ins>>5)&3;
            u32 samt;
            if (ins&(1<<4)) samt=g_cpu.r[(ins>>8)&0xF]&0xFF;
            else samt=(ins>>7)&0x1F;
            op2 = shift_op(rm, stype, samt, &cout);
        }
        u32 res=0; u64 res64=0;
        switch(opcode){
        case 0: res=rn&op2; setNZ(res); if(s)g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 1: res=rn^op2; setNZ(res); if(s)g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 2: res64=(u64)rn-op2; res=(u32)res64; if(s)setNZCV_sub(rn,op2,res64); break;
        case 3: res64=(u64)op2-rn; res=(u32)res64; if(s)setNZCV_sub(op2,rn,res64); break;
        case 4: res64=(u64)rn+op2; res=(u32)res64; if(s)setNZCV_add(rn,op2,res64); break;
        case 5: res64=(u64)rn+op2+((g_cpu.cpsr>>29)&1); res=(u32)res64; if(s)setNZCV_add(rn,op2,res64); break;
        case 6: res64=(u64)rn-op2-1+((g_cpu.cpsr>>29)&1); res=(u32)res64; if(s)setNZCV_sub(rn,op2,res64); break;
        case 7: res64=(u64)op2-rn-1+((g_cpu.cpsr>>29)&1); res=(u32)res64; if(s)setNZCV_sub(op2,rn,res64); break;
        case 8: res=rn&op2; setNZ(res); g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); return 4;
        case 9: res=rn^op2; setNZ(res); g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); return 4;
        case 0xA: res64=(u64)rn-op2; setNZCV_sub(rn,op2,res64); return 4;
        case 0xB: res64=(u64)rn+op2; setNZCV_add(rn,op2,res64); return 4;
        case 0xC: res=rn|op2; setNZ(res); if(s)g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 0xD: res=op2; setNZ(res); if(s)g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 0xE: res=rn&~op2; setNZ(res); if(s)g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 0xF: res=~op2; setNZ(res); if(s)g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        }
        g_cpu.r[rd_idx] = res;
        return 4;
    }
    /* MUL */
    if (!(ins&(1<<25)) && bit7 && bit4 && !(ins&(1<<24))) {
        u32 rm=g_cpu.r[ins&0xF], rs=g_cpu.r[(ins>>8)&0xF];
        u32 rd_idx=(ins>>16)&0xF;
        u32 res=rm*rs;
        if ((ins>>20)&1) setNZ(res);
        if ((ins>>21)&1) res+=g_cpu.r[(ins>>12)&0xF]; /* MLA */
        g_cpu.r[rd_idx]=res;
        return 8;
    }
    /* LDR/STR */
    if ((ins & 0x0C000000) == 0x04000000) {
        u32 rn_idx=(ins>>16)&0xF, rd_idx=(ins>>12)&0xF;
        u32 rn=g_cpu.r[rn_idx];
        int P=(ins>>24)&1, U=(ins>>23)&1, B=(ins>>22)&1, W=(ins>>21)&1, L=(ins>>20)&1;
        u32 off2; int cout=0;
        if (ins&(1<<25)) { u32 rm=g_cpu.r[ins&0xF]; off2=shift_op(rm,(ins>>5)&3,(ins>>7)&0x1F,&cout); }
        else off2=ins&0xFFF;
        u32 addr = P ? (U?rn+off2:rn-off2) : rn;
        if (L) {
            u32 v = B ? bus_r8(addr) : bus_r32(addr);
            g_cpu.r[rd_idx] = v;
        } else {
            u32 v = g_cpu.r[rd_idx]; if(rd_idx==15) v+=4;
            if (B) bus_w8(addr,v); else bus_w32(addr,v);
        }
        if (!P) addr = U ? rn+off2 : rn-off2;
        if (!P || W) g_cpu.r[rn_idx] = addr;
        return L ? 8 : 8;
    }
    /* LDM/STM */
    if ((ins & 0x0E000000) == 0x08000000) {
        u32 rn_idx=(ins>>16)&0xF;
        u32 rn=g_cpu.r[rn_idx];
        int P=(ins>>24)&1, U=(ins>>23)&1, W=(ins>>21)&1, L=(ins>>20)&1;
        u16 rlist=(u16)(ins&0xFFFF);
        u32 addr=rn; int cnt=0;
        for(int i=0;i<16;i++) if(rlist&(1<<i)) cnt++;
        if(!U) addr -= cnt*4;
        u32 base=addr;
        for(int i=0;i<16;i++) {
            if(!(rlist&(1<<i))) continue;
            u32 ea = P ? (U?addr+4:addr) : addr;
            if(!P) addr += U?4:-4; else addr = ea;
            if(L) g_cpu.r[i]=bus_r32(ea?ea:addr);
            else  bus_w32(ea?ea:addr, g_cpu.r[i]);
            addr += U?4:-4;
        }
        if(W) g_cpu.r[rn_idx] = U ? base+cnt*4 : base-cnt*4;
        return 8+cnt*4;
    }
    /* SWI */
    if ((ins & 0x0F000000) == 0x0F000000) {
        /* SWI: save state, jump to vector */
        g_cpu.r_svc[0] = SP; g_cpu.r_svc[1] = LR;
        g_cpu.spsr_svc = g_cpu.cpsr;
        LR = PC - 4;
        g_cpu.cpsr = (g_cpu.cpsr & ~(CPSR_M|CPSR_T)) | MODE_SVC | CPSR_I;
        PC = 0x00000008;
        return 16;
    }
    return 4;
}

/* ═══════════════════════════════════════════════════════════════════════
 * THUMB MODE EXECUTION
 * ═══════════════════════════════════════════════════════════════════════ */
static int thumb_step(void) {
    u16 ins = bus_r16(PC); PC += 2;
    u32 op = ins >> 13;
    /* Format 1: Move shifted register */
    if (op == 0 && ((ins>>11)&3) != 3) {
        u32 rs = g_cpu.r[(ins>>3)&7], rd_idx=ins&7;
        u32 off=(ins>>6)&0x1F, type=(ins>>11)&3;
        int cout=0;
        u32 res=shift_op(rs,type,off?off:32,&cout);
        g_cpu.r[rd_idx]=res; setNZ(res);
        g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0);
        return 4;
    }
    /* Format 2: Add/Sub */
    if (op == 0 && ((ins>>11)&3) == 3) {
        u32 rs=g_cpu.r[(ins>>3)&7], rd_idx=ins&7;
        u32 rn_v=((ins>>10)&1)?((ins>>6)&7):g_cpu.r[(ins>>6)&7];
        u64 res64=((ins>>9)&1)?(u64)rs-rn_v:(u64)rs+rn_v;
        u32 res=(u32)res64;
        g_cpu.r[rd_idx]=res;
        if((ins>>9)&1) setNZCV_sub(rs,rn_v,res64);
        else setNZCV_add(rs,rn_v,res64);
        return 4;
    }
    /* Format 3: Move/Compare/Add/Sub immediate */
    if (op == 1) {
        u32 rd_idx=(ins>>8)&7, imm=ins&0xFF;
        u32 op2=(ins>>11)&3;
        u64 res64;
        switch(op2){
        case 0: g_cpu.r[rd_idx]=imm; setNZ(imm); return 4;
        case 1: res64=(u64)g_cpu.r[rd_idx]-imm; setNZCV_sub(g_cpu.r[rd_idx],imm,res64); return 4;
        case 2: res64=(u64)g_cpu.r[rd_idx]+imm; g_cpu.r[rd_idx]=(u32)res64; setNZCV_add(g_cpu.r[rd_idx]-imm,imm,res64); return 4;
        case 3: res64=(u64)g_cpu.r[rd_idx]-imm; g_cpu.r[rd_idx]=(u32)res64; setNZCV_sub(g_cpu.r[rd_idx]+imm,imm,res64); return 4;
        }
        return 4;
    }
    /* Format 4: ALU */
    if ((ins>>10)==0x10) {
        u32 rop=(ins>>6)&0xF, rs=g_cpu.r[(ins>>3)&7], rd_idx=ins&7, rd=g_cpu.r[rd_idx];
        u64 res64; u32 res; int cout=0;
        switch(rop){
        case 0: res=rd&rs; setNZ(res); g_cpu.r[rd_idx]=res; break;
        case 1: res=rd^rs; setNZ(res); g_cpu.r[rd_idx]=res; break;
        case 2: res=shift_op(rd,0,rs&0xFF,&cout); g_cpu.r[rd_idx]=res; setNZ(res); g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 3: res=shift_op(rd,1,rs&0xFF,&cout); g_cpu.r[rd_idx]=res; setNZ(res); g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 4: res=shift_op(rd,2,rs&0xFF,&cout); g_cpu.r[rd_idx]=res; setNZ(res); g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 5: res64=(u64)rd+rs+((g_cpu.cpsr>>29)&1); res=(u32)res64; g_cpu.r[rd_idx]=res; setNZCV_add(rd,rs,res64); break;
        case 6: res64=(u64)rd-rs-1+((g_cpu.cpsr>>29)&1); res=(u32)res64; g_cpu.r[rd_idx]=res; setNZCV_sub(rd,rs,res64); break;
        case 7: res=shift_op(rd,3,rs&0xFF,&cout); g_cpu.r[rd_idx]=res; setNZ(res); g_cpu.cpsr=(g_cpu.cpsr&~CPSR_C)|(cout?CPSR_C:0); break;
        case 8: res=rd&rs; setNZ(res); break;
        case 9: res=(u32)(-(s32)rs); g_cpu.r[rd_idx]=res; setNZCV_sub(0,rs,(u64)0-rs); break;
        case 0xA: res64=(u64)rd-rs; setNZCV_sub(rd,rs,res64); break;
        case 0xB: res64=(u64)rd+rs; setNZCV_add(rd,rs,res64); break;
        case 0xC: res=rd|rs; setNZ(res); g_cpu.r[rd_idx]=res; break;
        case 0xD: res=rd*rs; setNZ(res); g_cpu.r[rd_idx]=res; break;
        case 0xE: res=rd&~rs; setNZ(res); g_cpu.r[rd_idx]=res; break;
        case 0xF: res=~rs; setNZ(res); g_cpu.r[rd_idx]=res; break;
        }
        return 4;
    }
    /* Format 5: HiReg/BX */
    if ((ins>>10)==0x11) {
        u32 rop=(ins>>8)&3, h1=(ins>>7)&1, h2=(ins>>6)&1;
        u32 rs=g_cpu.r[((ins>>3)&7)+(h2?8:0)];
        u32 rd_idx=(ins&7)+(h1?8:0);
        if(rop==3){if(rs&1){g_cpu.cpsr|=CPSR_T;PC=rs&~1u;}else{g_cpu.cpsr&=~CPSR_T;PC=rs&~3u;}return 8;}
        u32 rd=g_cpu.r[rd_idx];
        switch(rop){
        case 0: g_cpu.r[rd_idx]=rd+rs; break;
        case 1: { u64 r2=(u64)rd-rs; setNZCV_sub(rd,rs,r2); } break;
        case 2: g_cpu.r[rd_idx]=rs; break;
        }
        return 4;
    }
    /* Format 6: LDR PC-relative */
    if ((ins>>11)==9) { u32 off=(ins&0xFF)<<2; g_cpu.r[(ins>>8)&7]=bus_r32((PC&~3u)+off); return 8; }
    /* Format 7/8: LDR/STR */
    if (((ins>>12)&0xF)==5) {
        u32 L=(ins>>11)&1, B=(ins>>10)&1, ro=g_cpu.r[(ins>>6)&7];
        u32 rb=g_cpu.r[(ins>>3)&7], rd_idx=ins&7;
        u32 addr=rb+ro;
        if(L){ if(B) g_cpu.r[rd_idx]=bus_r8(addr); else g_cpu.r[rd_idx]=bus_r32(addr); }
        else { if(B) bus_w8(addr,g_cpu.r[rd_idx]); else bus_w32(addr,g_cpu.r[rd_idx]); }
        return 8;
    }
    /* Format 9: LDR/STR word/byte with immediate offset */
    if (((ins>>13)&3)==3) {
        u32 B=(ins>>12)&1, L=(ins>>11)&1, off5=((ins>>6)&0x1F)<<(B?0:2);
        u32 rb=g_cpu.r[(ins>>3)&7], rd_idx=ins&7;
        u32 addr=rb+off5;
        if(L){ if(B) g_cpu.r[rd_idx]=bus_r8(addr); else g_cpu.r[rd_idx]=bus_r32(addr); }
        else { if(B) bus_w8(addr,g_cpu.r[rd_idx]); else bus_w32(addr,g_cpu.r[rd_idx]); }
        return 8;
    }
    /* Format 10: LDRH/STRH */
    if ((ins>>12)==0x8) {
        u32 L=(ins>>11)&1, off5=((ins>>6)&0x1F)<<1;
        u32 rb=g_cpu.r[(ins>>3)&7], rd_idx=ins&7;
        u32 addr=rb+off5;
        if(L) g_cpu.r[rd_idx]=bus_r16(addr); else bus_w16(addr,(u16)g_cpu.r[rd_idx]);
        return 8;
    }
    /* Format 11: SP-relative LDR/STR */
    if ((ins>>12)==9) {
        u32 L=(ins>>11)&1, rd_idx=(ins>>8)&7, off=(ins&0xFF)<<2;
        u32 addr=SP+off;
        if(L) g_cpu.r[rd_idx]=bus_r32(addr); else bus_w32(addr,g_cpu.r[rd_idx]);
        return 8;
    }
    /* Format 12: load address */
    if ((ins>>12)==0xA) {
        u32 SP_or_PC=(ins>>11)&1, rd_idx=(ins>>8)&7, off=(ins&0xFF)<<2;
        g_cpu.r[rd_idx]=(SP_or_PC?SP:(PC&~3u))+off;
        return 4;
    }
    /* Format 13: ADD SP */
    if ((ins>>8)==0xB0) { s32 off=(ins&0x7F)<<2; if(ins&0x80) SP-=off; else SP+=off; return 4; }
    /* Format 14: PUSH/POP */
    if ((ins>>12)==0xB) {
        u32 L=(ins>>11)&1, R=(ins>>8)&1;
        u8 rlist=(u8)ins;
        if(!L){
            if(R){SP-=4;bus_w32(SP,LR);}
            for(int i=7;i>=0;i--){if(!(rlist&(1<<i)))continue;SP-=4;bus_w32(SP,g_cpu.r[i]);}
        }else{
            for(int i=0;i<8;i++){if(!(rlist&(1<<i)))continue;g_cpu.r[i]=bus_r32(SP);SP+=4;}
            if(R){PC=bus_r32(SP)&~1u;SP+=4;}
        }
        return 8;
    }
    /* Format 15: LDMIA/STMIA */
    if ((ins>>12)==0xC) {
        u32 L=(ins>>11)&1, rb_idx=(ins>>8)&7;
        u8 rlist=(u8)ins;
        u32 addr=g_cpu.r[rb_idx];
        for(int i=0;i<8;i++){if(!(rlist&(1<<i)))continue;if(L)g_cpu.r[i]=bus_r32(addr);else bus_w32(addr,g_cpu.r[i]);addr+=4;}
        g_cpu.r[rb_idx]=addr;
        return 8;
    }
    /* Format 16/17: Bcond / SWI */
    if ((ins>>12)==0xD) {
        u32 cond=(ins>>8)&0xF;
        if(cond==0xF){g_cpu.r_svc[0]=SP;g_cpu.r_svc[1]=LR;g_cpu.spsr_svc=g_cpu.cpsr;LR=PC-2;g_cpu.cpsr=(g_cpu.cpsr&~(CPSR_M|CPSR_T))|MODE_SVC|CPSR_I;PC=0x08;return 16;}
        s8 off=(s8)(ins&0xFF);
        int take=0;
        u32 n=(g_cpu.cpsr>>31)&1,z=(g_cpu.cpsr>>30)&1,c=(g_cpu.cpsr>>29)&1,v=(g_cpu.cpsr>>28)&1;
        switch(cond){case 0:take=z;break;case 1:take=!z;break;case 2:take=c;break;case 3:take=!c;break;case 4:take=n;break;case 5:take=!n;break;case 6:take=v;break;case 7:take=!v;break;case 8:take=c&&!z;break;case 9:take=!c||z;break;case 0xA:take=n==v;break;case 0xB:take=n!=v;break;case 0xC:take=!z&&(n==v);break;case 0xD:take=z||(n!=v);break;case 0xE:take=1;break;}
        if(take){PC+=(s16)((s32)off*2);return 12;}
        return 8;
    }
    /* Format 18: B */
    if ((ins>>11)==0x1C) { s32 off=(s32)((ins&0x7FF)<<21)>>20; PC+=off; return 12; }
    /* Format 19: BL */
    if ((ins>>11)==0x1E){LR=PC+(s32)((ins&0x7FF)<<21);return 4;}
    if ((ins>>11)==0x1F){u32 t=PC-2;PC=LR+(s32)((ins&0x7FF)<<1);LR=t|1;return 8;}
    return 4;
}

/* ═══════════════════════════════════════════════════════════════════════
 * PPU
 * ═══════════════════════════════════════════════════════════════════════ */
/* Convert GBA 15-bit BGR555 to XRGB8888 */
static u32 gba_color(u16 c) {
    u32 r=(c&0x1F)<<3, g=((c>>5)&0x1F)<<3, b=((c>>10)&0x1F)<<3;
    return (r<<16)|(g<<8)|b;
}

static void ppu_render_line(int y) {
    u16 dispcnt = (u16)g_io[IO_DISPCNT] | ((u16)g_io[IO_DISPCNT+1] << 8);
    u32 mode = dispcnt & 7;
    u32 *row = &g_fb[y * GBA_W];

    if (mode == 3) {
        /* Mode 3: 240x160 full-color bitmap in VRAM */
        for (int x = 0; x < GBA_W; x++) {
            u32 off = (y * GBA_W + x) * 2;
            u16 c = (u16)g_vram[off] | ((u16)g_vram[off+1] << 8);
            row[x] = gba_color(c);
        }
    } else if (mode == 4) {
        /* Mode 4: 240x160 8bpp paletted bitmap */
        u32 base = (dispcnt & (1<<4)) ? 0xA000 : 0;
        for (int x = 0; x < GBA_W; x++) {
            u8 idx = g_vram[base + y*GBA_W + x];
            u16 c = (u16)g_pal[idx*2] | ((u16)g_pal[idx*2+1] << 8);
            row[x] = gba_color(c);
        }
    } else if (mode == 5) {
        /* Mode 5: 160x128 full-color bitmap, letterboxed */
        for (int x = 0; x < GBA_W; x++) {
            if (x < 160 && y < 128) {
                u32 off = (y * 160 + x) * 2;
                u16 c = (u16)g_vram[off] | ((u16)g_vram[off+1] << 8);
                row[x] = gba_color(c);
            } else {
                row[x] = 0;
            }
        }
    } else {
        /* Mode 0: tiled BG */
        u16 bg0cnt = (u16)g_io[IO_BG0CNT] | ((u16)g_io[IO_BG0CNT+1]<<8);
        u16 hofs   = (u16)g_io[IO_BG0HOFS] | ((u16)g_io[IO_BG0HOFS+1]<<8);
        u16 vofs   = (u16)g_io[IO_BG0VOFS] | ((u16)g_io[IO_BG0VOFS+1]<<8);
        u32 tbase = ((bg0cnt >> 2) & 3) * 0x4000;
        u32 mbase = ((bg0cnt >> 8) & 0x1F) * 0x800;
        int bpp8  = (bg0cnt >> 7) & 1;
        int py = (y + vofs) & 0xFF;
        for (int x = 0; x < GBA_W; x++) {
            int px = (x + hofs) & 0xFF;
            u32 tile_x = px / 8, tile_y = py / 8;
            u32 map_addr = mbase + (tile_y * 32 + tile_x) * 2;
            u16 entry = (u16)g_vram[map_addr] | ((u16)g_vram[map_addr+1]<<8);
            u32 tile = entry & 0x3FF;
            int hf = (entry>>10)&1, vf=(entry>>11)&1;
            int tx = hf ? (7-(px&7)) : (px&7);
            int ty = vf ? (7-(py&7)) : (py&7);
            u8 pidx;
            if (bpp8) {
                pidx = g_vram[tbase + tile*64 + ty*8 + tx];
            } else {
                u32 pb = (u32)((entry>>12)&0xF)*32;
                u8 byte = g_vram[tbase + tile*32 + ty*4 + tx/2];
                pidx = (tx&1) ? (byte>>4) : (byte&0xF);
                if (pidx) pidx += pb;
            }
            u16 c;
            if (!pidx) { c = (u16)g_pal[0]|((u16)g_pal[1]<<8); }
            else { c = (u16)g_pal[pidx*2]|((u16)g_pal[pidx*2+1]<<8); }
            row[x] = gba_color(c);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * INTERRUPTS
 * ═══════════════════════════════════════════════════════════════════════ */
static void check_irq(void) {
    u16 ie = (u16)g_io[IO_IE] | ((u16)g_io[IO_IE+1]<<8);
    u16 ifl = (u16)g_io[IO_IF] | ((u16)g_io[IO_IF+1]<<8);
    u8 ime = g_io[IO_IME];
    if (ime && (ie & ifl) && !(g_cpu.cpsr & CPSR_I)) {
        g_cpu.r_irq[0] = SP; g_cpu.r_irq[1] = LR;
        g_cpu.spsr_irq = g_cpu.cpsr;
        LR = PC - (g_cpu.cpsr & CPSR_T ? 0 : 4) + 4;
        g_cpu.cpsr = (g_cpu.cpsr & ~(CPSR_M|CPSR_T)) | MODE_IRQ | CPSR_I;
        PC = 0x00000018;
        g_cpu.halted = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * AUDIO
 * ═══════════════════════════════════════════════════════════════════════ */
#define GBA_AUDIO_BUF 512
static s16 g_abuf[GBA_AUDIO_BUF * 2];
static void audio_push(void) {
    for (int i = 0; i < GBA_AUDIO_BUF; i++)
        g_abuf[i*2] = g_abuf[i*2+1] = 0;
    if (g_abatch_cb) g_abatch_cb(g_abuf, GBA_AUDIO_BUF);
}

/* ═══════════════════════════════════════════════════════════════════════
 * RESET
 * ═══════════════════════════════════════════════════════════════════════ */
static void mem_zero(void *p, u64 n) { u8 *b=p; while(n--) *b++=0; }

static void gba_reset_hw(void) {
    mem_zero(&g_cpu, sizeof g_cpu);
    mem_zero(g_ewram, EWRAM_SIZE);
    mem_zero(g_iwram, IWRAM_SIZE);
    mem_zero(g_vram, VRAM_SIZE);
    mem_zero(g_oam, OAM_SIZE);
    mem_zero(g_pal, PAL_SIZE);
    mem_zero(g_io, sizeof g_io);
    mem_zero(g_fb, sizeof g_fb);
    /* Initial CPU state: SVC mode, jump to ROM */
    g_cpu.cpsr = MODE_SVC | CPSR_I;
    if (g_rom) {
        /* Check for "Nintendo" logo and jump past header to 0x08000000 */
        PC = 0x08000000;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_M) | MODE_SYS;
        SP = 0x03007F00;
        g_cpu.r[12] = PC;
        LR = 0;
    }
    /* DISPCNT = mode 3 initially */
    g_io[IO_DISPCNT] = 3;
    /* Timers etc. = 0 */
    g_io[IO_IME] = 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * LIBRETRO API
 * ═══════════════════════════════════════════════════════════════════════ */
static void gba_init(void)   {}
static void gba_deinit(void) {}
static void gba_set_env(retro_environment_t cb) {
    g_env_cb = cb;
    u32 fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
static void gba_set_video(retro_video_refresh_t cb)      { g_video_cb = cb; }
static void gba_set_audio(retro_audio_sample_t cb)       { g_audio_cb = cb; }
static void gba_set_audio_batch(retro_audio_sample_batch_t cb) { g_abatch_cb = cb; }
static void gba_set_input_poll(retro_input_poll_t cb)    { g_input_poll_cb = cb; }
static void gba_set_input_state(retro_input_state_t cb)  { g_input_state_cb = cb; }

static void gba_get_sysinfo(struct retro_system_info *i) {
    i->library_name    = "GBA EmuC";
    i->library_version = "1.0";
    i->valid_extensions = "gba";
    i->need_fullpath   = 0;
    i->block_extract   = 0;
}
static void gba_get_avinfo(struct retro_system_av_info *i) {
    i->geometry.base_width   = GBA_W;
    i->geometry.base_height  = GBA_H;
    i->geometry.max_width    = GBA_W;
    i->geometry.max_height   = GBA_H;
    i->geometry.aspect_ratio = (float)GBA_W / GBA_H;
    i->timing.fps            = 59.7275;
    i->timing.sample_rate    = GBA_SR;
}

static int gba_load_game(const struct retro_game_info *g) {
    if (!g || !g->data || g->size < 0xC0) return 0;
    g_rom      = (const u8 *)g->data;
    g_rom_size = g->size;
    gba_reset_hw();
    return 1;
}
static void gba_unload(void) { g_rom=0; g_rom_size=0; }
static void gba_reset(void)  { gba_reset_hw(); }

static void gba_run(void) {
    if (g_input_poll_cb) g_input_poll_cb();
    /* GBA: 16.78 MHz CPU, 280896 cycles/frame, 228 scanlines, 960 cycles each */
    int scanline_cyc = 960;
    int vblank_start = 160;
    int total_lines  = 228;
    int audio_div    = 280896 / (GBA_SR / GBA_AUDIO_BUF);
    int audio_accum  = 0;

    for (int line = 0; line < total_lines; line++) {
        g_io[IO_VCOUNT] = (u8)line;
        /* Update DISPSTAT VBlank flag */
        if (line >= vblank_start) {
            g_io[IO_DISPSTAT] |= 1;
            if (line == vblank_start) {
                g_io[IO_IF] |= 1; /* VBlank IRQ */
                check_irq();
                if (g_video_cb)
                    g_video_cb(g_fb, GBA_W, GBA_H, (u64)(GBA_W * 4));
            }
        } else {
            g_io[IO_DISPSTAT] &= ~1;
        }
        if (line < vblank_start) ppu_render_line(line);
        /* Run CPU for one scanline */
        int cyc = 0;
        while (cyc < scanline_cyc) {
            if (g_cpu.halted) { cyc += 4; audio_accum += 4; continue; }
            int used = (g_cpu.cpsr & CPSR_T) ? thumb_step() : arm_step();
            cyc += used; audio_accum += used;
            check_irq();
            if (audio_accum >= audio_div) {
                audio_accum -= audio_div;
                audio_push();
            }
        }
    }
}
