/* libretro.cpp — mednafen Saturn/ST-V libretro core */

#include "libretro.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cmath>
#include <string>
#include <stdarg.h>

/* mednafen headers — found via -I src/ with src/mednafen/ proxy dir */
#include "mednafen.h"          /* types, settings etc. */
#include "mednafen-driver.h"   /* MDFNI_Init, MDFNI_LoadGame, MDFNI_Emulate… */
#include "mednafen.h"          /* MDFN_QSimpleCommand, MDFN_MSC_INSERT_COIN  */
#include "state.h"             /* MDFNSS_SaveSM / LoadSM */
#include "state-driver.h"      /* MDFND_SetStateStatus */
#include "netplay-driver.h"    /* MDFND_NetplayText */
#include "NativeVFS.h"
#include "git.h"
#include "MemoryStream.h"
#include "video/surface.h"
#include "video/Deinterlacer.h"

#include "FileStream.h"

#include "ss/ss.h"
#include "ss/vdp1.h"
#include "ss/vdp1_common.h"
#include "ss/vdp2.h"

using namespace Mednafen;

/* ── Stubs for excluded/unused modules ────────────────────────────────────── */
namespace Mednafen {

/* tests.cpp / testsexp.cpp excluded — provide stubs */
void MDFN_RunCheapTests(void) {}
void MDFN_RunExceptionTests(const unsigned, const unsigned) {}

} /* namespace Mednafen */

/* ── Libretro callbacks ────────────────────────────────────────────────────── */
retro_environment_t       environ_cb      = nullptr;
static retro_log_printf_t         log_cb          = nullptr;
static retro_video_refresh_t      video_cb        = nullptr;
static retro_audio_sample_batch_t audio_batch_cb  = nullptr;
static retro_input_poll_t         input_poll_cb   = nullptr;
static retro_input_state_t        input_state_cb  = nullptr;

/* ── State ─────────────────────────────────────────────────────────────────── */
static MDFNGI  *game_info        = nullptr;
static int      g_last_w         = 0, g_last_h = 0;
static bool     initialized      = false;
static size_t   s_serialize_size = 0;

static constexpr int FB_W = 704, FB_H = 512;
static MDFN_Surface *surf       = nullptr;
static int32        *line_widths = nullptr;

static constexpr int AUDIO_MAX = 44100;  /* 1s @ 44100Hz, mednafen needs >= 500ms */
static int16_t audio_buf[AUDIO_MAX * 2];


static uint8_t pad_data[2][32] = {};
static uint8_t *port_ptr[2]    = {};

static std::string sys_dir, save_dir;

/* ── BIOS Skip ─────────────────────────────────────────────────────────────── */
static bool        g_stv_skip_bios        = false;
static bool        g_bios_state_saved     = false;
static bool        g_bios_intback_resaved = false;
static std::string g_bios_state_path;
static int         g_bios_total_frames    = 0;
/* Strategy: save once at BIOS_SKIP_FALLBACK_FRAMES (900 frames / 15 s),
 * then overwrite once if SMPC INTBACK fires (games that call INTBACK
 * during attract or first input poll get a fresher game-start state). */
static const int   BIOS_SKIP_FALLBACK_FRAMES = 1080; /* 18 s at 60 fps */

/* ── Frameskip ─────────────────────────────────────────────────────────────── */
enum { FS_NONE = 0, FS_AUTO, FS_MANUAL };
static int g_frameskip_type     = FS_NONE;
static int g_frameskip_interval = 1;
static int g_frameskip_counter  = 0;

/* ── Deinterlacer ──────────────────────────────────────────────────────────── */
/* Sentinel for "renderer-side bob" (VDP2::SetDeinterlaceOff(true)) — bypasses
 * the SW Deinterlacer. Any other value is a real Deinterlacer enum constant
 * (DEINT_BOB, DEINT_WEAVE, etc.). */
static constexpr unsigned DEINT_OFF_SENTINEL = ~0u;
static Deinterlacer* g_deint      = nullptr;
static unsigned      g_deint_type = DEINT_OFF_SENTINEL;
static bool          g_prev_interlaced = false;

/* ── Logger ────────────────────────────────────────────────────────────────── */
static void lr_log(retro_log_level lvl, const char *fmt, ...)
{
    if(!log_cb) return;
    char buf[1024]; va_list ap;
    va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    log_cb(lvl, "%s", buf);
}

/* ── Required mednafen driver callbacks ────────────────────────────────────── */
namespace Mednafen {

void MDFND_OutputNotice(MDFN_NoticeType t, const char *s) noexcept
{
    lr_log(t==MDFN_NOTICE_ERROR ? RETRO_LOG_ERROR : RETRO_LOG_INFO,
           "[mdfn] %s\n", s);
}

void MDFND_OutputInfo(const char *s) noexcept
{
    lr_log(RETRO_LOG_INFO, "[mdfn] %s", s);
}

void MDFND_MidSync(EmulateSpecStruct *espec, const unsigned flags)
{
    if((flags & MIDSYNC_FLAG_UPDATE_INPUT) && input_poll_cb)
        input_poll_cb();
}

bool MDFND_CheckNeedExit(void) { return false; }
void MDFND_MediaSetNotification(uint32, uint32, uint32, uint32) {}
void MDFND_SetStateStatus(StateStatusStruct *) noexcept {}
void MDFND_SetMovieStatus(StateStatusStruct *) noexcept {}
void MDFND_NetplayText(const char *, bool) {}
void MDFND_NetplaySetHints(bool, bool, uint32) {}
void MDFND_DispMessage(char *) {}

} /* namespace Mednafen */

