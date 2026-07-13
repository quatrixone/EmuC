/* ps1_core.c - PlayStation 1 libretro core for PS5 EmuC
 * MIPS R3000A CPU, simplified GPU, SPU stub, PS-EXE / BIOS HLE loader.
 * No libc. Position-independent. _core_start at .text._core_start.
 */
#include "../src/core.h"
#include "../src/libretro.h"

/* ── Forward declarations ──────────────────────────────────────────────── */
static void ps1_init(void);
static void ps1_deinit(void);
static void ps1_set_env(retro_environment_t cb);
static void ps1_set_video(retro_video_refresh_t cb);
static void ps1_set_audio(retro_audio_sample_t cb);
static void ps1_set_audio_batch(retro_audio_sample_batch_t cb);
static void ps1_set_input_poll(retro_input_poll_t cb);
static void ps1_set_input_state(retro_input_state_t cb);
static void ps1_get_sysinfo(struct retro_system_info *i);
static void ps1_get_avinfo(struct retro_system_av_info *i);
static int  ps1_load_game(const struct retro_game_info *g);
static void ps1_unload(void);
static void ps1_run(void);
static void ps1_reset(void);

/* ── Core entry point ──────────────────────────────────────────────────── */
__attribute__((section(".text._core_start")))
void _core_start(u64 base, struct core_header *out) {
    out->magic = CORE_MAGIC; out->version = CORE_VERSION;
#define OFF(fn) (u32)((u64)(fn) - base)
    out->retro_init_off                   = OFF(ps1_init);
    out->retro_deinit_off                 = OFF(ps1_deinit);
    out->retro_set_environment_off        = OFF(ps1_set_env);
    out->retro_set_video_refresh_off      = OFF(ps1_set_video);
    out->retro_set_audio_sample_off       = OFF(ps1_set_audio);
    out->retro_set_audio_sample_batch_off = OFF(ps1_set_audio_batch);
    out->retro_set_input_poll_off         = OFF(ps1_set_input_poll);
    out->retro_set_input_state_off        = OFF(ps1_set_input_state);
    out->retro_get_system_info_off        = OFF(ps1_get_sysinfo);
    out->retro_get_system_av_info_off     = OFF(ps1_get_avinfo);
    out->retro_load_game_off              = OFF(ps1_load_game);
    out->retro_unload_game_off            = OFF(ps1_unload);
    out->retro_run_off                    = OFF(ps1_run);
    out->retro_reset_off                  = OFF(ps1_reset);
#undef OFF
}

/* ═══════════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════ */
#define PS1_W        320
#define PS1_H        240
#define PS1_SR       44100
#define CPU_HZ       33868800u  /* 33.8688 MHz */
#define CYCLES_FRAME (CPU_HZ / 60)

/* Memory sizes */
#define RAM_SIZE   (2*1024*1024)
#define VRAM_SIZE  (1024*512)      /* 1 MB (1024x512 pixels 16bpp) */
#define SPU_SIZE   (512*1024)
#define SCRATCHPAD 0x1F800000u

/* I/O ranges */
#define IO_BASE  0x1F801000u

/* PS1 VRAM coordinates: display area typically 0x0,0x0 to 320x240 */
#define VRAM_W   1024
#define VRAM_H   512

/* ═══════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════ */
static retro_video_refresh_t       g_video_cb;
static retro_audio_sample_t        g_audio_cb;
static retro_audio_sample_batch_t  g_abatch_cb;
static retro_input_poll_t          g_input_poll_cb;
static retro_input_state_t         g_input_state_cb;
static retro_environment_t         g_env_cb;

static const u8 *g_exe_data;
static u64       g_exe_size;

/* System memory */
static u8  g_ram[RAM_SIZE];
static u16 g_vram[VRAM_SIZE / 2];   /* VRAM: 16bpp words */
static u8  g_spu_ram[SPU_SIZE];
static u8  g_scratchpad[0x400];
static u8  g_io_mem[0x2000];        /* I/O space 0x1F801000-0x1F802FFF */

/* Framebuffer (XRGB8888) */
static u32 g_fb[PS1_W * PS1_H];

/* GPU state */
static u32 g_gpu_status;
static u16 g_disp_ox, g_disp_oy;   /* display start in VRAM */
static u16 g_disp_w, g_disp_h;
static u16 g_draw_x1, g_draw_y1, g_draw_x2, g_draw_y2; /* drawing area */
static s16 g_draw_ox, g_draw_oy;    /* drawing offset */
static u32 g_gpu_fifo[16];
static int g_gpu_fifo_cnt;
static int g_gpu_cmd_words;         /* remaining words for current GP0 command */
static u32 g_gpu_cmd;

/* MIPS R3000A CPU */
typedef struct {
    u32 r[32];
    u32 pc, hi, lo;
    u32 sr;       /* cop0 r12: status */
    u32 cause;    /* cop0 r13 */
    u32 epc;      /* cop0 r14 */
    u32 badvaddr; /* cop0 r8 */
    int ld_delay_slot; /* load delay slot: register index */
    u32 ld_delay_val;  /* value to write after delay */
    int bd_slot;       /* in branch delay slot */
    u32 bd_pc;         /* PC to restore for exceptions */
    int halted;
} R3000A;
static R3000A g_cpu;

/* ═══════════════════════════════════════════════════════════════════════
 * UTILITIES
 * ═══════════════════════════════════════════════════════════════════════ */
static void mem_zero(void *p, u64 n) { u8 *b=p; while(n--)*b++=0; }
static void mem_copy(void *d, const void *s, u64 n) { u8 *dd=d; const u8 *ss=s; while(n--)*dd++=*ss++; }

