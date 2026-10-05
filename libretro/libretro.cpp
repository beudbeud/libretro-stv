/* libretro.cpp — mednafen Saturn/ST-V libretro core */

#include "libretro.h"
#include "libretro_core_options.h"

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
#include "ss/stvio.h"

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

/* Display Rotation core option (libretro SET_ROTATION units):
 * -1 = Auto (follow the game database), else 0/1/2/3 = 0°/90°/180°/270° CCW. */
static int g_rotation_opt = -1;

/* Audio output volume multiplier passed to Mednafen's espec.SoundVolume.
 * 1.0 = unchanged; <1 attenuates, >1 amplifies (Mednafen clamps internally). */
static float g_sound_volume = 1.0f;

/* Effective frontend rotation in libretro SET_ROTATION units (0..3, ×90° CCW).
 * Auto follows the game database (game_info->rotated): horizontal (yoko) games
 * report 0 (no rotation), vertical (TATE) games report 90° so they display
 * upright on a horizontal screen. An explicit value forces that absolute
 * rotation for every game (e.g. "0" = no rotation for a physical TATE screen). */
static unsigned effective_rotation(void)
{
    if(g_rotation_opt >= 0) return (unsigned)g_rotation_opt;      /* forced absolute */
    return game_info ? ((unsigned)game_info->rotated & 3u) : 0u;  /* Auto: game database */
}

/* 90°/270° rotations swap the displayed width/height (and invert aspect). */
static bool rotation_swaps_axes(void)
{
    return (effective_rotation() & 1u) != 0u;
}

/* Report the effective frontend rotation to the frontend. */
static void send_rotation(void)
{
    unsigned rot = effective_rotation();
    environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rot);
}

/* Build a retro_game_geometry for an unrotated content size (w x h). When the
 * effective rotation is 90°/270° the displayed image is portrait: swap the
 * reported dimensions and invert the aspect ratio (3:4) so the rotated output
 * keeps correct proportions rather than being stretched into a 4:3 box. */
static retro_game_geometry make_geometry(int w, int h)
{
    retro_game_geometry geo = {};
    if(rotation_swaps_axes()) {
        geo.base_width   = h;  geo.base_height = w;
        geo.max_width    = FB_H; geo.max_height = w;
        geo.aspect_ratio = 3.f / 4.f;
    } else {
        geo.base_width   = w;  geo.base_height = h;
        geo.max_width    = FB_W; geo.max_height = h;
        geo.aspect_ratio = 4.f / 3.f;
    }
    return geo;
}
static MDFN_Surface *surf       = nullptr;
static int32        *line_widths = nullptr;

static constexpr int AUDIO_MAX = 44100;  /* 1s @ 44100Hz, mednafen needs >= 500ms */
static int16_t audio_buf[AUDIO_MAX * 2];


static uint8_t *port_ptr[2] = {};

/* ── Hammer (touchscreen) input — Critter Crusher ──────────────────────────── */
static bool g_is_hammer = false;

/* ── Trackball input — Hashire Patrol Car family (IOGA PORT-G counter mode) ─── */
static bool g_is_trackball = false;
/* Scales libretro RETRO_DEVICE_MOUSE relative deltas into trackball counter
 * units. Tunable; the games read deltas modulo 16-bit so only the ratio matters. */
#define STV_TRACKBALL_SCALE 1
/* Analog-stick → trackball: the stick gives an absolute deflection that we treat
 * as a roll velocity (counter delta per frame). Both stick indices are read since
 * some pads/autoconfigs report the usable stick on the right index.
 * g_trackball_sensitivity is a percentage (100 = default); see retro_run(). */
static int g_trackball_sensitivity = 100;
#define STV_TRACKBALL_ANALOG_DEADZONE 4096   /* ~12.5% of 32767 */
#define STV_TRACKBALL_ANALOG_DIV      80000  /* full deflection ≈ 41 counts/frame @100% */
/* Zero an analog axis value inside the deadzone, pass the rest through. Applied
 * per-stick before summing so an idle, drifting stick can't bias the other. */
static inline int stv_analog_deadzone(int v) {
    return (v > STV_TRACKBALL_ANALOG_DEADZONE || v < -STV_TRACKBALL_ANALOG_DEADZONE) ? v : 0;
}
/* gun buffer layout: [nom_x lo, nom_x hi, nom_y lo, nom_y hi, buttons]
 * nom_x ∈ [0, mouse_scale_x ≈ 21472], nom_y ∈ [mouse_offs_y, mouse_offs_y+mouse_scale_y]
 * RETRO_DEVICE_POINTER [-32768,32767] → this space via (ptr+32768)*scale/65536+offs */