/* ── Input mapping ─────────────────────────────────────────────────────────────
 *
 * Saturn gamepad DPtr format: uint16 LE bitfield (2 bytes).
 * IODevice_Gamepad::UpdateInput: buttons = (~(data[0]|(data[1]<<8))) &~0x3000
 * Bit=1 in data → button pressed (inverted inside mednafen).
 *
 * Bit layout (from beetle-saturn/input.cpp input_map_pad + gamepad.cpp):
 *  Position → libretro button → Saturn button
 *   0  : L1 (RETRO L)      → Z
 *   1  : X  (RETRO X)      → Y
 *   2  : Y  (RETRO Y)      → X
 *   3  : R2 (RETRO R2)     → R (Right Shoulder)
 *   4  : Up                → Up
 *   5  : Down              → Down
 *   6  : Left              → Left
 *   7  : Right             → Right
 *   8  : A  (RETRO A)      → B
 *   9  : R1 (RETRO R)      → C
 *  10  : B  (RETRO B)      → A
 *  11  : Start             → Start
 *  15  : L2 (RETRO L2)     → L (Left Shoulder)
 *
 * ST-V Builtin port 12 (IDII_Builtin in smpc.cpp):
 *  bit 0 = SS reset   (cleared by STVIO_TransformInput)
 *  bit 1 = smpc_reset
 *  bit 2 = stv_test     → mapped to Select player 1
 *  bit 3 = stv_service  → mapped to L3 player 1
 *  bit 4 = stv_pause    → mapped to R3 player 1
 *
 * Coin insertion: mapped to Select (RETRO_DEVICE_ID_JOYPAD_SELECT)
 *   via STVIO coin mechanism on port data
 * ──────────────────────────────────────────────────────────────────────────── */

/* Input map following beetle-saturn's exact bit positions */
enum {
    SAT_BIT_Z     = 0,   /* L1  */
    SAT_BIT_Y     = 1,   /* X   */
    SAT_BIT_X     = 2,   /* Y   */
    SAT_BIT_R     = 3,   /* R2  */
    SAT_BIT_UP    = 4,
    SAT_BIT_DOWN  = 5,
    SAT_BIT_LEFT  = 6,
    SAT_BIT_RIGHT = 7,
    SAT_BIT_B     = 8,   /* A   */
    SAT_BIT_C     = 9,   /* R1  */
    SAT_BIT_A     = 10,  /* B   */
    SAT_BIT_START = 11,
    SAT_BIT_L     = 15,  /* L2  */
};

static const struct { unsigned retro; unsigned bit; } s_pad_map[] = {
    { RETRO_DEVICE_ID_JOYPAD_L,     SAT_BIT_Z     },  /* L1  → Z  */
    { RETRO_DEVICE_ID_JOYPAD_X,     SAT_BIT_Y     },  /* X   → Y  */
    { RETRO_DEVICE_ID_JOYPAD_Y,     SAT_BIT_X     },  /* Y   → X  */
    { RETRO_DEVICE_ID_JOYPAD_R2,    SAT_BIT_R     },  /* R2  → R  */
    { RETRO_DEVICE_ID_JOYPAD_UP,    SAT_BIT_UP    },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,  SAT_BIT_DOWN  },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,  SAT_BIT_LEFT  },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT, SAT_BIT_RIGHT },
    { RETRO_DEVICE_ID_JOYPAD_A,     SAT_BIT_B     },  /* A   → B  */
    { RETRO_DEVICE_ID_JOYPAD_R,     SAT_BIT_C     },  /* R1  → C  */
    { RETRO_DEVICE_ID_JOYPAD_B,     SAT_BIT_A     },  /* B   → A  */
    { RETRO_DEVICE_ID_JOYPAD_START, SAT_BIT_START },
    { RETRO_DEVICE_ID_JOYPAD_L2,    SAT_BIT_L     },  /* L2  → L  */
};

static uint8_t *builtin_ptr = nullptr;

/* Track coin button edge to pulse coin signal */
static bool coin_was_pressed[2] = {};

static void update_input()
{
    if(!input_poll_cb || !input_state_cb) return;
    input_poll_cb();

    /* ── Player gamepad inputs ── */
    for(int p = 0; p < 2; p++) {
        if(!port_ptr[p]) continue;

        uint16_t bits = 0;
        for(auto &m : s_pad_map)
            if(input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, m.retro))
                bits |= (1u << m.bit);

        /* No simultaneous opposite directions */
        if((bits & (1<<SAT_BIT_UP))   && (bits & (1<<SAT_BIT_DOWN)))
            bits &= ~((1<<SAT_BIT_UP)|(1<<SAT_BIT_DOWN));
        if((bits & (1<<SAT_BIT_LEFT)) && (bits & (1<<SAT_BIT_RIGHT)))
            bits &= ~((1<<SAT_BIT_LEFT)|(1<<SAT_BIT_RIGHT));

        port_ptr[p][0] = (uint8_t)(bits & 0xFF);
        port_ptr[p][1] = (uint8_t)(bits >> 8);
    }

    /* ── ST-V Builtin port 12 (Test / Service / Pause / Coin) ──
     *
     * Mapping:
     *   Select (P1) → Coin insert P1  (via STVIO CoinPending mechanism)
     *   Select (P2) → Coin insert P2
     *   L3    (P1)  → Test button      (operator menu)
     *   R3    (P1)  → Service button   (free credit)
     *   R3    (P2)  → Pause button
     */
    if(builtin_ptr) {
        uint8_t b = 0;

        /* Test button (bit 2) */
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3))
            b |= (1 << 2);

        /* Service button (bit 3) */
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3))
            b |= (1 << 3);

        /* Pause button (bit 4) */
        if(input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3))
            b |= (1 << 4);

        builtin_ptr[0] = b;
    }

    /* ── Coin insertion via Select button ──
     * MDFN_QSimpleCommand(MDFN_MSC_INSERT_COIN) is the clean API:
     * it calls STVIO_InsertCoin() → increments CoinPending counter.
     * Edge detect: only trigger once per button press, not on hold. */
    for(int p = 0; p < 2; p++) {
        bool coin_now = input_state_cb(p, RETRO_DEVICE_JOYPAD, 0,
                                       RETRO_DEVICE_ID_JOYPAD_SELECT);
        if(coin_now && !coin_was_pressed[p])
            MDFN_QSimpleCommand(MDFN_MSC_INSERT_COIN);
        coin_was_pressed[p] = coin_now;
    }
}

