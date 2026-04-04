/* ps5_platform.c  –  PS5 platform backend reference for RetroArch orbis→PS5 port
 *
 * This file documents the PS5-specific API bindings that the EmulationStation
 * frontend (retro_main.c) and any future RetroArch PS5 port would use.
 *
 * Relationship to PS4 (orbis) RetroArch port:
 * ─────────────────────────────────────────────
 *   RetroArch already ships a full PS4 (orbis) platform driver under:
 *     frontend/drivers/platform_orbis.c
 *     input/drivers/orbis_input.c
 *     gfx/drivers/orbis_gfx.c
 *
 *   PS4 and PS5 share the same x86-64 architecture and Orbis OS (FreeBSD-based),
 *   so the orbis port is the closest starting point.  The differences are:
 *
 *   PS4 vs PS5 differences
 *   ─────────────────────────────────────────────────────────────────────────────
 *   Feature              PS4 (orbis)                  PS5 (this port)
 *   ─────────────────────────────────────────────────────────────────────────────
 *   Exploit type         Kernel exploit (WebKit/BPF)  LuaC0re userland JIT exploit
 *   Entry point          ELF loaded by exploit        _start(eboot_base, dlsym, ext)
 *   Symbol resolution    Standard dynamic linker       Custom dlsym via ROP gadget
 *   GADGET_OFFSET        Per-FW table (PS4 specific)  0x31AA9 (fw 13.00, Star Wars)
 *   EBOOT_GS_THREAD      0x–– (PS4 game)              0x057F89B0 (Star Wars Racer PS5)
 *   EBOOT_VIDOUT         0x–– (PS4 game)              0x02d695d0 (Star Wars Racer PS5)
 *   Controller           DualShock 4                  DualSense (different button bits)
 *   Video API            sceVideoOut* (same names)    sceVideoOut* (same, same handles)
 *   Audio API            sceAudioOut* (same names)    sceAudioOut* (same)
 *   Pad API              scePad* (same names)         scePad* (new button layout)
 *   Memory               sceKernelAllocateDirect...   same
 *   Modules              sceKernelLoadStartModule      same
 *
 * Key insight: since the API names are identical, the orbis gfx/audio drivers
 * can be reused almost verbatim.  Only the entry point mechanism and controller
 * input mapping need non-trivial changes.
 */

#include "core.h"
#include "libretro.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 – VIDEO BACKEND
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PS5 video output via libSceVideoOut.sprx
 * Matches RetroArch orbis_gfx.c function calls.
 */