static std::string sys_dir, save_dir;

/* ── BIOS Skip ─────────────────────────────────────────────────────────────── */
static bool        g_stv_skip_bios        = false;
static bool        g_bios_state_saved     = false;
static bool        g_bios_intback_resaved = false;
static bool        g_bios_service_entered = false;
static std::string g_bios_state_path;
static int         g_bios_total_frames    = 0;
/* Strategy: save once at BIOS_SKIP_FALLBACK_FRAMES (900 frames / 15 s),
 * then overwrite once if SMPC INTBACK fires (games that call INTBACK
 * during attract or first input poll get a fresher game-start state).
 * No save is ever written if the operator entered test/service mode. */
static const int   BIOS_SKIP_FALLBACK_FRAMES = 1080; /* 18 s at 60 fps */

/* ── Frameskip ─────────────────────────────────────────────────────────────── */
enum { FS_NONE = 0, FS_AUTO, FS_MANUAL };
static int  g_frameskip_type     = FS_NONE;
static int  g_frameskip_interval = 1;
static int  g_frameskip_counter  = 0;
static bool g_is_fastforwarding  = false;

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

    /* ── Player inputs ── */
    if(g_is_hammer) {
        /* Critter Crusher: touchscreen via RETRO_DEVICE_POINTER.
         * Convert pointer [-32768,32767] to mednafen gun buffer space:
         *   nom_x = (ptr_x + 32768) * mouse_scale_x / 65536 + mouse_offs_x
         *   nom_y = (ptr_y + 32768) * mouse_scale_y / 65536 + mouse_offs_y
         * STVIO then maps nom_x → x∈[0,62], nom_y → y∈[0,46] for the grid. */
        if(port_ptr[0]) {
            /* game_info is always non-null here (retro_run() guards before calling update_input) */
            const int16_t ptr_x = (int16_t)input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
            const int16_t ptr_y = (int16_t)input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
            const bool pressed  = (bool)input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);

            const int16_t nom_x = (int16_t)((float)((int32_t)ptr_x + 32768) * game_info->mouse_scale_x / 65536.0f + game_info->mouse_offs_x);
            const int16_t nom_y = (int16_t)((float)((int32_t)ptr_y + 32768) * game_info->mouse_scale_y / 65536.0f + game_info->mouse_offs_y);

            MDFN_en16lsb(port_ptr[0] + 0, (uint16_t)nom_x);
            MDFN_en16lsb(port_ptr[0] + 2, (uint16_t)nom_y);
            port_ptr[0][4] = pressed ? 0x01 : 0x00;
        }
    } else {
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
    }

    /* ── Trackball (Hashire Patrol Car family) ──
     * Feed relative mouse motion into STVIO's PORT-G counters. Buttons already
     * went through the gamepad path above (port 0). */
    if(g_is_trackball) {
        int dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X) * STV_TRACKBALL_SCALE;
        int dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y) * STV_TRACKBALL_SCALE;

        /* Analog stick as a velocity source: deflection past the deadzone adds a
         * per-frame delta scaled by the sensitivity option, letting these arcade
         * trackball games be played with a normal gamepad. Read both stick
         * indices (some pads/autoconfigs report the usable stick on the right
         * index) and deadzone each before summing — only one is ever deflected. */
        int lx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X);
        int ly = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y);
        int rx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
        int ry = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
        int ax = stv_analog_deadzone(lx) + stv_analog_deadzone(rx);
        int ay = stv_analog_deadzone(ly) + stv_analog_deadzone(ry);
        dx += ax * g_trackball_sensitivity / STV_TRACKBALL_ANALOG_DIV;
        dy += ay * g_trackball_sensitivity / STV_TRACKBALL_ANALOG_DIV;

        /* D-pad as a fixed-velocity movement source. Works on its own and also
         * catches the case where RetroArch's Analog-to-Digital converts the
         * stick into D-pad presses (then the analog read above stays 0). The
         * step equals a full stick deflection, scaled by the same sensitivity. */
        const int step = 32767 * g_trackball_sensitivity / STV_TRACKBALL_ANALOG_DIV;
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  dx -= step;
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) dx += step;
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    dy -= step;
        if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  dy += step;

        MDFN_IEN_SS::STVIO_SetTrackball(dx, dy);
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