/* ── Core options ─────────────────────────────────────────────────────────── */
static retro_core_option_v2_definition s_opts[] = {
    { "mednafen_stv_region", "Region", NULL, NULL, NULL, "system",
      { {"jp","Japan"},{"na","North America"},{"eu","Europe"},{"auto","Auto"},{NULL,NULL} }, "auto" },
    { "mednafen_stv_h_overscan", "Show Horizontal Overscan", NULL, NULL, NULL, "video",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
    { "mednafen_stv_h_blend", "Horizontal Blend Filter", NULL, NULL, NULL, "video",
      { {"disabled","Disabled"},{"enabled","Enabled"},{NULL,NULL} }, "disabled" },
    { "mednafen_stv_mesh_transparency", "Improved Mesh Transparency", NULL,
      "Replace VDP1's hardware-accurate (x ^ y) & 1 stipple, used by mesh-bit primitives, with a 50% blend against the final composited image. The stipple looks like a visible checkerboard on a flat panel (it relied on CRT phosphor blur); the blend improves the look of smoke, shadows, water and fade effects. 16-bit framebuffer only.", NULL, "video",
      { {"disabled","Disabled"},{"enabled","Enabled"},{NULL,NULL} }, "disabled" },
    { "mednafen_stv_correct_aspect", "Correct Aspect Ratio", NULL, NULL, NULL, "video",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
    { "mednafen_stv_slstart", "First Scanline (NTSC)", NULL, NULL, NULL, "video",
      { {"0","0"},{"2","2"},{"4","4"},{"8","8"},{NULL,NULL} }, "8" },
    { "mednafen_stv_slend", "Last Scanline (NTSC)", NULL, NULL, NULL, "video",
      { {"239","239"},{"234","234"},{"231","231"},{"224","224"},{NULL,NULL} }, "231" },
    { "mednafen_stv_cart", "Expansion Cart", NULL, NULL, NULL, "system",
      { {"auto","Auto"},{"none","None"},{"backup","Backup RAM"},{"4mram","4M RAM"},{"8mram","8M RAM"},{NULL,NULL} }, "auto" },
    { "mednafen_stv_bios_sanity", "BIOS Sanity Checks", NULL, NULL, NULL, "system",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
    { "mednafen_stv_autortc", "Auto-set RTC", NULL, NULL, NULL, "system",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
    { "mednafen_stv_autortc_lang", "BIOS Language", NULL, NULL, NULL, "system",
      { {"english","English"},{"japanese","Japanese"},{"french","French"},
        {"german","German"},{"spanish","Spanish"},{"italian","Italian"},{NULL,NULL} }, "english" },
    { "mednafen_stv_cpu_cache", "CPU Cache Emulation", NULL,
      "SH-2 cache emulation level. 'Fast' skips instruction cache (recommended). 'Full' emulates both caches accurately but is slower. Restart required.", NULL, "performance",
      { {"data_cb","Fast (recommended)"},{"data","Data cache only"},{"full","Full (accurate, slow)"},{NULL,NULL} }, "data_cb" },
    { "mednafen_stv_frameskip", "Frameskip", NULL,
      "'Auto' skips frames when the frontend signals video is not needed. '1'–'5' skips N frames between each rendered frame (manual). 'Disabled' renders every frame.", NULL, "performance",
      { {"disabled","Disabled"},{"auto","Auto"},{"1","1"},{"2","2"},{"3","3"},{"4","4"},{"5","5"},{NULL,NULL} }, "disabled" },
    { "mednafen_stv_skip_bios", "Skip BIOS", NULL,
      "Boot directly into the game, bypassing the ST-V BIOS startup sequence. On first boot the BIOS runs normally and a state is cached; subsequent boots load that state instantly. The cache is stored per-game in the save directory. Restart required after toggling.", NULL, "system",
      { {"disabled","Disabled"},{"enabled","Enabled"},{NULL,NULL} }, "disabled" },
    { "mednafen_stv_deinterlacer", "Deinterlacer", NULL,
      "Handling of 480i scenes (e.g. Astra Superstars, VF Kids attract). 'Blend' averages adjacent fields for smooth LCD output (recommended). 'Off' duplicates each rendered field's lines onto the opposite-field row at render time (no CPU cost but visible per-line transitions on detailed sprites). 'Weave' is CRT-like (combing on motion). 'Bob Offset' is sharp but flickers.", NULL, "video",
      { {"blend","Blend (smooth, recommended for LCD)"},{"off","Off (renderer-side bob, full resolution)"},{"weave","Weave (CRT-like, combing on motion)"},{"bob","Bob"},{"bob_offset","Bob with offset (sharp, flickers)"},{"blend_rg","Blend (gamma-correct, more CPU)"},{NULL,NULL} }, "blend" },
    { NULL,NULL,NULL,NULL,NULL,NULL,{{0}},NULL }
};
static retro_core_options_v2 s_opts_v2 = { nullptr, s_opts };

static void apply_options()
{
    if(!initialized) return;
    struct retro_variable var = {};

#define BOOL_OPT(opt_key, setting) \
    var.key = (opt_key); \
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var) && var.value) \
        MDFNI_SetSetting(setting, strcmp(var.value,"enabled")==0?"1":"0");

#define STR_OPT(opt_key, setting) \
    var.key = (opt_key); \
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var) && var.value) \
        MDFNI_SetSetting(setting, var.value);

    /* Region: "auto" enables autodetect; explicit region disables it. */
    var.key = "mednafen_stv_region";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if(strcmp(var.value, "auto") == 0) {
            MDFNI_SetSetting("ss.region_autodetect", "1");
        } else {
            MDFNI_SetSetting("ss.region_autodetect", "0");
            MDFNI_SetSetting("ss.region_default", var.value);
        }
    }
    BOOL_OPT("mednafen_stv_h_overscan",   "ss.h_overscan");
    BOOL_OPT("mednafen_stv_h_blend",      "ss.h_blend");
    BOOL_OPT("mednafen_stv_correct_aspect","ss.correct_aspect");
    STR_OPT ("mednafen_stv_slstart",      "ss.slstart");
    STR_OPT ("mednafen_stv_slend",        "ss.slend");
    STR_OPT ("mednafen_stv_cart",         "ss.cart");
    BOOL_OPT("mednafen_stv_bios_sanity",  "ss.bios_sanity");
    BOOL_OPT("mednafen_stv_autortc",      "ss.smpc.autortc");
    STR_OPT ("mednafen_stv_autortc_lang", "ss.smpc.autortc.lang");
    STR_OPT ("mednafen_stv_cpu_cache",    "ss.cpu_cache_stv");