/* Call sequence to open video output on PS5 ─────────────────────────────── */
/*
 * s32 ps5_video_open(void *G, void *D) {
 *
 *   // Load module
 *   s32 vid_mod = NC(G, load_mod, "libSceVideoOut.sprx", 0,0,0,0,0);
 *
 *   void *vid_open = SYM(G, D, vid_mod, "sceVideoOutOpen");
 *   void *vid_reg  = SYM(G, D, vid_mod, "sceVideoOutRegisterBuffers");
 *   void *vid_flip = SYM(G, D, vid_mod, "sceVideoOutSubmitFlip");
 *   void *vid_rate = SYM(G, D, vid_mod, "sceVideoOutSetFlipRate");
 *   void *vid_evt  = SYM(G, D, vid_mod, "sceVideoOutAddFlipEvent");
 *
 *   // Open video port: userId=0xFF (system), busType=0 (main), index=0
 *   s32 handle = NC(G, vid_open, 0xFF, 0, 0, 0, 0, 0);
 *
 *   // Allocate 2× framebuffers in direct GPU-accessible memory
 *   //   Each: SCR_W(1920) × SCR_H(1080) × 4 bytes = 8,294,400 bytes
 *   //   Aligned to 2 MB boundaries → FB_ALIGNED = 0x820000
 *   //   Total: FB_TOTAL = FB_ALIGNED * 2
 *   u64 phys = 0;
 *   NC(G, alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, &phys);
 *   void *vmem = NULL;
 *   NC(G, map_dm, &vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
 *   //   0x33 = PROT_READ|PROT_WRITE|MAP_SHARED flag for DirectMemory
 *
 *   void *fbs[2] = { vmem, (u8*)vmem + FB_ALIGNED };
 *
 *   // Register buffer attribute struct (64 bytes):
 *   u8 attr[64] = {0};
 *   *(u32*)(attr+ 0) = 0x80000000;  // pixelFormat = ABGR8888 (SCE_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB)
 *   *(u32*)(attr+ 4) = 1;           // tilingMode  = linear (SCE_VIDEO_OUT_TILING_MODE_LINEAR)
 *   *(u32*)(attr+12) = SCR_W;       // width
 *   *(u32*)(attr+16) = SCR_H;       // height
 *   *(u32*)(attr+20) = SCR_W;       // pitch (pixels per row)
 *
 *   NC(G, vid_reg, handle, 0, fbs, 2, attr, 0);
 *   NC(G, vid_rate, handle, 0, 0,0,0,0);  // 0 = every vsync = 60 Hz
 *
 *   // Create event queue for vsync synchronization
 *   u64 eq = 0;
 *   NC(G, create_eq, &eq, "esq", 0,0,0,0);
 *   NC(G, vid_evt, eq, handle, 0,0,0,0);
 *
 *   return handle;
 * }
 *
 * // Flip (present) a framebuffer:
 * //   bufferIndex: 0 or 1  (double-buffer)
 * //   flipMode:    1 = vsync
 * //   flipArg:     monotonically increasing frame counter
 * void ps5_video_flip(void *G, void *vid_flip, s32 handle,
 *                     int buf_idx, u64 frame_num) {
 *   NC(G, vid_flip, handle, buf_idx, 1, frame_num, 0, 0);
 * }
 *
 * // Wait for vsync via event queue (prevents tearing):
 * void ps5_video_wait(void *G, void *wait_eq, u64 eq) {
 *   u8 evt[64]; s32 cnt = 0;
 *   NC(G, wait_eq, eq, evt, 1, &cnt, 0, 0);
 * }
 */

/* Framebuffer pixel format note ─────────────────────────────────────────── *
 *   The PS5 framebuffer uses BGRA8888 (pixelFormat 0x80000000).
 *   libretro cores typically output XRGB8888.
 *   Conversion: PS5_pixel = (libretro_pixel & 0x00FFFFFF) swapped R↔B
 *               i.e.  out = ((in & 0x00FF0000) >> 16)        // R→B
 *                        | ((in & 0x0000FF00))                // G stays
 *                        | ((in & 0x000000FF) << 16)          // B→R
 *                        |  0xFF000000;                       // alpha=255
 *   In practice for a 1920×1080 upscale from core output (nearest neighbour):
 *     for each (x,y) in [0..1919][0..1079]:
 *       u32 sx = x * core_w / SCR_W;
 *       u32 sy = y * core_h / SCR_H;
 *       u32 px = src[sy * (pitch/4) + sx];
 *       fb[y * SCR_W + x] = swap_rb(px) | 0xFF000000;
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 – AUDIO BACKEND
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PS5 audio output via libSceAudioOut.sprx
 */

/*
 * s32 ps5_audio_open(void *G, void *D, s32 userId) {
 *
 *   s32 aud_mod = NC(G, load_mod, "libSceAudioOut.sprx", 0,0,0,0,0);
 *   // Also needed for userId:
 *   NC(G, load_mod, "libSceUserService.sprx", 0,0,0,0,0);
 *
 *   void *aud_close = SYM(G, D, aud_mod, "sceAudioOutClose");
 *   void *aud_open  = SYM(G, D, aud_mod, "sceAudioOutOpen");
 *
 *   // Close any handles that the game already has open (0..7)
 *   for (int h = 0; h < 8; h++)
 *     NC(G, aud_close, h, 0,0,0,0,0);
 *
 *   // Open our audio port:
 *   //   userId     = 0xFF (system user, same as video)
 *   //   portType   = 0    (SCE_AUDIO_OUT_PORT_TYPE_MAIN)
 *   //   index      = 0
 *   //   len        = 256  (samples per output call)
 *   //   freq       = 48000 Hz
 *   //   param      = 1    (SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO)
 *   s32 handle = NC(G, aud_open, 0xFF, 0, 0, 256, 48000, 1);
 *   return handle;
 * }
 *
 * // Output 256 stereo s16 samples:
 * void ps5_audio_output(void *G, void *aud_out, s32 handle, const s16 *buf) {
 *   NC(G, aud_out, handle, buf, 0,0,0,0);
 * }
 *
 * // sceAudioOutOutput blocks until the hardware consumes the buffer,
 * // providing natural frame pacing (no explicit sleep needed for 60 Hz).
 *
 * Audio pipeline note:
 *   libretro cores call retro_audio_sample_batch_t with stereo s16 samples.
 *   The batch size is typically 735 samples (48000/fps) per frame at 60 Hz.
 *   We must buffer these into 256-sample chunks for sceAudioOutOutput.
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 – INPUT BACKEND (DualSense)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * DualSense input via libScePad.sprx
 * Key difference from PS4 (DualShock 4): button bit layout changed.
 */