/* Option definitions live in libretro_core_options.h as a single v2 source of
 * truth; libretro_set_core_options() (called from retro_set_environment) handles
 * down-conversion to the v1 and legacy formats. */

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
    BOOL_OPT("mednafen_stv_autortc",      "ss.smpc.autortc");
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

    /* Touchscreen crosshair. STVIO_SetCrosshairsColor is a no-op for non-HAMMER games.
     * color > 0xFFFFFF disables drawing (chair_draw = color <= 0xFFFFFF in gun.cpp). */
    {
        bool crosshair_on = true;
        var.key = "mednafen_stv_crosshair";
        if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            crosshair_on = strcmp(var.value, "disabled") != 0;

        uint32_t color = 0xFFFFFFFFu; /* hidden */
        if(crosshair_on) {
            static const struct { const char *name; uint32_t rgb; } s_colors[] = {
                { "white",  0xFFFFFFu }, { "red",    0xFF0000u },
                { "green",  0x00FF00u }, { "blue",   0x0000FFu },
                { "yellow", 0xFFFF00u }, { "cyan",   0x00FFFFu },
            };
            color = 0xFFFFFFu; /* white default */
            var.key = "mednafen_stv_crosshair_color";
            if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
                for(const auto &e : s_colors) {
                    if(!strcmp(var.value, e.name)) { color = e.rgb; break; }
                }
            }
        }
        MDFN_IEN_SS::STVIO_SetCrosshairsColor(0, color);
    }

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

    /* Display Rotation: -1 = Auto (game database), else 0/1/2/3 SET_ROTATION
     * units. effective_rotation() interprets these; the matching SET_ROTATION /
     * SET_GEOMETRY updates are driven from retro_run() (and retro_load_game /
     * retro_get_system_av_info). */
    var.key = "mednafen_stv_rotation";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if      (!strcmp(var.value, "auto")) g_rotation_opt = -1;
        else if (!strcmp(var.value, "90"))   g_rotation_opt = 1;
        else if (!strcmp(var.value, "180"))  g_rotation_opt = 2;
        else if (!strcmp(var.value, "270"))  g_rotation_opt = 3;
        else                                 g_rotation_opt = 0; /* "0" */
    }

    /* Audio Volume: percentage → multiplier for espec.SoundVolume (applied by
     * Mednafen during MDFNI_Emulate). Changeable at runtime; no restart needed. */
    var.key = "mednafen_stv_volume";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int pct = atoi(var.value);
        if(pct < 0) pct = 0;
        g_sound_volume = (float)pct / 100.0f;
    }

    var.key = "mednafen_stv_trackball_sensitivity";
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int pct = atoi(var.value);
        if(pct < 1) pct = 1;
        g_trackball_sensitivity = pct;
    }
}