#undef BOOL_OPT
#undef STR_OPT

    /* Improved mesh transparency: forwarded to VDP1::SetMeshImproved,
     * which sets a module-level bool read by VDP1 rasterisation and
     * VDP2 compositing. Called on the emulator main thread, same
     * thread that runs the renderer, so no synchronisation needed. */
    var.key = "mednafen_stv_skip_bios";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        g_stv_skip_bios = (strcmp(var.value, "enabled") == 0);

    var.key = "mednafen_stv_mesh_transparency";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        MDFN_IEN_SS::VDP1::SetMeshImproved(strcmp(var.value, "enabled") == 0);

    var.key = "mednafen_stv_frameskip";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if(strcmp(var.value, "auto") == 0) {
            g_frameskip_type = FS_AUTO;
        } else if(strcmp(var.value, "disabled") == 0) {
            g_frameskip_type = FS_NONE;
        } else {
            g_frameskip_type     = FS_MANUAL;
            g_frameskip_interval = atoi(var.value);
            if(g_frameskip_interval < 1) g_frameskip_interval = 1;
            if(g_frameskip_interval > 5) g_frameskip_interval = 5;
        }
        g_frameskip_counter = 0;
    }

    /* Deinterlacer: "off" = renderer-side bob (VDP2 mirrors each scanline
     * onto the opposite-field row at draw time); other values pick a SW
     * Deinterlacer post-processor invoked after MDFNI_Emulate.
     *
     * Default ("off" / unknown value) is the renderer-side bob. */
    var.key = "mednafen_stv_deinterlacer";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        unsigned new_type = DEINT_OFF_SENTINEL;
        bool use_renderer_bob = true;

        if     (!strcmp(var.value, "weave"))      { new_type = Deinterlacer::DEINT_WEAVE;      use_renderer_bob = false; }
        else if(!strcmp(var.value, "bob"))        { new_type = Deinterlacer::DEINT_BOB;        use_renderer_bob = false; }
        else if(!strcmp(var.value, "bob_offset")) { new_type = Deinterlacer::DEINT_BOB_OFFSET; use_renderer_bob = false; }
        else if(!strcmp(var.value, "blend"))      { new_type = Deinterlacer::DEINT_BLEND;      use_renderer_bob = false; }
        else if(!strcmp(var.value, "blend_rg"))   { new_type = Deinterlacer::DEINT_BLEND_RG;   use_renderer_bob = false; }

        MDFN_IEN_SS::VDP2::SetDeinterlaceOff(use_renderer_bob);

        if(new_type != g_deint_type) {
            delete g_deint;
            g_deint = use_renderer_bob ? nullptr : Deinterlacer::Create(new_type);
            g_deint_type = new_type;
            g_prev_interlaced = false; /* force ClearState on next interlaced frame */
        }
    }
}