static u32 read_le32(const u8 *p) {
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static u16 read_le16(const u8 *p) {
    return (u16)p[0]|((u16)p[1]<<8);
}

/* ═══════════════════════════════════════════════════════════════════════
 * MEMORY BUS
 * ═══════════════════════════════════════════════════════════════════════ */
/* Physical address: strip the top 3 bits (KSEG0/KSEG1 mirrors) */
static u32 phys(u32 va) { return va & 0x1FFFFFFFu; }

static u32 bus_r32(u32 vaddr);
static u16 bus_r16(u32 vaddr);
static u8  bus_r8 (u32 vaddr);
static void bus_w32(u32 vaddr, u32 v);
static void bus_w16(u32 vaddr, u16 v);
static void bus_w8 (u32 vaddr, u8  v);

/* GPU registers */
static u32 gpu_r(u32 off);
static void gpu_w32(u32 off, u32 v);

/* Joypad controller (simplified digital pad) */
static u16 joy_read_data(void) {
    if (!g_input_state_cb) return 0x5A00;
    /* PSX digital pad: active-low, returns two bytes after 0xFF */
    u16 btn = 0xFFFF;
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT)) btn &= ~(1<<0);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START))  btn &= ~(1<<3);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP))     btn &= ~(1<<4);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT))  btn &= ~(1<<5);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN))   btn &= ~(1<<6);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT))   btn &= ~(1<<7);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2))     btn &= ~(1<<8);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2))     btn &= ~(1<<9);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L))      btn &= ~(1<<10);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R))      btn &= ~(1<<11);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X))      btn &= ~(1<<12);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A))      btn &= ~(1<<13);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B))      btn &= ~(1<<14);
    if (g_input_state_cb(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y))      btn &= ~(1<<15);
    return btn;
}

static u32 bus_r32(u32 vaddr) {
    u32 pa = phys(vaddr);
    if (pa < RAM_SIZE) {
        return (u32)g_ram[pa]|((u32)g_ram[pa+1]<<8)|((u32)g_ram[pa+2]<<16)|((u32)g_ram[pa+3]<<24);
    }
    if (pa == SCRATCHPAD || (pa >= SCRATCHPAD && pa < SCRATCHPAD+0x400)) {
        u32 off=pa-SCRATCHPAD; return (u32)g_scratchpad[off]|((u32)g_scratchpad[off+1]<<8)|((u32)g_scratchpad[off+2]<<16)|((u32)g_scratchpad[off+3]<<24);
    }
    if (pa >= IO_BASE && pa < IO_BASE+0x2000) return gpu_r(pa - IO_BASE);
    if (pa >= 0x1F000000u && pa < 0x1F800000u) return 0; /* expansion */
    return 0;
}
static u16 bus_r16(u32 vaddr) {
    u32 pa=phys(vaddr);
    if (pa < RAM_SIZE) return (u16)g_ram[pa]|((u16)g_ram[pa+1]<<8);
    if (pa >= SCRATCHPAD && pa < SCRATCHPAD+0x400) { u32 o=pa-SCRATCHPAD; return (u16)g_scratchpad[o]|((u16)g_scratchpad[o+1]<<8); }
    if (pa >= IO_BASE && pa < IO_BASE+0x2000) return (u16)gpu_r(pa - IO_BASE);
    return 0;
}
static u8 bus_r8(u32 vaddr) {
    u32 pa=phys(vaddr);
    if (pa < RAM_SIZE) return g_ram[pa];
    if (pa >= SCRATCHPAD && pa < SCRATCHPAD+0x400) return g_scratchpad[pa-SCRATCHPAD];
    if (pa >= IO_BASE && pa < IO_BASE+0x2000) return (u8)gpu_r(pa - IO_BASE);
    return 0;
}
static void bus_w32(u32 vaddr, u32 v) {
    u32 pa=phys(vaddr);
    if (pa < RAM_SIZE) { g_ram[pa]=(u8)v; g_ram[pa+1]=(u8)(v>>8); g_ram[pa+2]=(u8)(v>>16); g_ram[pa+3]=(u8)(v>>24); return; }
    if (pa >= SCRATCHPAD && pa < SCRATCHPAD+0x400) { u32 o=pa-SCRATCHPAD; g_scratchpad[o]=(u8)v; g_scratchpad[o+1]=(u8)(v>>8); g_scratchpad[o+2]=(u8)(v>>16); g_scratchpad[o+3]=(u8)(v>>24); return; }
    if (pa >= IO_BASE && pa < IO_BASE+0x2000) { gpu_w32(pa - IO_BASE, v); return; }
}
static void bus_w16(u32 vaddr, u16 v) {
    u32 pa=phys(vaddr);
    if (pa < RAM_SIZE) { g_ram[pa]=(u8)v; g_ram[pa+1]=(u8)(v>>8); return; }
    if (pa >= SCRATCHPAD && pa < SCRATCHPAD+0x400) { u32 o=pa-SCRATCHPAD; g_scratchpad[o]=(u8)v; g_scratchpad[o+1]=(u8)(v>>8); return; }
    if (pa >= IO_BASE && pa < IO_BASE+0x2000) { gpu_w32(pa - IO_BASE, (u32)v); return; }
}
static void bus_w8(u32 vaddr, u8 v) {
    u32 pa=phys(vaddr);
    if (pa < RAM_SIZE) { g_ram[pa]=v; return; }
    if (pa >= SCRATCHPAD && pa < SCRATCHPAD+0x400) { g_scratchpad[pa-SCRATCHPAD]=v; return; }
}