/* ── API ───────────────────────────────────────────────────────────────────── */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    struct retro_log_callback lc = {};
    if(cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc)) log_cb = lc.log;

    bool categories_supported = false;
    libretro_set_core_options(cb, &categories_supported);
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
    info->library_version  = LIBRETRO_CORE_VERSION;
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
    /* make_geometry applies the effective rotation: 90°/270° swap the reported
     * dimensions and invert the aspect ratio. max_height = base_height (not
     * FB_H) keeps SwitchRes's fractal Y calc correct. */
    info->geometry = make_geometry(nom_w, nom_h);
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
    g_bios_service_entered    = false;
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

    /* Detect Hammer (touchscreen) games — ss.cpp pushes "gun" as first
     * DesiredInput entry for STV_CONTROL_HAMMER titles (Critter Crusher). */
    g_is_hammer = !game_info->DesiredInput.empty()
               && game_info->DesiredInput[0].device_name
               && strcmp(game_info->DesiredInput[0].device_name, "gun") == 0;

    /* Trackball (Hashire Patrol Car family): both player ports stay "gamepad"
     * for buttons; trackball motion rides a side channel into STVIO. The control
     * scheme isn't visible via DesiredInput, so query STVIO directly. */
    g_is_trackball = MDFN_IEN_SS::STVIO_IsTrackball();
    lr_log(RETRO_LOG_INFO, "Input scheme: %s\n",
           g_is_hammer ? "hammer/touchscreen" : (g_is_trackball ? "gamepad+trackball" : "gamepad"));

    /* Initialize ALL ports declared by the SS module (0..N-1).
     * ST-V has 13 ports (port12 = "builtin"). Without initializing all
     * of them, DPtr[12] stays nullptr → STVIO_TransformInput() crashes
     * with *DPtr[12] dereference on the first Emulate() call.          */
    {
        int nports = (int)game_info->PortInfo.size();
        lr_log(RETRO_LOG_INFO, "SS module has %d input ports\n", nports);
        for(int p = 0; p < nports; p++) {
            MDFNI_SetInput(p, 0);
        }

        if(g_is_hammer) {
            /* Gun type = index 7 in InputDeviceInfoSSVPort (smpc.cpp).
             * STVIO_SetInput only accepts type="gun" for port 0 in HAMMER scheme;
             * gamepad would be silently nulled, disabling all touchscreen input. */
            port_ptr[0] = MDFNI_SetInput(0, 7);
            if(port_ptr[0]) memset(port_ptr[0], 0, 5);
            port_ptr[1] = nullptr;
        } else {
            for(int p = 0; p < 2; p++) {
                port_ptr[p] = MDFNI_SetInput(p, 1);
                if(port_ptr[p]) memset(port_ptr[p], 0, 2);
            }
        }

        /* Port 12 = builtin (type 0) — holds STV Test/Service/Pause */
        int builtin_port = nports - 1;
        builtin_ptr = MDFNI_SetInput(builtin_port, 0);
        if(builtin_ptr) memset(builtin_ptr, 0, 1);
    }


    lr_log(RETRO_LOG_INFO,"Loaded: %s @ %.3fHz\n", game->path,
           (double)game_info->fps/(65536.0*256.0));

    /* Input descriptors — shown in RetroArch's control remapping UI */
    if(g_is_hammer) {
        static const struct retro_input_descriptor desc_hammer[] = {
            {0,RETRO_DEVICE_POINTER,0,RETRO_DEVICE_ID_POINTER_X,      "Touch X"},
            {0,RETRO_DEVICE_POINTER,0,RETRO_DEVICE_ID_POINTER_Y,      "Touch Y"},
            {0,RETRO_DEVICE_POINTER,0,RETRO_DEVICE_ID_POINTER_PRESSED,"Touch"},
            {0,RETRO_DEVICE_JOYPAD, 0,RETRO_DEVICE_ID_JOYPAD_SELECT,  "Insert Coin"},
            {0,RETRO_DEVICE_JOYPAD, 0,RETRO_DEVICE_ID_JOYPAD_L3,      "Test Button"},
            {0,RETRO_DEVICE_JOYPAD, 0,RETRO_DEVICE_ID_JOYPAD_R3,      "Service Button"},
            {0,0,0,0,nullptr},
        };
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc_hammer);
    } else if(g_is_trackball) {
        static const struct retro_input_descriptor desc_trackball[] = {
            {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,"Trackball X"},
            {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,"Trackball Y"},
            {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X,"Trackball X"},
            {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y,"Trackball Y"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT, "Trackball Left"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Trackball Right"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,   "Trackball Up"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN, "Trackball Down"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,     "A"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,     "B"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,     "C"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START, "Start"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Insert Coin"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L3,    "Test Button"},
            {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,    "Service Button"},
            {0,0,0,0,nullptr},
        };
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc_trackball);
    } else {
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

    /* Signal display rotation from the Display Rotation option (see
     * effective_rotation): Auto follows the game database, an explicit value
     * forces an absolute rotation. Re-sent from retro_run() if it changes. */
    send_rotation();

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
    g_is_hammer = false;
    g_is_trackball = false;
    s_serialize_size = 0;
    g_frameskip_counter = 0;
    g_bios_state_saved        = false;
    g_bios_intback_resaved    = false;
    g_bios_service_entered    = false;
    g_bios_state_path.clear();
}

RETRO_API void retro_reset(void) { if(game_info) MDFNI_Reset(); }