/* ── API ───────────────────────────────────────────────────────────────────── */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    struct retro_log_callback lc = {};
    if(cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc)) log_cb = lc.log;
    if(!cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &s_opts_v2)) {
        static const retro_variable legacy[] = {
            {"mednafen_stv_region","Region; jp|na|eu|auto"},
            {NULL,NULL}
        };
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)legacy);
    }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)          { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t)               {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb){ audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)                { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)              { input_state_cb = cb; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "Mednafen STV";
    info->library_version  = MEDNAFEN_VERSION;
    info->valid_extensions = "zip|ic8|bin|cue|toc|ccd|chd|m3u";
    info->need_fullpath    = true;
    info->block_extract    = true;   /* mednafen handles zip extraction itself */
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    /* Fixed initial geometry — never conditional on game_info.
     * Like Beetle PCE Fast: retro_get_system_av_info reports constants,
     * actual resolution updates go via SET_GEOMETRY in retro_run.
     * 320x240 @ 59.826Hz is the Saturn NTSC nominal.                   */
    /* Use actual nominal dimensions from loaded game when available.
     * nominal_height = slend - slstart + 1, computed by VDP2REND_SetGetVideoParams.
     * MAME reports 224 (8..231) for RSgun — we match that with our defaults.
     *
     * CRITICAL for SwitchRes: max_height must equal base_height, NOT FB_H.
     * RetroArch passes max_height to SwitchRes as the "source" height.
     * If max_height=512 (FB_H), SwitchRes computes Y_fractal = 240/512 = 0.47
     * instead of 240/448 = 0.535 that MAME gets with its 224-line geometry. */
    int nom_w = game_info ? game_info->nominal_width  : 320;
    int nom_h = game_info ? game_info->nominal_height : 224;
    info->geometry.base_width   = nom_w;
    info->geometry.base_height  = nom_h;
    info->geometry.max_width    = FB_W;   /* framebuffer width (required for Saturn variable H-res) */
    info->geometry.max_height   = nom_h;  /* = base_height: SwitchRes uses this for fractal Y calc */
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = game_info
        ? (double)game_info->fps / (65536.0 * 256.0) : 59.826;
    info->timing.sample_rate    = 44100.0;
}



RETRO_API void retro_init(void)
{
    if(initialized) return;
    const char *dir = nullptr;
    if(environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) sys_dir  = dir;
    if(environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,   &dir) && dir) save_dir = dir;
    if(sys_dir.empty())  sys_dir  = ".";
    if(save_dir.empty()) save_dir = ".";

    if(!MDFNI_Init()) { lr_log(RETRO_LOG_ERROR,"MDFNI_Init failed\n"); return; }

    // Use basedir = save_dir for mednafen internal files (state, sav, etc.)
    if(!MDFNI_InitFinalize(save_dir.c_str()))
    { lr_log(RETRO_LOG_ERROR,"MDFNI_InitFinalize failed\n"); MDFNI_Kill(); return; }

    // Point firmware path directly to RetroArch system directory.
    // BIOS files must be placed flat in system_dir (e.g. /recalbox/share/bios/).
    // Setting an absolute path makes mednafen use it directly without any
    // subdirectory search (no firmware/ subfolder needed).
    MDFNI_SetSetting("filesys.path_firmware", sys_dir.c_str());

    // BIOS filenames — mednafen builds: sys_dir + "/" + filename
    MDFNI_SetSetting("ss.bios_stv_jp",   "epr-20091.ic8");   // ST-V Japan
    MDFNI_SetSetting("ss.bios_stv_asia", "epr-19854.ic8");   // ST-V Asia/Taiwan
    MDFNI_SetSetting("ss.bios_stv_na",   "epr-17952a.ic8");  // ST-V North America
    MDFNI_SetSetting("ss.bios_stv_eu",   "epr-17954a.ic8");  // ST-V Europe
    MDFNI_SetSetting("ss.bios_jp",      "sega_101.bin");    // Saturn Japan (unused for ST-V)
    MDFNI_SetSetting("ss.bios_na_eu",   "mpr-17933.bin");   // Saturn NA/EU (unused for ST-V)

    /* XRGB8888: opp=4, R at bit16, G at bit8, B at bit0, A at bit24 */
    MDFN_PixelFormat pf(MDFN_COLORSPACE_RGB, 4, 16, 8, 0, 24);
    surf        = new MDFN_Surface(nullptr, FB_W, FB_H, FB_W, pf);
    line_widths = new int32[FB_H];

    initialized = true;
    lr_log(RETRO_LOG_INFO,"Mednafen STV initialized\n");
}

