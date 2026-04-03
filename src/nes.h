#ifndef NES_STATE_H
#define NES_STATE_H

#include "core.h"

#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_U 0x20
#define F_V 0x40
#define F_N 0x80

#define SET_ZN(v) do { \
    nes->flags = (nes->flags & ~(F_Z|F_N)) \
               | ((v)==0 ? F_Z : 0) \
               | ((v)&0x80 ? F_N : 0); \
} while(0)

#define MAX_ROMS 4096
#define MAX_NAME 28

struct rom_entry {
    char filename[48];
    char display[MAX_NAME];
};

struct pulse_ch {
    u8  duty, halt, const_vol, vol_period;
    u8  sweep_en, sweep_neg, sweep_shift, sweep_period;
    u16 timer;
    u8  length, env_vol, env_counter, env_start;
    u16 timer_count;
    u8  duty_pos, sweep_reload, sweep_counter, enabled;
};

struct triangle_ch {
    u8  linear_load, control;
    u16 timer;
    u8  length, linear_counter, linear_reload;
    u16 timer_count;
    u8  step, enabled;
};

struct noise_ch {
    u8  halt, const_vol, vol_period, mode, period_idx;
    u8  length, env_vol, env_counter, env_start;
    u16 timer_count, shift_reg;
    u8  enabled;
};

struct NES {
    u16 pc;
    u8  a, x, y, sp, flags;
    s32 cycles;
    s32 total_cycles;
    u8  nmi_pending, prev_nmi_line;

    u8  ppu_ctrl, ppu_mask, ppu_status, oam_addr;
    u8  in_vblank;
    u16 vram_addr, temp_addr;
    u8  fine_x, write_toggle, read_buf;

    u8  ram[0x800];
    u8  sram[0x2000];
    u8  vram[0x800];
    u8  palette[0x20];
    u8  oam[256];
    u8  *prg, *chr;
    s32 prg_size, chr_size;
    s32 chr_is_ram;
    s32 mirror;
    s32 mapper;
    s32 prg_bank, chr_bank;
    s32 prg_banks, chr_banks;

    u8  mmc1_shift, mmc1_count;
    u8  mmc1_ctrl, mmc1_chr0, mmc1_chr1, mmc1_prg;

    u8  mmc3_select, mmc3_regs[8];
    u8  mmc3_irq_latch, mmc3_irq_count;
    u8  mmc3_irq_enable, mmc3_irq_reload;
    u8  mmc3_prg_mode, mmc3_chr_mode;

    u8  mmc2_chr_lo[2], mmc2_chr_hi[2];
    u8  mmc2_latch0, mmc2_latch1;

    u8  fme7_cmd, fme7_prg[4], fme7_chr[8];

    u8  pad_state, pad_shift, pad_strobe;
    u8  irq_pending, prev_irq_inhibit;

    struct pulse_ch    pulse[2];
    struct triangle_ch tri;
    struct noise_ch    noise;
    u8  apu_status, frame_mode, frame_irq_inhibit;
    s32 frame_counter, sample_acc;
    s32 fc_step[2][6];

    s16 audio_buf[2048 * 2];
    s32 audio_pos;
    void *gadget;
    void *audio_out_fn;
    s32  audio_handle;
    s16  lpf_prev;
    s32  hpf_in, hpf_out;

    u8  screen[NES_W * NES_H];
    s32 rom_loaded;
    u8  is_pal;
    s32 cpu_freq, num_scanlines;
};

struct ext_args {
    s64 status;
    s64 step;
    u32 frame_count;
    u32 _pad;
    s32 log_fd;
    s32 pad_fd;
    u8  log_addr[16];
    u64 dbg[8];
};

/* bus.c */
u8   ppu_read(struct NES *nes, u16 addr);
void ppu_write(struct NES *nes, u16 addr, u8 val);
u8   cpu_read(struct NES *nes, u16 addr);
void cpu_write(struct NES *nes, u16 addr, u8 val);

/* cpu.c */
void cpu_step(struct NES *nes);
u16  cpu_read16(struct NES *nes, u16 addr);

/* ppu.c */
void render_scanline(struct NES *nes, int y);
void run_frame(struct NES *nes);

/* apu.c */
void apu_write_reg(struct NES *nes, u16 addr, u8 val);
void apu_step(struct NES *nes, int cycles);
void apu_flush(struct NES *nes);

/* ppu.c */
void draw_char(u8 *scr, int x, int y, char ch, u8 color);
void draw_str(u8 *scr, int x, int y, const char *s, u8 color);
void draw_centered(u8 *scr, int y, const char *s, u8 color);
void draw_hline(u8 *scr, int y, int x1, int x2, u8 color);
int  str_len(const char *s);
int  is_rom_file(const char *name);
void extract_rom_name(const char *fn, char *out, int max);
void scale_to_framebuf(u32 *fb, const u8 *nes_screen, u8 ppu_mask);

#endif