RETRO_API void retro_run(void)
{
    if(!game_info || !surf) return;
    bool opts = false;
    if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&opts) && opts) {
        apply_options();
        /* The Rotation core option may have changed: re-send rotation and
         * geometry (like Beetle PCE Fast) so it takes effect without a restart. */
        send_rotation();
        if(g_last_w > 0) {
            retro_game_geometry geo = make_geometry(g_last_w, g_last_h);
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
        }
    }

    update_input();

    /* Query the frontend's fast-forward state. In FS_AUTO this lets us drop
     * frames so fast-forward runs faster without flooding the video driver. */
    g_is_fastforwarding = false;
    environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &g_is_fastforwarding);

    /* Run-ahead / preemptive frames: on throwaway frames the frontend disables
     * audio and/or video via GET_AUDIO_VIDEO_ENABLE. Honor it every frame,
     * independent of the Frameskip option, so those frames skip rendering and
     * don't emit duplicate audio — this is what makes run-ahead efficient when
     * Frameskip is off (the default). */
    bool skip_frame    = false;
    bool audio_enabled = true;
    {
        int av = ~0;
        if(environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av)) {
            skip_frame    = !(av & RETRO_AV_ENABLE_VIDEO);
            audio_enabled =  (av & RETRO_AV_ENABLE_AUDIO);
        }
    }

    /* Frameskip: may additionally skip rendering this frame */
    if(g_frameskip_type == FS_AUTO) {
        /* The frontend mutes audio while fast-forwarding, so the AV-enable
         * hint above may not fire; render only every other frame instead. */
        if(g_is_fastforwarding && (g_frameskip_counter ^= 1))
            skip_frame = true;
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
    espec.SoundVolume     = g_sound_volume;
    espec.soundmultiplier = 1.0;
    espec.NeedRewind      = false;

    MDFNI_Emulate(&espec);

    if(g_stv_skip_bios && !g_bios_state_path.empty()
       && !(g_bios_state_saved && g_bios_intback_resaved)) {
        const uint32_t intback = MDFN_IEN_SS::SS_GetINTBACKCount();

        if(builtin_ptr && (builtin_ptr[0] & ((1 << 2) | (1 << 3))))
            g_bios_service_entered = true;

        g_bios_total_frames++;

        if(g_bios_service_entered) {
            /* Never capture a state taken from inside the service/test menu. */
        } else if(!g_bios_state_saved && g_bios_total_frames >= BIOS_SKIP_FALLBACK_FRAMES) {
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
    const bool frame_interlaced = espec.InterlaceOn;
    if(espec.InterlaceOn) {
        /* On skipped frames the renderer drew nothing: LineWidths are stale
         * (~0) and the surface untouched, so running the deinterlacer would
         * read garbage widths. The frame isn't displayed anyway. */
        if(g_deint && !skip_frame) {
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
        int dy = dr.y;

        /* The user's slstart/slend (default 8..231) is a 224-line NTSC safe-area
         * crop. When the game switches VDP2 to 240-line mode (e.g. Puyo Puyo
         * Sun) content fills rows 0..239 without borders, so the same window
         * loses 8 pixels top and bottom. For any non-224-NTSC mode (240/256, or
         * PAL), follow the game's actual content area instead. */
        /* Only follow the content area for *non-interlaced* mode switches.
         * GetContentArea reports a single-field height (224/240/256); for an
         * interlaced frame the surface is double-height (e.g. Decathlete's
         * 240-line + interlace = 448) and DisplayRect.h already carries it, so
         * applying the single-field crop here would show only the top half. */
        int content_y, content_h;
        MDFN_IEN_SS::VDP2::GetContentArea(&content_y, &content_h);
        if(!frame_interlaced && (content_y != 8 || content_h != 224)) {
            dy = content_y;
            dh = content_h;
        }

        /* Saturn: per-scanline widths */
        if(dh > 0 && line_widths[dy] != (int32)~0) {
            int mx = 0;
            for(int y=dy; y<dy+dh; y++) if(line_widths[y]>mx) mx=line_widths[y];
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
            retro_game_geometry geo = make_geometry(dw, display_h);
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
        }

        const uint32_t *px = reinterpret_cast<const uint32_t*>(surf->pixels)
            + (uint64_t)dy * surf->pitchinpix + dr.x;
        /* For interlaced frames, we pass the full dh so RA can deinterlace.
         * The pitch stays the same (full framebuffer row stride).            */
        video_cb(px, dw, dh, surf->pitchinpix * sizeof(uint32_t));
    }

    /* Use espec.SoundBuf not audio_buf: mednafen may redirect to its internal buffer.
     * Suppress output on throwaway frames (run-ahead) so they don't emit duplicate audio. */
    if(audio_enabled && espec.SoundBufSize > 0 && audio_batch_cb && espec.SoundBuf)
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