RETRO_API void retro_deinit(void)
{
    if(!initialized) return;
    if(game_info) { MDFNI_CloseGame(); game_info = nullptr; }
    MDFNI_Kill();
    delete surf;          surf = nullptr;
    delete[] line_widths; line_widths = nullptr;
    delete g_deint;       g_deint = nullptr;
    g_deint_type = DEINT_OFF_SENTINEL;
    g_prev_interlaced = false;
    initialized = false;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    if(!initialized || !game || !game->path) return false;
    apply_options();

    retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    { lr_log(RETRO_LOG_ERROR,"XRGB8888 not supported\n"); return false; }

    /* Use mednafen global NVFS — same as standalone mednafen.
     * A local NativeVFS would be destroyed after retro_load_game()
     * but mednafen may store this pointer for use during Emulate(). */
    game_info = MDFNI_LoadGame("ss", &NVFS, game->path);
    if(!game_info) { lr_log(RETRO_LOG_ERROR,"Load failed: %s\n",game->path); return false; }

    /* Re-apply options now that the renderer is up. The first call (above)
     * wrote MDFNI_SetSetting entries that MDFNI_LoadGame needed to read; it
     * also enqueued VDP2REND commands, but VDP2REND_Init (called during
     * LoadGame) re-initialises its command queue and resets DeinterlaceOff,
     * so any VDP2-thread options applied before LoadGame are discarded.
     * Re-applying here is idempotent for the MDFNI_SetSetting half and
     * correctly takes effect for the VDP2 half. */
    apply_options();

    /* BIOS skip: build per-game state path from ROM MD5, then try to load it. */
    g_bios_state_saved        = false;
    g_bios_intback_resaved    = false;
    g_bios_state_path.clear();
    g_bios_total_frames       = 0;
    if(g_stv_skip_bios) {
        char md5_hex[33] = {};
        for(int i = 0; i < 16; i++)
            snprintf(md5_hex + i * 2, 3, "%02x", (unsigned char)game_info->MD5[i]);
        md5_hex[32] = '\0';
        g_bios_state_path = save_dir + "/" + md5_hex + "_biosstate.mcs";

        try {
            Mednafen::FileStream st(g_bios_state_path, Mednafen::FileStream::MODE_READ);
            MDFNSS_LoadSM(&st, false);
            g_bios_state_saved = true; /* already cached, no need to re-save */
            lr_log(RETRO_LOG_INFO, "[skip_bios] Loaded cached state: %s\n", g_bios_state_path.c_str());
        } catch(...) {
            /* No cached state yet — will boot through BIOS and auto-save. */
            lr_log(RETRO_LOG_INFO, "[skip_bios] No cached state, booting through BIOS to create it.\n");
        }
    }

    /* Initialize ALL ports declared by the SS module (0..N-1).
     * ST-V has 13 ports (port12 = "builtin"). Without initializing all
     * of them, DPtr[12] stays nullptr → STVIO_TransformInput() crashes
     * with *DPtr[12] dereference on the first Emulate() call.          */
    {
        int nports = (int)game_info->PortInfo.size();
        lr_log(RETRO_LOG_INFO, "SS module has %d input ports\n", nports);
        for(int p = 0; p < nports; p++) {
            /* type 0 = first/default device for this port */
            MDFNI_SetInput(p, 0);
        }
        /* Gamepad for players 0 and 1 (type 1 = gamepad) */
        for(int p = 0; p < 2; p++) {
            port_ptr[p] = MDFNI_SetInput(p, 1);
            if(port_ptr[p]) memset(port_ptr[p], 0, 2);
        }
        /* Port 12 = builtin (type 0) — holds STV Test/Service/Pause */
        int builtin_port = nports - 1;
        builtin_ptr = MDFNI_SetInput(builtin_port, 0);
        if(builtin_ptr) memset(builtin_ptr, 0, 1);
    }


    lr_log(RETRO_LOG_INFO,"Loaded: %s @ %.3fHz\n", game->path,
           (double)game_info->fps/(65536.0*256.0));

    /* Input descriptors — shown in RetroArch's control remapping UI */
    {
        static const struct retro_input_descriptor desc[] = {
            /* Player 1 */
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,    "Up"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START, "Start"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,     "A"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,     "B"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,     "C"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,     "X"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,     "Y"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,     "Z"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,    "Left Shoulder (L)"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,    "Right Shoulder (R)"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Insert Coin"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L3,    "Test Button"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,    "Service Button"},
            /* Player 2 */
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,    "Up"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START, "Start"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,     "A"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,     "B"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,     "C"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,     "X"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,     "Y"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,     "Z"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,    "Left Shoulder (L)"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,    "Right Shoulder (R)"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Insert Coin"},
            {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,    "Pause Button"},
            {0,0,0,0,nullptr},
        };
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc);
    }

    /* Signal display rotation. tate_mode=enabled → game_info->rotated=MDFN_ROTATE90 → rotation 1.
     * tate_mode=disabled (default) → rotation 0, overriding any frontend auto-detect. */
    {
        unsigned rot = (game_info->rotated != 0) ? 1u : 0u;
        environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rot);
    }

    /* Memory map — exposes Saturn bus regions so RetroArch's NCI
     * READ_CORE_MEMORY / RetroAchievements can inspect them.
     * All descriptors use the default (blank) addrspace so a plain
     * hex bus address (e.g. "06000000") finds them directly.
     * Storage is uint16 host-byte-order; the BIGENDIAN flag tells the
     * frontend the underlying Saturn bus is big-endian, so byte reads
     * on LE hosts see byte-swapped 16-bit words. */
    {
        using namespace MDFN_IEN_SS;
        static retro_memory_descriptor desc[] = {
            /* SH-2 work RAM, low half (CS0/A-bus) */
            { RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x00200000, 0, 0, 0x100000, nullptr },
            /* Internal SMPC Backup RAM (real bus has open-bus on odd
             * bytes; we expose 32 KB contiguous for debug). */
            { RETRO_MEMDESC_SAVE_RAM   | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x00180000, 0, 0, 0x8000,   nullptr },
            /* VDP1 VRAM (512 KB) */
            { RETRO_MEMDESC_VIDEO_RAM  | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x05C00000, 0, 0, 0x80000,  nullptr },
            /* VDP1 framebuffer 0 — real bus addr of the display FB
             * region. Note: VDP1 swaps which FB is bus-visible; this
             * descriptor always shows FB[0]. */
            { RETRO_MEMDESC_VIDEO_RAM  | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x05C80000, 0, 0, 0x40000,  nullptr },
            /* VDP1 framebuffer 1 — placed in unused bus region just
             * after the real FB (0x05CC0000-0x05D7FFFF is open bus
             * on a real Saturn). */
            { RETRO_MEMDESC_VIDEO_RAM  | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x05CC0000, 0, 0, 0x40000,  nullptr },
            /* VDP2 VRAM (512 KB) */
            { RETRO_MEMDESC_VIDEO_RAM  | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x05E00000, 0, 0, 0x80000,  nullptr },
            /* VDP2 CRAM (palette, 4 KB) */
            { RETRO_MEMDESC_VIDEO_RAM  | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x05F00000, 0, 0, 0x1000,   nullptr },
            /* SH-2 work RAM, high half (CS3/B-bus) — main work RAM */
            { RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN,
              nullptr, 0, 0x06000000, 0, 0, 0x100000, nullptr },
        };
        desc[0].ptr = SS_GetWorkRAML();
        desc[1].ptr = SS_GetBackupRAM();
        desc[2].ptr = VDP1::VRAM;
        desc[3].ptr = VDP1::FB[0];
        desc[4].ptr = VDP1::FB[1];
        desc[5].ptr = VDP2::GetVRAM();
        desc[6].ptr = VDP2::GetCRAM();
        desc[7].ptr = SS_GetWorkRAMH();

        retro_memory_map mmap = { desc, (unsigned)(sizeof(desc) / sizeof(desc[0])) };
        environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
    }

    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t)
{ return false; }

