/* stv_headless: runs the libretro core with no frontend.
**
**   stv_headless [options] <core.so> <rom.zip>
**     -f N            frames to run (default 600)
**     -s DIR          system directory (BIOS; default: directory of the ROM)
**     -S DIR          save directory (default: /tmp)
**     -o key=value    core option (repeatable), e.g. -o mednafen_stv_autortc=disabled
**     -w FRAMES       warm-up frames excluded from timing (default 0)
**     -q              quiet: only the final summary line
**
** Prints one line per 300 frames (ms/frame) and a final summary:
**   frames=N video=<fnv1a of all frames> audio=<fnv1a of all samples> samples=N ms/frame=X
** The hashes let two runs be compared (e.g. DSP JIT on/off, before/after a
** change) without any display; the timing is a headless benchmark.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <dlfcn.h>
#include "libretro/libretro.h"

static struct { char key[64]; char val[64]; } opts[32];
static int nopts;
static char sysdir[1024], savedir[1024];
static int quiet;
static uint64_t vhash = 1469598103934665603ULL, ahash = 1469598103934665603ULL;
static uint64_t nsamples, nframes_video;
static unsigned cur_w, cur_h;

static void fnv(uint64_t* h, const void* p, size_t n)
{
 const uint8_t* b = (const uint8_t*)p;
 uint64_t x = *h;
 for(size_t i = 0; i < n; i++) { x ^= b[i]; x *= 1099511628211ULL; }
 *h = x;
}

static void log_cb(enum retro_log_level level, const char* fmt, ...)
{
 if(quiet && level < RETRO_LOG_WARN) return;
 va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

static bool env_cb(unsigned cmd, void* data)
{
 switch(cmd)
 {
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
   return *(enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_XRGB8888;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = sysdir; return true;
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = savedir; return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = log_cb; return true;
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data = 2; return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE:
  {
   struct retro_variable* v = (struct retro_variable*)data;
   for(int i = 0; i < nopts; i++)
    if(!strcmp(opts[i].key, v->key)) { v->value = opts[i].val; return true; }
   v->value = NULL;
   return false;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data = false; return true;
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  {
   const struct retro_game_geometry* g = (const struct retro_game_geometry*)data;
   cur_w = g->base_width; cur_h = g->base_height;
   return true;
  }
  case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool*)data = false; return true;
  case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int*)data = 3; return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_ROTATION:
  case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
   return true;
  default:
   return false;
 }
}

static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch)
{
 if(!data) return;	/* duped frame */
 for(unsigned y = 0; y < h; y++)
  fnv(&vhash, (const uint8_t*)data + y * pitch, (size_t)w * 4);
 nframes_video++;
}
static void audio_sample_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; fnv(&ahash, s, 4); nsamples++; }
static size_t audio_batch_cb(const int16_t* data, size_t frames) { fnv(&ahash, data, frames * 4); nsamples += frames; return frames; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) { (void)port; (void)device; (void)index; (void)id; return 0; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char** argv)
{
 int frames = 600, warm = 0;
 strcpy(savedir, "/tmp");
 sysdir[0] = 0;
 int i = 1;
 for(; i < argc && argv[i][0] == '-'; i++)
 {
  if(!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
  else if(!strcmp(argv[i], "-w") && i + 1 < argc) warm = atoi(argv[++i]);
  else if(!strcmp(argv[i], "-s") && i + 1 < argc) snprintf(sysdir, sizeof(sysdir), "%s", argv[++i]);
  else if(!strcmp(argv[i], "-S") && i + 1 < argc) snprintf(savedir, sizeof(savedir), "%s", argv[++i]);
  else if(!strcmp(argv[i], "-q")) quiet = 1;
  else if(!strcmp(argv[i], "-o") && i + 1 < argc && nopts < 32)
  {
   const char* kv = argv[++i]; const char* eq = strchr(kv, '=');
   if(!eq) { fprintf(stderr, "bad option %s\n", kv); return 2; }
   snprintf(opts[nopts].key, sizeof(opts[nopts].key), "%.*s", (int)(eq - kv), kv);
   snprintf(opts[nopts].val, sizeof(opts[nopts].val), "%s", eq + 1);
   nopts++;
  }
  else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
 }
 if(argc - i != 2) { fprintf(stderr, "usage: %s [-f N] [-w N] [-s sysdir] [-S savedir] [-o k=v]... [-q] core.so rom\n", argv[0]); return 2; }
 const char* core = argv[i], *rom = argv[i + 1];
 if(!sysdir[0])
 {
  snprintf(sysdir, sizeof(sysdir), "%s", rom);
  char* sl = strrchr(sysdir, '/');
  if(sl) *sl = 0; else strcpy(sysdir, ".");
 }

 void* h = dlopen(core, RTLD_NOW | RTLD_LOCAL);
 if(!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
 #define SYM(name) name##_t name = (name##_t)dlsym(h, #name); if(!name) { fprintf(stderr, "missing %s\n", #name); return 1; }
 typedef void (*retro_set_environment_t)(retro_environment_t);
 typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
 typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
 typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
 typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
 typedef void (*retro_set_input_state_t)(retro_input_state_t);
 typedef void (*retro_init_t)(void);
 typedef void (*retro_deinit_t)(void);
 typedef bool (*retro_load_game_t)(const struct retro_game_info*);
 typedef void (*retro_unload_game_t)(void);
 typedef void (*retro_run_t)(void);
 typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info*);
 SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch)
 SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_deinit) SYM(retro_load_game)
 SYM(retro_unload_game) SYM(retro_run) SYM(retro_get_system_av_info)

 retro_set_environment(env_cb);
 retro_init();
 retro_set_video_refresh(video_cb);
 retro_set_audio_sample(audio_sample_cb);
 retro_set_audio_sample_batch(audio_batch_cb);
 retro_set_input_poll(input_poll_cb);
 retro_set_input_state(input_state_cb);

 struct retro_game_info gi; memset(&gi, 0, sizeof(gi)); gi.path = rom;
 if(!retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }
 struct retro_system_av_info av; retro_get_system_av_info(&av);
 cur_w = av.geometry.base_width; cur_h = av.geometry.base_height;
 if(!quiet) fprintf(stderr, "loaded: %ux%u @ %.3f Hz\n", cur_w, cur_h, av.timing.fps);

 double t_total = 0, t_win = 0, tmax = 0;
 for(int f = 0; f < frames; f++)
 {
  const double t0 = now_ms();
  retro_run();
  const double dt = now_ms() - t0;
  if(f >= warm) { t_total += dt; t_win += dt; if(dt > tmax) tmax = dt; }
  if(!quiet && (f + 1) % 300 == 0) { fprintf(stderr, "frame %6d  %.2f ms/frame (window)\n", f + 1, t_win / 300.0); t_win = 0; }
 }
 const int timed = frames > warm ? frames - warm : 1;
 printf("frames=%d video=%016llx audio=%016llx samples=%llu ms/frame=%.3f max=%.2f\n",
        frames, (unsigned long long)vhash, (unsigned long long)ahash, (unsigned long long)nsamples, t_total / timed, tmax);

 retro_unload_game();
 retro_deinit();
 dlclose(h);
 return 0;
}