/*
 * DualSense raw button bitmask (from scePadRead() first 4 bytes):
 *
 *   Bit       DualSense button          libretro mapping
 *   ────────────────────────────────────────────────────────
 *   0x00000001  SELECT / SHARE          RETRO_DEVICE_ID_JOYPAD_SELECT
 *   0x00000002  L3                      RETRO_DEVICE_ID_JOYPAD_L3
 *   0x00000004  R3                      RETRO_DEVICE_ID_JOYPAD_R3
 *   0x00000008  OPTIONS                 RETRO_DEVICE_ID_JOYPAD_START
 *   0x00000010  D-PAD UP                RETRO_DEVICE_ID_JOYPAD_UP
 *   0x00000020  D-PAD RIGHT             RETRO_DEVICE_ID_JOYPAD_RIGHT
 *   0x00000040  D-PAD DOWN              RETRO_DEVICE_ID_JOYPAD_DOWN
 *   0x00000080  D-PAD LEFT              RETRO_DEVICE_ID_JOYPAD_LEFT
 *   0x00000100  L2 (digital threshold)  RETRO_DEVICE_ID_JOYPAD_L2
 *   0x00000200  R2 (digital threshold)  RETRO_DEVICE_ID_JOYPAD_R2
 *   0x00000400  L1                      RETRO_DEVICE_ID_JOYPAD_L
 *   0x00000800  R1                      RETRO_DEVICE_ID_JOYPAD_R
 *   0x00001000  TRIANGLE                RETRO_DEVICE_ID_JOYPAD_Y  (libretro Y=top)
 *   0x00002000  CIRCLE                  RETRO_DEVICE_ID_JOYPAD_A  (libretro A=right)
 *   0x00004000  CROSS                   RETRO_DEVICE_ID_JOYPAD_B  (libretro B=bottom)
 *   0x00008000  SQUARE                  RETRO_DEVICE_ID_JOYPAD_X  (libretro X=left)
 *
 * Note: libretro uses the SNES button naming convention.
 *   A=right, B=bottom, X=top, Y=left (viewed from front of controller).
 *   For NES emulation: libretro A → NES A, libretro B → NES B.
 *
 * s32 ps5_input_open(void *G, void *D, s32 userId) {
 *   s32 pad_mod = NC(G, load_mod, "libScePad.sprx", 0,0,0,0,0);
 *   void *pad_init = SYM(G, D, pad_mod, "scePadInit");
 *   void *pad_geth = SYM(G, D, pad_mod, "scePadGetHandle");
 *   if (pad_init) NC(G, pad_init, 0,0,0,0,0,0);
 *   return (s32)NC(G, pad_geth, userId, 0, 0, 0, 0, 0);
 * }
 *
 * // Read raw buttons from DualSense:
 * u32 ps5_input_read(void *G, void *pad_read, s32 pad_h) {
 *   u8 buf[128] = {0};
 *   s32 n = NC(G, pad_read, pad_h, buf, 1, 0, 0, 0);
 *   if (n <= 0 || (u32)n >= 0x80000000) return 0;
 *   u32 raw = *(u32*)buf;
 *   if (raw & 0x80000000) return 0;  // invalid report
 *   return raw & 0x001FFFFF;
 * }
 *
 * // Convert DualSense raw buttons to libretro joypad bitmask:
 * u16 ps5_ds_to_retro(u32 ds) {
 *   u16 r = 0;
 *   if (ds & 0x00004000) r |= (1 << RETRO_DEVICE_ID_JOYPAD_B);   // CROSS
 *   if (ds & 0x00002000) r |= (1 << RETRO_DEVICE_ID_JOYPAD_A);   // CIRCLE
 *   if (ds & 0x00008000) r |= (1 << RETRO_DEVICE_ID_JOYPAD_X);   // SQUARE
 *   if (ds & 0x00001000) r |= (1 << RETRO_DEVICE_ID_JOYPAD_Y);   // TRIANGLE
 *   if (ds & 0x00000001) r |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
 *   if (ds & 0x00000008) r |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
 *   if (ds & 0x00000010) r |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
 *   if (ds & 0x00000040) r |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
 *   if (ds & 0x00000080) r |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
 *   if (ds & 0x00000020) r |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
 *   if (ds & 0x00000400) r |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
 *   if (ds & 0x00000800) r |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
 *   if (ds & 0x00000100) r |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);
 *   if (ds & 0x00000200) r |= (1 << RETRO_DEVICE_ID_JOYPAD_R2);
 *   if (ds & 0x00000002) r |= (1 << RETRO_DEVICE_ID_JOYPAD_L3);
 *   if (ds & 0x00000004) r |= (1 << RETRO_DEVICE_ID_JOYPAD_R3);
 *   return r;
 * }
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 – PORTING CHECKLIST: RetroArch orbis → PS5
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Files to create/modify in RetroArch source tree:
 *
 *  frontend/drivers/platform_ps5.c
 *    - Copy platform_orbis.c as base
 *    - Replace exploit entry with our _start(eboot_base, dlsym, ext_args*)
 *    - Remove BSD process/fork code (not available in userland exploit)
 *    - Keep: frontend_ctx_driver_t platform_ps5 struct
 *
 *  input/drivers/ps5_input.c
 *    - Copy orbis_input.c
 *    - Replace button mapping table with DualSense layout above
 *    - scePadRead and scePadInit calls are identical
 *
 *  gfx/drivers/ps5_gfx.c
 *    - Copy orbis_gfx.c
 *    - Change pixelFormat in RegisterBuffers attr: same 0x80000000
 *    - Update swap_rb macro for XRGB8888→BGRA8888
 *    - Frame pacing: use sceKernelWaitEqueue on vsync event queue
 *
 *  audio/drivers/ps5_audio.c
 *    - Nearly identical to orbis_audio.c
 *    - sceAudioOutOpen params identical
 *    - Buffer size: SAMPLES_PER_BUF = 256 stereo s16 = 1024 bytes
 *
 *  Configuration (retroarch_ps5.cfg, created at /savedata0/retroarch.cfg):
 *    video_driver = "ps5"
 *    audio_driver = "ps5"
 *    input_driver = "ps5"
 *    system_directory = "/av_contents/content_tmp/system"
 *    savefile_directory = "/savedata0/saves"
 *    savestate_directory = "/savedata0/states"
 *    libretro_directory = "/av_contents/content_tmp/cores"
 *    content_database = "/av_contents/content_tmp/database"
 *
 *  Compilation flags (same as our shellcode build):
 *    CFLAGS += -ffreestanding -fno-stack-protector -fno-builtin \
 *              -fpie -mno-red-zone -fomit-frame-pointer
 *    Replace libc calls with our inline replacements (memset, memcpy, etc.)
 *    or link against musl-libc as static lib
 *
 * ── Steps to build ─────────────────────────────────────────────────────────
 *  1. ./build_retroarch.sh          (clones + patches RetroArch)
 *  2. cd retroarch && make -f Makefile.ps5
 *  3. objcopy -O binary retroarch retroarch.bin
 *  4. python3 gen_lua.py retroarch.bin es.lua
 *  5. python3 es_launcher.py <PS5_IP> --cores-dir cores/
 */