RETRO_API void retro_unload_game(void)
{
    if(game_info) { MDFNI_CloseGame(); game_info = nullptr; }
    port_ptr[0] = port_ptr[1] = nullptr;
    s_serialize_size = 0;
    g_frameskip_counter = 0;
    g_bios_state_saved        = false;
    g_bios_intback_resaved    = false;
    g_bios_state_path.clear();
}

RETRO_API void retro_reset(void) { if(game_info) MDFNI_Reset(); }

RETRO_API void retro_run(void)
{
    if(!game_info || !surf) return;
    bool opts = false;
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&opts) && opts) {
        apply_options();
        /* Also update geometry on option change (like Beetle PCE Fast) */
        if(g_last_w > 0) {
            bool is_tate = game_info && (game_info->rotated != 0);
            retro_game_geometry geo={};
            if(is_tate) {
                geo.base_width=g_last_h; geo.base_height=g_last_w;
                geo.max_width=FB_H;      geo.max_height=g_last_w;
            } else {
                geo.base_width=g_last_w; geo.base_height=g_last_h;
                geo.max_width=FB_W;      geo.max_height=g_last_h;
            }
            geo.aspect_ratio = 4.f/3.f;
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
        }
    }

    update_input();

    /* Frameskip: decide whether to skip rendering this frame */
    bool skip_frame = false;
    if(g_frameskip_type == FS_AUTO) {
        int av = ~0;
        if(environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av))
            skip_frame = !(av & 1);
    } else if(g_frameskip_type == FS_MANUAL) {
        if(g_frameskip_counter == 0) {
            g_frameskip_counter = g_frameskip_interval;
        } else {
            --g_frameskip_counter;
            skip_frame = true;
        }
    }

    for(int i = 0; i < FB_H; i++) line_widths[i] = ~0;

    EmulateSpecStruct espec;
    espec.surface         = surf;
    espec.VideoFormatChanged = false;
    espec.DisplayRect     = { 0, 0, 0, 0 };
    espec.LineWidths      = line_widths;
    espec.CustomPalette   = nullptr;
    espec.CustomPaletteNumEntries = 0;
    espec.InterlaceOn     = false;
    espec.InterlaceField  = false;
    espec.skip            = skip_frame ? 1 : 0;
    espec.SoundFormatChanged = false;
    espec.SoundRate       = 44100.0;
    espec.SoundBuf        = audio_buf;
    espec.SoundBufMaxSize = AUDIO_MAX;
    espec.SoundBufSize    = 0;
    espec.SoundBufSize_InternalProcessed = 0;
    espec.SoundBufSize_DriverProcessed   = 0;
    espec.MasterCycles    = 0;
    espec.MasterCycles_InternalProcessed = 0;
    espec.MasterCycles_DriverProcessed   = 0;
    espec.SoundVolume     = 1.0;
    espec.soundmultiplier = 1.0;
    espec.NeedRewind      = false;

    MDFNI_Emulate(&espec);

    if(g_stv_skip_bios && !g_bios_state_path.empty()
       && !(g_bios_state_saved && g_bios_intback_resaved)) {
        const uint32_t intback = MDFN_IEN_SS::SS_GetINTBACKCount();

        g_bios_total_frames++;

        if(!g_bios_state_saved && g_bios_total_frames >= BIOS_SKIP_FALLBACK_FRAMES) {
            try {
                Mednafen::FileStream st(g_bios_state_path, Mednafen::FileStream::MODE_WRITE);
                MDFNSS_SaveSM(&st, false, nullptr, nullptr, nullptr);
                lr_log(RETRO_LOG_INFO, "[skip_bios] state saved (frame %d)\n", g_bios_total_frames);
            } catch(...) {
                lr_log(RETRO_LOG_WARN, "[skip_bios] failed to save state\n");
            }
            g_bios_state_saved = true;
        } else if(g_bios_state_saved && !g_bios_intback_resaved && intback > 0) {
            /* Re-save once on first INTBACK — yields a fresher game-start state
             * for games that call INTBACK during attract or first input poll. */
            g_bios_intback_resaved = true;
            try {
                Mednafen::FileStream st(g_bios_state_path, Mednafen::FileStream::MODE_WRITE);
                MDFNSS_SaveSM(&st, false, nullptr, nullptr, nullptr);
                lr_log(RETRO_LOG_INFO, "[skip_bios] state re-saved on INTBACK (frame %d)\n", g_bios_total_frames);
            } catch(...) {
                lr_log(RETRO_LOG_WARN, "[skip_bios] failed to re-save state\n");
            }
        }
    }

    /* Deinterlace handling. Both paths yield a progressive full-height surface
     * — clear InterlaceOn either way so the geometry/video_cb code below sees
     * a single coherent state.
     *   - SW mode (g_deint != null): Process fills the opposite-field rows.
     *   - "Off" mode (g_deint == null): VDP2::SetDeinterlaceOff was set to
     *     true; the renderer already mirrored each scanline at draw time. */
    if(espec.InterlaceOn) {
        if(g_deint) {
            if(!g_prev_interlaced) g_deint->ClearState();
            g_deint->Process(espec.surface, espec.DisplayRect, espec.LineWidths, espec.InterlaceField);
        }
        g_prev_interlaced = true;
        espec.InterlaceOn = false;
    } else {
        g_prev_interlaced = false;
    }

    /* Video */
    if(skip_frame) {
        video_cb(NULL, g_last_w > 0 ? g_last_w : 320,
                       g_last_h > 0 ? g_last_h : 224, 0);
    } else {
        const MDFN_Rect &dr = espec.DisplayRect;
        int dw = dr.w, dh = dr.h;
        /* Saturn: per-scanline widths */
        if(dh > 0 && line_widths[dr.y] != (int32)~0) {
            int mx = 0;
            for(int y=dr.y; y<dr.y+dh; y++) if(line_widths[y]>mx) mx=line_widths[y];
            if(mx > 0) dw = mx;
        }
        if(dw<=0) dw=320; if(dh<=0) dh=240;
        /* Clamp garbage transition frames (e.g. 4x224 during VDP2 mode switch). */
        if(dw < 64) dw = (g_last_w >= 64) ? g_last_w : 320;

        /* Both deinterlace paths (renderer-side bob and SW Process) produce a
         * progressive full-height surface — InterlaceOn was cleared above —
         * so dh is the displayable height directly. */
        int display_h = dh;

        /* Like Beetle PCE Fast: immediate SET_GEOMETRY on resolution change. */
        if(dw != g_last_w || display_h != g_last_h) {
            g_last_w = dw; g_last_h = display_h;
            bool is_tate = game_info && (game_info->rotated != 0);
            retro_game_geometry geo={};
            if(is_tate) {
                geo.base_width   = display_h;
                geo.base_height  = dw;
                geo.max_width    = FB_H;
                geo.max_height   = dw;
            } else {
                geo.base_width   = dw;
                geo.base_height  = display_h;
                geo.max_width    = FB_W;
                geo.max_height   = display_h;
            }
            geo.aspect_ratio = 4.f / 3.f;
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
        }

        const uint32_t *px = reinterpret_cast<const uint32_t*>(surf->pixels)
            + (uint64_t)dr.y * surf->pitchinpix + dr.x;
        /* For interlaced frames, we pass the full dh so RA can deinterlace.
         * The pitch stays the same (full framebuffer row stride).            */
        video_cb(px, dw, dh, surf->pitchinpix * sizeof(uint32_t));
    }

    /* Use espec.SoundBuf not audio_buf: mednafen may redirect to its internal buffer */
    if(espec.SoundBufSize > 0 && audio_batch_cb && espec.SoundBuf)
        audio_batch_cb(espec.SoundBuf, (size_t)espec.SoundBufSize);
}