/* ═══════════════════════════════════════════════════════════════════════
 * GPU
 * ═══════════════════════════════════════════════════════════════════════ */
/* Convert BGR555 → XRGB8888 */
static u32 ps1_color(u16 c) {
    if (!c) return 0; /* transparent */
    u32 r=(c&0x1F)<<3, g=((c>>5)&0x1F)<<3, b=((c>>10)&0x1F)<<3;
    return (r<<16)|(g<<8)|b;
}

static void vram_put_pixel(int x, int y, u16 c) {
    if (x<0||x>=VRAM_W||y<0||y>=VRAM_H) return;
    g_vram[y*VRAM_W+x] = c;
}
static u16 vram_get_pixel(int x, int y) {
    if (x<0||x>=VRAM_W||y<0||y>=VRAM_H) return 0;
    return g_vram[y*VRAM_W+x];
}

/* Flat-shaded triangle rasterizer (top-left fill convention) */
static void gpu_tri(int x0,int y0,int x1,int y1,int x2,int y2,u16 color) {
    /* Sort vertices top to bottom */
    if (y1 < y0) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
    if (y2 < y0) { int t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
    if (y2 < y1) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    int dy01=y1-y0, dy02=y2-y0, dy12=y2-y1;
    int dx01=x1-x0, dx02=x2-x0, dx12=x2-x1;
    for (int y=y0; y<=y2; y++) {
        int ey = y - y0;
        int lx, rx;
        /* Left edge: 0->2 */
        lx = dy02 ? x0 + dx02*ey/dy02 : x0;
        /* Right edge */
        if (y < y1) rx = dy01 ? x0 + dx01*ey/dy01 : x0;
        else { int ey2=y-y1; rx = dy12 ? x1 + dx12*ey2/dy12 : x1; }
        if (lx > rx) { int t=lx; lx=rx; rx=t; }
        int ax=lx+g_draw_ox, ay=y+g_draw_oy;
        for (int x=lx; x<=rx; x++)
            vram_put_pixel(ax+(x-lx), ay, color);
    }
}

/* Draw rectangle */
static void gpu_rect(int x, int y, int w, int h, u16 color) {
    x += g_draw_ox; y += g_draw_oy;
    for (int py=y; py<y+h; py++)
        for (int px=x; px<x+w; px++)
            vram_put_pixel(px, py, color);
}

/* Line */
static void gpu_line(int x0,int y0,int x1,int y1,u16 color) {
    x0+=g_draw_ox; y0+=g_draw_oy; x1+=g_draw_ox; y1+=g_draw_oy;
    int dx=x1-x0>0?x1-x0:x0-x1, dy=y1-y0>0?y1-y0:y0-y1;
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy;
    while(1){
        vram_put_pixel(x0,y0,color);
        if(x0==x1&&y0==y1) break;
        int e2=err*2;
        if(e2>-dy){err-=dy;x0+=sx;}
        if(e2<dx) {err+=dx;y0+=sy;}
    }
}

/* GP0 packet dispatch */
static void gpu_gp0_exec(void) {
    u32 cmd  = g_gpu_fifo[0] >> 24;
    u32 color16 = 0;
    /* Build BGR555 from FIFO[0] RGB888 */
    {
        u32 rgb = g_gpu_fifo[0] & 0xFFFFFF;
        u32 r=(rgb>>3)&0x1F, gr=((rgb>>8)>>3)&0x1F, b=((rgb>>16)>>3)&0x1F;
        color16 = (u16)(r|(gr<<5)|(b<<10));
    }
    switch(cmd){
    case 0x00: break; /* NOP */
    case 0x01: /* clear texture cache - ignore */ break;
    case 0x02: { /* fill rect in VRAM */
        int x=(g_gpu_fifo[1])&0x3F0, y=(g_gpu_fifo[1]>>16)&0x1FF;
        int w=((g_gpu_fifo[2])&0x3FF+15)&~0xF, h=(g_gpu_fifo[2]>>16)&0x1FF;
        gpu_rect(x-g_draw_ox, y-g_draw_oy, w, h, color16);
        break;
    }
    case 0x20: case 0x21: case 0x22: case 0x23: { /* flat shaded tri */
        int x0=(s16)(g_gpu_fifo[1]&0xFFFF), y0=(s16)(g_gpu_fifo[1]>>16);
        int x1=(s16)(g_gpu_fifo[2]&0xFFFF), y1=(s16)(g_gpu_fifo[2]>>16);
        int x2=(s16)(g_gpu_fifo[3]&0xFFFF), y2=(s16)(g_gpu_fifo[3]>>16);
        gpu_tri(x0,y0,x1,y1,x2,y2,color16); break;
    }
    case 0x28: case 0x29: case 0x2A: case 0x2B: { /* flat shaded quad */
        int x0=(s16)(g_gpu_fifo[1]&0xFFFF), y0=(s16)(g_gpu_fifo[1]>>16);
        int x1=(s16)(g_gpu_fifo[2]&0xFFFF), y1=(s16)(g_gpu_fifo[2]>>16);
        int x2=(s16)(g_gpu_fifo[3]&0xFFFF), y2=(s16)(g_gpu_fifo[3]>>16);
        int x3=(s16)(g_gpu_fifo[4]&0xFFFF), y3=(s16)(g_gpu_fifo[4]>>16);
        gpu_tri(x0,y0,x1,y1,x2,y2,color16);
        gpu_tri(x1,y1,x2,y2,x3,y3,color16); break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: { /* monochrome line */
        int x0=(s16)(g_gpu_fifo[1]&0xFFFF), y0=(s16)(g_gpu_fifo[1]>>16);
        int x1=(s16)(g_gpu_fifo[2]&0xFFFF), y1=(s16)(g_gpu_fifo[2]>>16);
        gpu_line(x0,y0,x1,y1,color16); break;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: { /* flat rect variable size */
        int x=(s16)(g_gpu_fifo[1]&0xFFFF), y=(s16)(g_gpu_fifo[1]>>16);
        int w=(s16)(g_gpu_fifo[2]&0xFFFF), h=(s16)(g_gpu_fifo[2]>>16);
        gpu_rect(x,y,w,h,color16); break;
    }
    case 0x68: { /* dot 1x1 */
        int x=(s16)(g_gpu_fifo[1]&0xFFFF), y=(s16)(g_gpu_fifo[1]>>16);
        vram_put_pixel(x+g_draw_ox, y+g_draw_oy, color16); break;
    }
    case 0x78: { /* rect 8x8 */
        int x=(s16)(g_gpu_fifo[1]&0xFFFF), y=(s16)(g_gpu_fifo[1]>>16);
        gpu_rect(x,y,8,8,color16); break;
    }
    case 0x7C: { /* rect 16x16 */
        int x=(s16)(g_gpu_fifo[1]&0xFFFF), y=(s16)(g_gpu_fifo[1]>>16);
        gpu_rect(x,y,16,16,color16); break;
    }
    case 0x80: { /* copy rect VRAM-to-VRAM */
        int sx=(g_gpu_fifo[1])&0x3FF, sy=(g_gpu_fifo[1]>>16)&0x1FF;
        int dx=(g_gpu_fifo[2])&0x3FF, dy=(g_gpu_fifo[2]>>16)&0x1FF;
        int w=(g_gpu_fifo[3])&0x3FF,  h=(g_gpu_fifo[3]>>16)&0x1FF;
        for(int row=0;row<h;row++)
            for(int col=0;col<w;col++)
                vram_put_pixel(dx+col,dy+row,vram_get_pixel(sx+col,sy+row));
        break;
    }
    case 0xA0: { /* CPU-to-VRAM transfer (handled via multi-word writes elsewhere) */ break; }
    case 0xC0: { /* VRAM-to-CPU transfer */ break; }
    case 0xE1: /* draw mode setting */ g_gpu_status=(g_gpu_status&~0x7FF)|(g_gpu_fifo[0]&0x7FF); break;
    case 0xE2: break; /* texture window */
    case 0xE3: {
        g_draw_x1=(u16)(g_gpu_fifo[0]&0x3FF);
        g_draw_y1=(u16)((g_gpu_fifo[0]>>10)&0x1FF);
        break;
    }
    case 0xE4: {
        g_draw_x2=(u16)(g_gpu_fifo[0]&0x3FF);
        g_draw_y2=(u16)((g_gpu_fifo[0]>>10)&0x1FF);
        break;
    }
    case 0xE5: {
        g_draw_ox=(s16)(((s32)(g_gpu_fifo[0]&0x7FF)<<21)>>21);
        g_draw_oy=(s16)(((s32)((g_gpu_fifo[0]>>11)&0x7FF)<<21)>>21);
        break;
    }
    case 0xE6: break; /* mask bit setting */
    default: break;
    }
    g_gpu_fifo_cnt = 0;
    g_gpu_cmd_words = 0;
}

/* GP0 word sizes by command byte */
static int gp0_cmd_size(u32 cmd) {
    switch(cmd) {
    case 0x00: case 0x01: return 1;
    case 0x02: return 3;
    case 0x20: case 0x21: return 4;
    case 0x22: case 0x23: return 4;
    case 0x28: case 0x29: return 5;
    case 0x2A: case 0x2B: return 5;
    case 0x30: case 0x31: return 6; /* gouraud tri (simplified) */
    case 0x38: case 0x39: return 8; /* gouraud quad */
    case 0x40: case 0x41: case 0x42: case 0x43: return 3;
    case 0x48: return 4; /* polyline (simplified) */
    case 0x60: case 0x61: case 0x62: case 0x63: return 3;
    case 0x68: return 2;
    case 0x78: case 0x7C: return 2;
    case 0x80: return 4;
    case 0xA0: return 3;
    case 0xC0: return 3;
    case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: return 1;
    default: return 1;
    }
}

static void gpu_gp0_write(u32 v) {
    if (g_gpu_cmd_words == 0) {
        g_gpu_fifo_cnt = 0;
        g_gpu_cmd = v >> 24;
        g_gpu_cmd_words = gp0_cmd_size(g_gpu_cmd);
    }
    if (g_gpu_fifo_cnt < 16) g_gpu_fifo[g_gpu_fifo_cnt++] = v;
    g_gpu_cmd_words--;
    if (g_gpu_cmd_words <= 0) gpu_gp0_exec();
}

static void gpu_gp1_write(u32 v) {
    u32 cmd = v >> 24;
    switch(cmd) {
    case 0x00: /* Reset GPU */
        g_gpu_status = 0x14802000u;
        g_disp_ox=0; g_disp_oy=0;
        g_draw_ox=0; g_draw_oy=0;
        g_draw_x1=0; g_draw_y1=0;
        g_draw_x2=PS1_W-1; g_draw_y2=PS1_H-1;
        break;
    case 0x01: g_gpu_fifo_cnt=0; g_gpu_cmd_words=0; break;
    case 0x02: g_gpu_status &= ~(1<<23); break; /* ACK IRQ */
    case 0x03: if(v&1) g_gpu_status&=~(1<<23); else g_gpu_status|=(1<<23); break; /* display enable */
    case 0x04: g_gpu_status=(g_gpu_status&~0x60000000u)|(((v&3)^2)<<29); break; /* DMA direction */
    case 0x05: g_disp_ox=(u16)(v&0x3FF); g_disp_oy=(u16)((v>>10)&0x1FF); break;
    case 0x06: break; /* h display range */
    case 0x07: break; /* v display range */
    case 0x08: { /* display mode */
        g_gpu_status=(g_gpu_status&~0x7F4000u)|(v&0x3F)<<17|(v&0x40)<<10;
        /* Width select */
        u32 hres=(v&3);
        static const u16 ws[4]={256,320,512,640};
        g_disp_w=ws[hres]; g_disp_h=(v&4)?480:240;
        break;
    }
    case 0x10: break; /* GPU info */
    default: break;
    }
}

static u32 gpu_r(u32 off) {
    if (off == 0x810) return 0; /* GPUREAD */
    if (off == 0x814) return g_gpu_status;
    if (off >= 0xC80 && off < 0xD00) return joy_read_data(); /* JOY_DATA */
    if (off >= 0xD80 && off < 0xD84) return 0x5; /* JOY_STAT: TX_RDY | RX_RDY */
    if (off >= 0x1070 && off < 0x1078) return 0; /* I_STAT / I_MASK */
    /* CDROM, SPU, etc. - stubs */
    return 0;
}
static void gpu_w32(u32 off, u32 v) {
    if (off == 0x810) { gpu_gp0_write(v); return; }
    if (off == 0x814) { gpu_gp1_write(v); return; }
    if (off >= 0x1070 && off < 0x1078) return; /* I_STAT / I_MASK */
    /* DMA, timers, etc. - stubs */
}

/* ═══════════════════════════════════════════════════════════════════════
 * MIPS R3000A CPU
 * ═══════════════════════════════════════════════════════════════════════ */
/* Exception handling */
static void raise_exception(u32 code, u32 bad) {
    g_cpu.epc = g_cpu.bd_slot ? g_cpu.bd_pc : g_cpu.pc - 4;
    g_cpu.cause = (code << 2) | (g_cpu.bd_slot ? (1u<<31) : 0);
    g_cpu.badvaddr = bad;
    g_cpu.sr = (g_cpu.sr & ~0x3Fu) | ((g_cpu.sr & 0xF) << 2);
    g_cpu.pc = (g_cpu.sr & (1<<22)) ? 0xBFC00180u : 0x80000080u;
    g_cpu.bd_slot = 0;
}

/* BIOS HLE: intercept common BIOS calls to avoid needing the BIOS ROM */
static int bios_hle(u32 pc) {
    u32 func = g_cpu.r[9]; /* t1 = function number */
    if (pc == 0xA0) {
        switch(func) {
        case 0x3C: /* putchar */
        case 0x3E: /* puts */ return 1;
        case 0x40: /* sys_exit */ g_cpu.halted=1; return 1;
        default: return 0;
        }
    }
    if (pc == 0xB0) {
        switch(func) {
        case 0x3D: /* putchar */ return 1;
        default: return 0;
        }
    }
    if (pc == 0xC0) return 0;
    return 0;
}

#define RS  ((ins>>21)&0x1F)
#define RT  ((ins>>16)&0x1F)
#define RD  ((ins>>11)&0x1F)
#define SA  ((ins>>6)&0x1F)
#define IMM ((s32)(s16)(ins&0xFFFF))
#define IMMU ((u32)(u16)(ins&0xFFFF))
#define TARGET ((ins&0x3FFFFFFu)<<2)
#define FNCODE (ins&0x3F)
#define RSVRS g_cpu.r[RS]
#define RSVRT g_cpu.r[RT]

/* Returns cycles used */
static int cpu_step(void) {
    u32 pc = g_cpu.pc;
    g_cpu.bd_slot = 0;

    /* Check BIOS HLE */
    if (pc == 0xA0 || pc == 0xB0 || pc == 0xC0) {
        if (bios_hle(pc)) {
            g_cpu.pc = g_cpu.r[31]; /* return address in $ra */
            return 8;
        }
    }

    u32 ins = bus_r32(pc);
    g_cpu.pc += 4;

    /* Apply load delay slot value from previous cycle */
    if (g_cpu.ld_delay_slot) {
        if (g_cpu.r[g_cpu.ld_delay_slot] == 0xDEADBEEFu) /* not overwritten */
            g_cpu.r[g_cpu.ld_delay_slot] = g_cpu.ld_delay_val;
        g_cpu.ld_delay_slot = 0;
    }
    g_cpu.r[0] = 0; /* r0 always 0 */

    u32 op = ins >> 26;
    switch(op) {
    case 0x00: { /* SPECIAL */
        switch(FNCODE) {
        case 0x00: if(SA) g_cpu.r[RD]=RSVRT<<SA; break; /* SLL */
        case 0x02: g_cpu.r[RD]=(s32)RSVRT>>SA; break; /* SRL: actually logical */
        case 0x03: g_cpu.r[RD]=(s32)((s32)RSVRT>>SA); break; /* SRA */
        case 0x04: g_cpu.r[RD]=RSVRT<<(RSVRS&0x1F); break; /* SLLV */
        case 0x06: g_cpu.r[RD]=RSVRT>>(RSVRS&0x1F); break; /* SRLV */
        case 0x07: g_cpu.r[RD]=(u32)((s32)RSVRT>>(s32)(RSVRS&0x1F)); break; /* SRAV */
        case 0x08: { /* JR */
            u32 tgt=RSVRS; g_cpu.bd_slot=1; g_cpu.bd_pc=pc;
            u32 di=bus_r32(g_cpu.pc); g_cpu.pc+=4;
            (void)di; /* execute delay slot - simplified: skip */
            g_cpu.pc=tgt; break;
        }
        case 0x09: { /* JALR */
            u32 tgt=RSVRS; g_cpu.r[RD]=g_cpu.pc+4; g_cpu.bd_slot=1; g_cpu.bd_pc=pc;
            g_cpu.pc=tgt; break;
        }
        case 0x0C: raise_exception(8,0); break; /* SYSCALL */
        case 0x0D: raise_exception(9,0); break; /* BREAK */
        case 0x10: g_cpu.r[RD]=g_cpu.hi; break; /* MFHI */
        case 0x11: g_cpu.hi=RSVRS; break; /* MTHI */
        case 0x12: g_cpu.r[RD]=g_cpu.lo; break; /* MFLO */
        case 0x13: g_cpu.lo=RSVRS; break; /* MTLO */
        case 0x18: { /* MULT */ s64 r=(s64)(s32)RSVRS*(s64)(s32)RSVRT; g_cpu.lo=(u32)r; g_cpu.hi=(u32)(r>>32); break; }
        case 0x19: { /* MULTU */ u64 r=(u64)RSVRS*(u64)RSVRT; g_cpu.lo=(u32)r; g_cpu.hi=(u32)(r>>32); break; }
        case 0x1A: { /* DIV */ if(RSVRT){g_cpu.lo=(u32)((s32)RSVRS/(s32)RSVRT);g_cpu.hi=(u32)((s32)RSVRS%(s32)RSVRT);} break; }
        case 0x1B: { /* DIVU */ if(RSVRT){g_cpu.lo=RSVRS/RSVRT;g_cpu.hi=RSVRS%RSVRT;} break; }
        case 0x20: g_cpu.r[RD]=(u32)((s32)RSVRS+(s32)RSVRT); break; /* ADD */
        case 0x21: g_cpu.r[RD]=RSVRS+RSVRT; break; /* ADDU */
        case 0x22: g_cpu.r[RD]=(u32)((s32)RSVRS-(s32)RSVRT); break; /* SUB */
        case 0x23: g_cpu.r[RD]=RSVRS-RSVRT; break; /* SUBU */
        case 0x24: g_cpu.r[RD]=RSVRS&RSVRT; break; /* AND */
        case 0x25: g_cpu.r[RD]=RSVRS|RSVRT; break; /* OR */
        case 0x26: g_cpu.r[RD]=RSVRS^RSVRT; break; /* XOR */
        case 0x27: g_cpu.r[RD]=~(RSVRS|RSVRT); break; /* NOR */
        case 0x2A: g_cpu.r[RD]=(s32)RSVRS<(s32)RSVRT?1:0; break; /* SLT */
        case 0x2B: g_cpu.r[RD]=RSVRS<RSVRT?1:0; break; /* SLTU */
        default: break;
        }
        break;
    }
    case 0x01: { /* REGIMM */
        u32 sub=(ins>>16)&0x1F;
        s32 off=IMM<<2;
        int take=0;
        if (sub==0x00) take=(s32)RSVRS<0;       /* BLTZ */
        else if(sub==0x01) take=(s32)RSVRS>=0;  /* BGEZ */
        else if(sub==0x10) { take=(s32)RSVRS<0;  g_cpu.r[31]=g_cpu.pc+4; } /* BLTZAL */
        else if(sub==0x11) { take=(s32)RSVRS>=0; g_cpu.r[31]=g_cpu.pc+4; } /* BGEZAL */
        if(take){ g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc+=off; }
        break;
    }
    case 0x02: { /* J */ g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc=(g_cpu.pc&0xF0000000u)|(TARGET); break; }
    case 0x03: { /* JAL */ g_cpu.r[31]=g_cpu.pc+4; g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc=(g_cpu.pc&0xF0000000u)|(TARGET); break; }
    case 0x04: { /* BEQ */ if(RSVRS==RSVRT){ g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc+=IMM<<2; } break; }
    case 0x05: { /* BNE */ if(RSVRS!=RSVRT){ g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc+=IMM<<2; } break; }
    case 0x06: { /* BLEZ */ if((s32)RSVRS<=0){ g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc+=IMM<<2; } break; }
    case 0x07: { /* BGTZ */ if((s32)RSVRS>0){ g_cpu.bd_slot=1; g_cpu.bd_pc=pc; g_cpu.pc+=IMM<<2; } break; }
    case 0x08: g_cpu.r[RT]=(u32)((s32)RSVRS+IMM); break; /* ADDI */
    case 0x09: g_cpu.r[RT]=RSVRS+(u32)(s32)IMM; break;  /* ADDIU */
    case 0x0A: g_cpu.r[RT]=(s32)RSVRS<IMM?1:0; break;   /* SLTI */
    case 0x0B: g_cpu.r[RT]=RSVRS<IMMU?1:0; break;        /* SLTIU */
    case 0x0C: g_cpu.r[RT]=RSVRS&IMMU; break;  /* ANDI */
    case 0x0D: g_cpu.r[RT]=RSVRS|IMMU; break;  /* ORI */
    case 0x0E: g_cpu.r[RT]=RSVRS^IMMU; break;  /* XORI */
    case 0x0F: g_cpu.r[RT]=IMMU<<16; break;     /* LUI */
    case 0x10: { /* COP0 */
        u32 cop=(ins>>21)&0x1F;
        if(cop==0x00) { /* MFC0 */
            u32 cn=RD;
            if(cn==12) g_cpu.r[RT]=g_cpu.sr;
            else if(cn==13) g_cpu.r[RT]=g_cpu.cause;
            else if(cn==14) g_cpu.r[RT]=g_cpu.epc;
        } else if(cop==0x04) { /* MTC0 */
            u32 cn=RD;
            if(cn==12) g_cpu.sr=RSVRT;
        } else if(cop==0x10 && (ins&0x3F)==0x10) { /* RFE */
            g_cpu.sr=(g_cpu.sr&~0xF)|((g_cpu.sr>>2)&0xF);
        }
        break;
    }
    case 0x20: { /* LB */ u32 a=RSVRS+(u32)(s32)IMM; g_cpu.r[RT]=(u32)(s32)(s8)bus_r8(a); break; }
    case 0x21: { /* LH */ u32 a=RSVRS+(u32)(s32)IMM; g_cpu.r[RT]=(u32)(s32)(s16)bus_r16(a); break; }
    case 0x22: { /* LWL */ u32 a=RSVRS+(u32)(s32)IMM; u32 sh=(a&3)*8; u32 mask=0xFFFFFFFFu<<sh; g_cpu.r[RT]=(g_cpu.r[RT]&~mask)|(bus_r32(a&~3u)<<sh); break; }
    case 0x23: { /* LW */ u32 a=RSVRS+(u32)(s32)IMM; g_cpu.r[RT]=bus_r32(a); break; }
    case 0x24: { /* LBU */ u32 a=RSVRS+(u32)(s32)IMM; g_cpu.r[RT]=bus_r8(a); break; }
    case 0x25: { /* LHU */ u32 a=RSVRS+(u32)(s32)IMM; g_cpu.r[RT]=(u32)bus_r16(a); break; }
    case 0x26: { /* LWR */ u32 a=RSVRS+(u32)(s32)IMM; u32 sh=(3-(a&3))*8; u32 mask=0xFFFFFFFFu>>sh; g_cpu.r[RT]=(g_cpu.r[RT]&~mask)|(bus_r32(a&~3u)>>sh); break; }
    case 0x28: { /* SB */ u32 a=RSVRS+(u32)(s32)IMM; bus_w8(a,(u8)RSVRT); break; }
    case 0x29: { /* SH */ u32 a=RSVRS+(u32)(s32)IMM; bus_w16(a,(u16)RSVRT); break; }
    case 0x2A: { /* SWL */ u32 a=RSVRS+(u32)(s32)IMM; u32 sh=(a&3)*8; u32 mask=0xFFFFFFFFu>>sh; u32 old=bus_r32(a&~3u); bus_w32(a&~3u,(old&~mask)|(RSVRT>>sh)); break; }
    case 0x2B: { /* SW */ u32 a=RSVRS+(u32)(s32)IMM; bus_w32(a,RSVRT); break; }
    case 0x2E: { /* SWR */ u32 a=RSVRS+(u32)(s32)IMM; u32 sh=(3-(a&3))*8; u32 mask=0xFFFFFFFFu<<sh; u32 old=bus_r32(a&~3u); bus_w32(a&~3u,(old&~mask)|(RSVRT<<sh)); break; }
    default: break;
    }
    g_cpu.r[0] = 0;
    return 4;
}

/* ═══════════════════════════════════════════════════════════════════════
 * FRAME BLIT
 * ═══════════════════════════════════════════════════════════════════════ */
static void blit_frame(void) {
    int w = g_disp_w ? g_disp_w : PS1_W;
    int h = g_disp_h ? g_disp_h : PS1_H;
    int ox = g_disp_ox, oy = g_disp_oy;
    for (int y = 0; y < PS1_H; y++) {
        int sy = oy + (y * h / PS1_H);
        for (int x = 0; x < PS1_W; x++) {
            int sx = ox + (x * w / PS1_W);
            g_fb[y * PS1_W + x] = ps1_color(vram_get_pixel(sx % VRAM_W, sy % VRAM_H));
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * PS-EXE LOADER
 * ═══════════════════════════════════════════════════════════════════════ */
static int ps_exe_load(const u8 *data, u64 size) {
    /* PS-EXE header: "PS-X EXE" at offset 0, then:
     * 0x10: PC (entry), 0x18: initial GP, 0x1C: load address, 0x20: file size
     * 0x24: data+BSS addr, 0x28: BSS size, 0x2C: initial SP */
    static const u8 magic[] = {'P','S','-','X',' ','E','X','E'};
    for (int i=0;i<8;i++) if (data[i]!=magic[i]) return 0;
    u32 pc   = read_le32(data+0x10);
    u32 gp   = read_le32(data+0x14);
    u32 dst  = read_le32(data+0x18);
    u32 flen = read_le32(data+0x1C);
    u32 sp   = read_le32(data+0x30);
    /* Copy code to RAM */
    u32 phys_dst = dst & 0x1FFFFFu;
    if (phys_dst + flen > RAM_SIZE) flen = RAM_SIZE - phys_dst;
    if (size < 0x800 + flen) flen = (u32)(size - 0x800);
    mem_copy(g_ram + phys_dst, data + 0x800, flen);
    g_cpu.pc    = pc;
    g_cpu.r[28] = gp;
    g_cpu.r[29] = sp ? sp : 0x801FFFF0u; /* $sp */
    g_cpu.r[30] = g_cpu.r[29];
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * RESET
 * ═══════════════════════════════════════════════════════════════════════ */
static void ps1_reset_hw(void) {
    mem_zero(&g_cpu, sizeof g_cpu);
    mem_zero(g_ram, RAM_SIZE);
    mem_zero(g_vram, VRAM_SIZE);
    mem_zero(g_spu_ram, SPU_SIZE);
    mem_zero(g_scratchpad, sizeof g_scratchpad);
    mem_zero(g_io_mem, sizeof g_io_mem);
    mem_zero(g_fb, sizeof g_fb);
    g_gpu_status = 0x14802000u;
    g_disp_ox=0; g_disp_oy=0; g_disp_w=PS1_W; g_disp_h=PS1_H;
    g_draw_ox=0; g_draw_oy=0;
    g_draw_x1=0; g_draw_y1=0; g_draw_x2=PS1_W-1; g_draw_y2=PS1_H-1;
    g_gpu_fifo_cnt=0; g_gpu_cmd_words=0;
    /* Default CPU state: SR=0x10000 (interrupts disabled), start at BIOS vector */
    g_cpu.sr = 0x10000;
    g_cpu.pc = 0xBFC00000u;
    /* Install HLE stub: JR $ra at BIOS entry points */
    /* A0 = 0xA0, B0 = 0xB0, C0 = 0xC0 (these are KSEG0 so masked to 0x20/0x30/0x40) */
    /* We'll just let bios_hle() handle it when PC hits these addresses */
    if (g_exe_data) ps_exe_load(g_exe_data, g_exe_size);
}

/* ═══════════════════════════════════════════════════════════════════════
 * AUDIO
 * ═══════════════════════════════════════════════════════════════════════ */
#define PS1_AUDIO_BUF 512
static s16 g_abuf[PS1_AUDIO_BUF * 2];
static void audio_push(void) {
    for (int i=0;i<PS1_AUDIO_BUF;i++) g_abuf[i*2]=g_abuf[i*2+1]=0;
    if (g_abatch_cb) g_abatch_cb(g_abuf, PS1_AUDIO_BUF);
}

/* ═══════════════════════════════════════════════════════════════════════
 * LIBRETRO API
 * ═══════════════════════════════════════════════════════════════════════ */
static void ps1_init(void)   {}
static void ps1_deinit(void) {}
static void ps1_set_env(retro_environment_t cb) {
    g_env_cb = cb;
    u32 fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
static void ps1_set_video(retro_video_refresh_t cb)      { g_video_cb = cb; }
static void ps1_set_audio(retro_audio_sample_t cb)       { g_audio_cb = cb; }
static void ps1_set_audio_batch(retro_audio_sample_batch_t cb) { g_abatch_cb = cb; }
static void ps1_set_input_poll(retro_input_poll_t cb)    { g_input_poll_cb = cb; }
static void ps1_set_input_state(retro_input_state_t cb)  { g_input_state_cb = cb; }

static void ps1_get_sysinfo(struct retro_system_info *i) {
    i->library_name    = "PS1 EmuC";
    i->library_version = "1.0";
    i->valid_extensions = "exe|psx";
    i->need_fullpath   = 0;
    i->block_extract   = 0;
}
static void ps1_get_avinfo(struct retro_system_av_info *i) {
    i->geometry.base_width   = PS1_W;
    i->geometry.base_height  = PS1_H;
    i->geometry.max_width    = PS1_W;
    i->geometry.max_height   = PS1_H;
    i->geometry.aspect_ratio = (float)PS1_W / PS1_H;
    i->timing.fps            = 59.94;
    i->timing.sample_rate    = PS1_SR;
}

static int ps1_load_game(const struct retro_game_info *g) {
    if (!g || !g->data || g->size < 8) return 0;
    g_exe_data = (const u8 *)g->data;
    g_exe_size = g->size;
    ps1_reset_hw();
    return 1;
}
static void ps1_unload(void) { g_exe_data=0; g_exe_size=0; }
static void ps1_reset(void)  { ps1_reset_hw(); }

static void ps1_run(void) {
    if (g_input_poll_cb) g_input_poll_cb();
    /* Run ~563200 cycles per frame (33.87 MHz / 60) */
    int cycles_per_frame = CYCLES_FRAME;
    int audio_div = cycles_per_frame / (PS1_SR / PS1_AUDIO_BUF);
    int audio_acc = 0;
    int vblank_cyc = cycles_per_frame * 240 / 263;
    int cy = 0;
    while (cy < cycles_per_frame) {
        if (g_cpu.halted) { cy += 4; audio_acc += 4; }
        else {
            int used = cpu_step();
            cy += used; audio_acc += used;
        }
        audio_acc += 0;
        if (audio_acc >= audio_div) {
            audio_acc -= audio_div;
            audio_push();
        }
        /* Trigger VBlank at ~240/263 of frame */
        if (cy == vblank_cyc) {
            blit_frame();
            if (g_video_cb)
                g_video_cb(g_fb, PS1_W, PS1_H, (u64)(PS1_W * 4));
            /* Signal VBlank via I_STAT */
            u32 istat = (u32)g_io_mem[0x70] | ((u32)g_io_mem[0x71]<<8) | ((u32)g_io_mem[0x72]<<16) | ((u32)g_io_mem[0x73]<<24);
            istat |= 1; /* VBlank */
            g_io_mem[0x70]=(u8)istat; g_io_mem[0x71]=(u8)(istat>>8);
        }
    }
}
