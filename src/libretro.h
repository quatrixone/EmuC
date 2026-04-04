/* libretro.h - PS5 EmuC libretro API (subset)
 * Based on the libretro API spec: https://github.com/libretro/libretro-common
 */
#ifndef LIBRETRO_H
#define LIBRETRO_H

#include "core.h"

/* ── Input devices ─────────────────────────────────────────────────────── */
#define RETRO_DEVICE_NONE       0
#define RETRO_DEVICE_JOYPAD     1
#define RETRO_DEVICE_MOUSE      2
#define RETRO_DEVICE_KEYBOARD   3
#define RETRO_DEVICE_ANALOG     5

/* JOYPAD button IDs */
#define RETRO_DEVICE_ID_JOYPAD_B        0
#define RETRO_DEVICE_ID_JOYPAD_Y        1
#define RETRO_DEVICE_ID_JOYPAD_SELECT   2
#define RETRO_DEVICE_ID_JOYPAD_START    3
#define RETRO_DEVICE_ID_JOYPAD_UP       4
#define RETRO_DEVICE_ID_JOYPAD_DOWN     5
#define RETRO_DEVICE_ID_JOYPAD_LEFT     6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT    7
#define RETRO_DEVICE_ID_JOYPAD_A        8
#define RETRO_DEVICE_ID_JOYPAD_X        9
#define RETRO_DEVICE_ID_JOYPAD_L        10
#define RETRO_DEVICE_ID_JOYPAD_R        11
#define RETRO_DEVICE_ID_JOYPAD_L2       12
#define RETRO_DEVICE_ID_JOYPAD_R2       13
#define RETRO_DEVICE_ID_JOYPAD_L3       14
#define RETRO_DEVICE_ID_JOYPAD_R3       15

/* ── Pixel formats ─────────────────────────────────────────────────────── */
#define RETRO_PIXEL_FORMAT_0RGB1555  0   /* default */
#define RETRO_PIXEL_FORMAT_XRGB8888  1
#define RETRO_PIXEL_FORMAT_RGB565    2

/* ── Environment commands ──────────────────────────────────────────────── */
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT      10
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY  9
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY    31
#define RETRO_ENVIRONMENT_SET_GEOMETRY          37
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE     27

/* ── Core structs ──────────────────────────────────────────────────────── */
struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;   /* comma-separated, e.g. "nes|rom" */
    int         need_fullpath;
    int         block_extract;
};

struct retro_game_geometry {
    u32 base_width;
    u32 base_height;
    u32 max_width;
    u32 max_height;
    float aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

struct retro_game_info {
    const char *path;
    const void *data;
    u64         size;
    const char *meta;
};

struct retro_variable {
    const char *key;
    const char *value;
};

struct retro_log_callback {
    void (*log)(int level, const char *fmt, ...);
};

/* ── Callback function types ───────────────────────────────────────────── */
typedef void  (*retro_video_refresh_t)(const void *data, u32 width, u32 height, u64 pitch);
typedef void  (*retro_audio_sample_t)(s16 left, s16 right);
typedef u64   (*retro_audio_sample_batch_t)(const s16 *data, u64 frames);
typedef void  (*retro_input_poll_t)(void);
typedef s16   (*retro_input_state_t)(u32 port, u32 device, u32 index, u32 id);
typedef int   (*retro_environment_t)(u32 cmd, void *data);

/* ── Core function table ───────────────────────────────────────────────── */
/* All dynamically loaded .bin cores start with this header at byte 0.
 * Offsets are relative to the start of the binary (since linker base = 0).
 * The frontend adds the mapped base address to each offset to get a
 * callable function pointer. */
#define CORE_MAGIC 0x35505243u  /* 'CRP5' */
#define CORE_VERSION 1u

struct core_header {
    u32 magic;
    u32 version;
    u32 retro_init_off;
    u32 retro_deinit_off;
    u32 retro_set_environment_off;
    u32 retro_set_video_refresh_off;
    u32 retro_set_audio_sample_off;
    u32 retro_set_audio_sample_batch_off;
    u32 retro_set_input_poll_off;
    u32 retro_set_input_state_off;
    u32 retro_get_system_info_off;
    u32 retro_get_system_av_info_off;
    u32 retro_load_game_off;
    u32 retro_unload_game_off;
    u32 retro_run_off;
    u32 retro_reset_off;
};

/* Live core instance after loading */
struct core_instance {
    u8  *base;       /* mapped executable memory */
    u64  size;

    void (*retro_init)(void);
    void (*retro_deinit)(void);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_get_system_info)(struct retro_system_info *);
    void (*retro_get_system_av_info)(struct retro_system_av_info *);
    int  (*retro_load_game)(const struct retro_game_info *);
    void (*retro_unload_game)(void);
    void (*retro_run)(void);
    void (*retro_reset)(void);
};

#endif /* LIBRETRO_H */
