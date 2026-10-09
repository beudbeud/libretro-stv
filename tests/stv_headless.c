/* stv_headless: runs the libretro core with no frontend.
**
**   stv_headless [options] <core.so> <rom.zip>
**     -f N            frames to run (default 600)
**     -s DIR          system directory (BIOS; default: directory of the ROM)
**     -S DIR          save directory (default: /tmp)
**     -o key=value    core option (repeatable), e.g. -o mednafen_stv_autortc=disabled
**     -w FRAMES       warm-up frames excluded from timing (default 0)
**     -q              quiet: only the final summary line
**     -c CPU          pin the main thread to this CPU (Linux)
**     -i FILE         write the last frame as a binary PPM image
**     -l FILE         load a raw core state (retro_serialize output; a
**                     RetroArch .state must be unpacked first) after loading
**     -r N            savestate round trip after frame N: serialize, run 300
**                     frames, unserialize, run the same 300 frames again; the
**                     two video/audio hashes must match (exit 3 otherwise).
**                     Needs a deterministic core (mednafen_stv_sound_thread=disabled).
**     -p FILE         sample the program counter (SIGPROF, 1 kHz, all threads)
**                     and write "<tid> <pc> <frame>" lines plus the core's load
**                     address to FILE; symbolize with nm on the unstripped core
**     -t FILE         write each frame's wall time (ms), one per line
**
** Prints one line per 300 frames (ms/frame) and a final summary:
**   frames=N video=<fnv1a of all frames> audio=<fnv1a of all samples> samples=N ms/frame=X cpu_ms/frame=Y
** (cpu_ms is the main thread's CPU time: wall minus cpu is time spent waiting)
** The hashes let two runs be compared (e.g. DSP JIT on/off, before/after a
** change) without any display; the timing is a headless benchmark.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>
#include <sched.h>
#include <dirent.h>
#include "libretro/libretro.h"

static struct { char key[64]; char val[64]; } opts[32];
static int nopts;
static char sysdir[1024], savedir[1024];
static int quiet;
static uint64_t vhash = 1469598103934665603ULL, ahash = 1469598103934665603ULL;
static uint64_t nsamples, nframes_video;
static unsigned cur_w, cur_h;
static const char* img_path;
static int img_frames_left = -1;

/* --- sampling profiler --- */
#define PROF_MAX (1 << 21)
static uintptr_t* prof_pc;
static int* prof_tid;
static int* prof_frame;
static volatile int cur_frame;
static volatile unsigned prof_n;
static void prof_handler(int sig, siginfo_t* si, void* uc)
{
 (void)sig; (void)si;
 unsigned i = prof_n;
 if(i < PROF_MAX)
 {
#if defined(__aarch64__)
  prof_pc[i] = ((ucontext_t*)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
  prof_pc[i] = ((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
#else
  prof_pc[i] = 0;
#endif
  prof_tid[i] = (int)syscall(SYS_gettid);
  prof_frame[i] = cur_frame;
  prof_n = i + 1;
 }
}
static void prof_start(void)
{
 prof_pc = malloc(PROF_MAX * sizeof(*prof_pc));
 prof_tid = malloc(PROF_MAX * sizeof(*prof_tid));
 prof_frame = malloc(PROF_MAX * sizeof(*prof_frame));
 struct sigaction sa; memset(&sa, 0, sizeof(sa));
 sa.sa_sigaction = prof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
 sigaction(SIGPROF, &sa, NULL);
 struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
 setitimer(ITIMER_PROF, &it, NULL);
}
static void prof_dump(const char* path, void* core_handle)
{
 struct itimerval it = { { 0, 0 }, { 0, 0 } };
 setitimer(ITIMER_PROF, &it, NULL);
 Dl_info di; memset(&di, 0, sizeof(di));
 void* sym = dlsym(core_handle, "retro_run");
 if(sym) dladdr(sym, &di);
 FILE* f = fopen(path, "w");
 if(!f) { perror(path); return; }
 fprintf(f, "base %lx\n", (unsigned long)di.dli_fbase);
 for(unsigned i = 0; i < prof_n; i++) fprintf(f, "%d %lx %d\n", prof_tid[i], (unsigned long)prof_pc[i], prof_frame[i]);
 fclose(f);
 fprintf(stderr, "profile: %u samples -> %s\n", prof_n, path);
}

/* FNV-1a over 64-bit words (byte-wise would cost ~1.3 ms per 480i frame and
** skew the timing); the tail bytes are hashed one by one. */
static void fnv(uint64_t* h, const void* p, size_t n)
{
 const uint8_t* b = (const uint8_t*)p;
 uint64_t x = *h;
 size_t i = 0;
 for(; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); x ^= w; x *= 1099511628211ULL; }
 for(; i < n; i++) { x ^= b[i]; x *= 1099511628211ULL; }
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
 if(img_path && img_frames_left == 0)
 {
  FILE* f = fopen(img_path, "wb");
  if(f)
  {
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   for(unsigned y = 0; y < h; y++)
    for(unsigned x = 0; x < w; x++)
    {
     const uint32_t p = ((const uint32_t*)((const uint8_t*)data + y * pitch))[x];
     const uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
     fwrite(rgb, 1, 3, f);
    }
   fclose(f);
  }
 }
 for(unsigned y = 0; y < h; y++)
  fnv(&vhash, (const uint8_t*)data + y * pitch, (size_t)w * 4);
 nframes_video++;
}
static void audio_sample_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; fnv(&ahash, s, 4); nsamples++; }
static size_t audio_batch_cb(const int16_t* data, size_t frames) { fnv(&ahash, data, frames * 4); nsamples += frames; return frames; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) { (void)port; (void)device; (void)index; (void)id; return 0; }

/* Per-thread CPU time of this process (Linux): "name=seconds" pairs. */
static void print_thread_cpu(FILE* out, int frames)
{
#ifdef __linux__
 char path[256];
 DIR* d = opendir("/proc/self/task");
 if(!d) return;
 fprintf(out, "thread_cpu_ms/frame:");
 struct dirent* de;
 while((de = readdir(d)))
 {
  long tid = atol(de->d_name); if(!tid) continue;
  snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
  FILE* f = fopen(path, "r"); if(!f) continue;
  char buf[1024]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
  char* rp = strrchr(buf, ')'); char* lp = strchr(buf, '('); if(!rp || !lp) continue;
  *rp = 0; const char* name = lp + 1;
  long ut = 0, st = 0; /* fields 14, 15 after ") " */
  { const char* q = rp + 2; int fi = 3; char* end;
    while(*q && fi < 14) { if(*q == ' ') fi++; q++; }
    ut = strtol(q, &end, 10); st = strtol(end, NULL, 10); }
  fprintf(out, " %s=%.2f", name, (ut + st) * 1000.0 / sysconf(_SC_CLK_TCK) / frames);
 }
 closedir(d);
 fprintf(out, "\n");
#else
 (void)out; (void)frames;
#endif
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double cpu_ms(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char** argv)
{
 int frames = 600, warm = 0;
 const char* prof_path = NULL;
 int pin_cpu = -1, rt_frame = -1;
 const char* times_path = NULL;
 const char* load_path = NULL;
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
  else if(!strcmp(argv[i], "-p") && i + 1 < argc) prof_path = argv[++i];
  else if(!strcmp(argv[i], "-c") && i + 1 < argc) pin_cpu = atoi(argv[++i]);
  else if(!strcmp(argv[i], "-r") && i + 1 < argc) rt_frame = atoi(argv[++i]);
  else if(!strcmp(argv[i], "-l") && i + 1 < argc) load_path = argv[++i];
  else if(!strcmp(argv[i], "-t") && i + 1 < argc) times_path = argv[++i];
  else if(!strcmp(argv[i], "-i") && i + 1 < argc) img_path = argv[++i];
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
 if(argc - i != 2) { fprintf(stderr, "usage: %s [-f N] [-w N] [-s sysdir] [-S savedir] [-o k=v]... [-q] [-p prof] [-c cpu] core.so rom\n", argv[0]); return 2; }
 const char* core = argv[i], *rom = argv[i + 1];
 if(!sysdir[0])
 {
  snprintf(sysdir, sizeof(sysdir), "%s", rom);
  char* sl = strrchr(sysdir, '/');
  if(sl) *sl = 0; else strcpy(sysdir, ".");
 }

 if(pin_cpu >= 0)
 {
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(pin_cpu, &cs);
  if(sched_setaffinity(0, sizeof(cs), &cs)) perror("sched_setaffinity");
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
 typedef size_t (*retro_serialize_size_t)(void);
 typedef bool (*retro_serialize_t)(void*, size_t);
 typedef bool (*retro_unserialize_t)(const void*, size_t);
 SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch)
 SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_deinit) SYM(retro_load_game)
 SYM(retro_unload_game) SYM(retro_run) SYM(retro_get_system_av_info)
 SYM(retro_serialize_size) SYM(retro_serialize) SYM(retro_unserialize)

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
 if(load_path)
 {
  FILE* f = fopen(load_path, "rb");
  if(!f) { perror(load_path); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  void* buf = malloc(n);
  if(fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "%s: short read\n", load_path); return 1; }
  fclose(f);
  if(!retro_unserialize(buf, n)) { fprintf(stderr, "%s: retro_unserialize failed\n", load_path); return 1; }
  free(buf);
 }
 if(!quiet) fprintf(stderr, "loaded: %ux%u @ %.3f Hz\n", cur_w, cur_h, av.timing.fps);

 double t_total = 0, t_win = 0, tmax = 0, c_total = 0;
 FILE* times_f = times_path ? fopen(times_path, "w") : NULL;
 for(int f = 0; f < frames; f++)
 {
  if(prof_path && f == warm) prof_start();
  img_frames_left = frames - 1 - f;
  cur_frame = f;
  const double t0 = now_ms(), c0 = cpu_ms();
  retro_run();
  const double dt = now_ms() - t0, dc = cpu_ms() - c0;
  if(f >= warm) { t_total += dt; t_win += dt; c_total += dc; if(dt > tmax) tmax = dt; }
  if(times_f) fprintf(times_f, "%.3f\n", dt);
  if(!quiet && (f + 1) % 300 == 0) { fprintf(stderr, "frame %6d  %.2f ms/frame (window)\n", f + 1, t_win / 300.0); t_win = 0; }
 }
 int rt_fail = 0;
 if(rt_frame >= 0)
 {
  /* Savestate round trip (see -r). */
  const size_t sz = retro_serialize_size();
  void* st = malloc(sz ? sz : 1);
  void* st2 = malloc(sz ? sz : 1);
  for(int f = frames; f < rt_frame; f++) retro_run();
  if(!sz || !retro_serialize(st, sz)) { fprintf(stderr, "roundtrip: serialize failed (size %zu)\n", sz); rt_fail = 1; }
  else
  {
   uint64_t h[2];
   for(int pass = 0; pass < 2; pass++)
   {
    if(pass == 1 && !retro_unserialize(st, sz)) { fprintf(stderr, "roundtrip: unserialize failed\n"); rt_fail = 1; break; }
    vhash = ahash = 1469598103934665603ULL;
    for(int f = 0; f < 300; f++) retro_run();
    h[pass] = vhash ^ (ahash * 31);
   }
   /* the restored state must serialize back to the same bytes */
   if(!rt_fail)
   {
    retro_unserialize(st, sz);
    if(!retro_serialize(st2, sz) || memcmp(st, st2, sz)) { fprintf(stderr, "roundtrip: re-serialized state differs\n"); rt_fail = 1; }
   }
   if(!rt_fail && h[0] != h[1]) { fprintf(stderr, "roundtrip: replay differs (%016llx vs %016llx)\n", (unsigned long long)h[0], (unsigned long long)h[1]); rt_fail = 1; }
   if(!rt_fail) fprintf(stderr, "roundtrip: OK (state %zu bytes, replay hash %016llx)\n", sz, (unsigned long long)h[0]);
  }
  free(st); free(st2);
 }
 const int timed = frames > warm ? frames - warm : 1;
 if(prof_path) prof_dump(prof_path, h);
 if(times_f) fclose(times_f);
 print_thread_cpu(stderr, frames);
 printf("frames=%d video=%016llx audio=%016llx samples=%llu ms/frame=%.3f cpu_ms/frame=%.3f max=%.2f geometry=%ux%u\n",
        frames, (unsigned long long)vhash, (unsigned long long)ahash, (unsigned long long)nsamples, t_total / timed, c_total / timed, tmax, cur_w, cur_h);

 retro_unload_game();
 retro_deinit();
 dlclose(h);
 return rt_fail ? 3 : 0;
}
