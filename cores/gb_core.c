/* gb_core.c - Game Boy / GBC libretro core for PS5 */
#include "../src/core.h"
#include "../src/libretro.h"

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

__attribute__((section(".text._core_start")))
void _core_start(u64 base, struct core_header *out) {
    out->magic = CORE_MAGIC; out->version = CORE_VERSION;
#define OFF(fn) (u32)((u64)(fn) - base)
    out->retro_init_off                 = OFF(gb_init);
    out->retro_deinit_off               = OFF(gb_deinit);
    out->retro_set_environment_off      = OFF(gb_set_env);
    out->retro_set_video_refresh_off    = OFF(gb_set_video);
    out->retro_set_audio_sample_off     = OFF(gb_set_audio);
    out->retro_set_audio_sample_batch_off = OFF(gb_set_audio_batch);
    out->retro_set_input_poll_off       = OFF(gb_set_input_poll);
    out->retro_set_input_state_off      = OFF(gb_set_input_state);
    out->retro_get_system_info_off      = OFF(gb_get_sysinfo);
    out->retro_get_system_av_info_off   = OFF(gb_get_avinfo);
    out->retro_load_game_off            = OFF(gb_load_game);
    out->retro_unload_game_off          = OFF(gb_unload);
    out->retro_run_off                  = OFF(gb_run);
    out->retro_reset_off                = OFF(gb_reset);
#undef OFF
}