RETRO_API size_t retro_serialize_size(void)
{
    if(!game_info) return 0;
    if(!s_serialize_size) {
        try {
            MemoryStream st(8 * 1024 * 1024, false);
            MDFNSS_SaveSM(&st, true);
            s_serialize_size = (size_t)st.size();
            lr_log(RETRO_LOG_INFO, "Serialize size: %zu bytes\n", s_serialize_size);
        } catch(std::exception &e) {
            lr_log(RETRO_LOG_ERROR, "retro_serialize_size failed: %s\n", e.what());
            return 0;
        }
    }
    return s_serialize_size;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    if(!game_info || !data) return false;
    try {
        MemoryStream st(s_serialize_size ? s_serialize_size : size, false);
        MDFNSS_SaveSM(&st, true);
        size_t written = (size_t)st.size();
        if(written > size) {
            lr_log(RETRO_LOG_ERROR, "Serialize overflow: %zu > %zu\n", written, size);
            return false;
        }
        memcpy(data, st.map(), written);
        if(written < size)
            memset((uint8_t*)data + written, 0, size - written);
        return true;
    } catch(std::exception &e) {
        lr_log(RETRO_LOG_ERROR, "retro_serialize failed: %s\n", e.what());
        return false;
    }
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    if(!game_info || !data) return false;
    try {
        MemoryStream st(size, -1);
        memcpy(st.map(), data, size);
        st.seek(0, SEEK_SET);
        MDFNSS_LoadSM(&st, true);
        return true;
    } catch(std::exception &e) {
        lr_log(RETRO_LOG_ERROR, "retro_unserialize failed: %s\n", e.what());
        return false;
    }
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
RETRO_API unsigned retro_get_region(void)
{
    if(!initialized) return RETRO_REGION_NTSC;
    try {
        std::string r = MDFN_GetSettingS("ss.region_default");
        return (r=="eu") ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
    } catch(...) { return RETRO_REGION_NTSC; }
}
RETRO_API void *retro_get_memory_data(unsigned id)
{
    if(!initialized) return nullptr;
    switch(id) {
    case RETRO_MEMORY_SYSTEM_RAM: return MDFN_IEN_SS::SS_GetWorkRAMH();
    case RETRO_MEMORY_VIDEO_RAM:  return MDFN_IEN_SS::VDP2::GetVRAM();
    default: return nullptr;
    }
}
RETRO_API size_t retro_get_memory_size(unsigned id)
{
    if(!initialized) return 0;
    switch(id) {
    case RETRO_MEMORY_SYSTEM_RAM: return 1024 * 1024;  /* WorkRAMH */
    case RETRO_MEMORY_VIDEO_RAM:  return 512 * 1024;   /* VDP2 VRAM */
    default: return 0;
    }
}
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if(!initialized || port > 1) return;
    port_ptr[port] = MDFNI_SetInput(port, (device==5) ? 2 : 1);
    if(port_ptr[port]) memset(port_ptr[port], 0, 16);
}
