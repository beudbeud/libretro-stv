#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
 *
 * Single source of truth for the core's options. Define options here once,
 * in the v2 (categorised) form; libretro_set_core_options() below down-converts
 * to the v1 and legacy formats automatically depending on what the frontend
 * supports. Do NOT maintain parallel v1 / retro_variable copies by hand.
 */

/* Category visibility groups shown by frontends that support v2 categories. */
static struct retro_core_option_v2_category option_cats_us[] = {
   { "system",      "System",      NULL },
   { "input",       "Input",       NULL },
   { "video",       "Video",       NULL },
   { "audio",       "Audio",       NULL },
   { "performance", "Performance", NULL },
   { NULL, NULL, NULL }
};

static struct retro_core_option_v2_definition option_defs_us[] = {
   /* ── System ── */
   { "mednafen_stv_region", "Region", NULL, NULL, NULL, "system",
      { {"jp","Japan"},{"na","North America"},{"eu","Europe"},{"tw","Asia"},{"auto","Auto"},{NULL,NULL} }, "auto" },
   { "mednafen_stv_cart", "Expansion Cart", NULL, NULL, NULL, "system",
      { {"auto","Auto"},{"none","None"},{"backup","Backup RAM"},{"4mram","4M RAM"},{"8mram","8M RAM"},{NULL,NULL} }, "auto" },
   { "mednafen_stv_skip_bios", "Skip BIOS", NULL,
      "Boot directly into the game, bypassing the ST-V BIOS startup sequence. On first boot the BIOS runs normally and a state is cached; subsequent boots load that state instantly. The cache is stored per-game in the save directory. Restart required after toggling.", NULL, "system",
      { {"disabled","Disabled"},{"enabled","Enabled"},{NULL,NULL} }, "disabled" },
   { "mednafen_stv_autortc", "Auto-set RTC", NULL, NULL, NULL, "system",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },

   /* ── Input ── */
   { "mednafen_stv_crosshair", "Touchscreen Crosshair", NULL,
      "Show a crosshair cursor for touchscreen games (Critter Crusher).", NULL, "input",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
   { "mednafen_stv_crosshair_color", "Touchscreen Crosshair Color", NULL,
      "Color of the touchscreen crosshair.", NULL, "input",
      { {"white","White"},{"red","Red"},{"green","Green"},{"blue","Blue"},{"yellow","Yellow"},{"cyan","Cyan"},{NULL,NULL} }, "white" },
   { "mednafen_stv_trackball_sensitivity", "Trackball Analog Stick Sensitivity", NULL,
      "Roll speed of the left analog stick for trackball games (Hashire Patrol Car, Sky Challenger, Nerae! Super Goal, Technical Bowling). Higher rolls faster at full deflection. Does not affect mouse/real trackball input.", NULL, "input",
      { {"25","25%"},{"50","50%"},{"75","75%"},{"100","100%"},{"150","150%"},{"200","200%"},{"300","300%"},{"400","400%"},{NULL,NULL} }, "100" },

   /* ── Video ── */
   { "mednafen_stv_correct_aspect", "Correct Aspect Ratio", NULL, NULL, NULL, "video",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
   { "mednafen_stv_h_overscan", "Show Horizontal Overscan", NULL, NULL, NULL, "video",
      { {"enabled","Enabled"},{"disabled","Disabled"},{NULL,NULL} }, "enabled" },
   { "mednafen_stv_h_blend", "Horizontal Blend Filter", NULL, NULL, NULL, "video",
      { {"disabled","Disabled"},{"enabled","Enabled"},{NULL,NULL} }, "disabled" },
   { "mednafen_stv_mesh_transparency", "Improved Mesh Transparency", NULL,
      "Replace VDP1's hardware-accurate (x ^ y) & 1 stipple, used by mesh-bit primitives, with a 50% blend against the final composited image. The stipple looks like a visible checkerboard on a flat panel (it relied on CRT phosphor blur); the blend improves the look of smoke, shadows, water and fade effects. 16-bit framebuffer only.", NULL, "video",
      { {"disabled","Disabled"},{"enabled","Enabled"},{NULL,NULL} }, "disabled" },
   { "mednafen_stv_deinterlacer", "Deinterlacer", NULL,
      "Handling of 480i scenes (e.g. Astra Superstars, VF Kids attract). 'Blend' averages adjacent fields for smooth LCD output (recommended). 'Off' duplicates each rendered field's lines onto the opposite-field row at render time (no CPU cost but visible per-line transitions on detailed sprites). 'Weave' is CRT-like (combing on motion). 'Bob Offset' is sharp but flickers.", NULL, "video",
      { {"blend","Blend (smooth, recommended for LCD)"},{"off","Off (renderer-side bob, full resolution)"},{"weave","Weave (CRT-like, combing on motion)"},{"bob","Bob"},{"bob_offset","Bob with offset (sharp, flickers)"},{"blend_rg","Blend (gamma-correct, more CPU)"},{NULL,NULL} }, "blend" },
   { "mednafen_stv_slstart", "First Scanline (NTSC)", NULL, NULL, NULL, "video",
      { {"0","0"},{"2","2"},{"4","4"},{"8","8"},{NULL,NULL} }, "8" },
   { "mednafen_stv_slend", "Last Scanline (NTSC)", NULL, NULL, NULL, "video",
      { {"239","239"},{"234","234"},{"231","231"},{"224","224"},{NULL,NULL} }, "231" },
   { "mednafen_stv_rotation", "Display Rotation", NULL,
      "Frontend display rotation. 'Auto' follows the game database: horizontal "
      "(yoko) games are not rotated, vertical (TATE) games are rotated 90° so they "
      "display upright on a horizontal screen, without stretching. Force a fixed "
      "value for special setups: '0' = no rotation (e.g. a physically-rotated TATE "
      "screen), '90'/'180'/'270' apply that absolute rotation to every game.", NULL, "video",
      { {"auto","Auto (game database)"},{"0","0 degrees (native orientation)"},{"90","90 degrees"},{"180","180 degrees"},{"270","270 degrees"},{NULL,NULL} }, "auto" },

   /* ── Audio ── */
   { "mednafen_stv_volume", "Audio Volume", NULL,
      "Output volume as a percentage. 100% is unchanged; lower attenuates, higher amplifies (useful for quiet titles). Above 100% may clip on loud scenes.", NULL, "audio",
      { {"50","50%"},{"75","75%"},{"100","100%"},{"125","125%"},{"150","150%"},{"175","175%"},{"200","200%"},{NULL,NULL} }, "100" },

   /* ── Performance ── */
   { "mednafen_stv_frameskip", "Frameskip", NULL,
      "'Auto' skips frames when the frontend signals video is not needed. '1'–'5' skips N frames between each rendered frame (manual). 'Disabled' renders every frame.", NULL, "performance",
      { {"disabled","Disabled"},{"auto","Auto"},{"1","1"},{"2","2"},{"3","3"},{"4","4"},{"5","5"},{NULL,NULL} }, "disabled" },
   { "mednafen_stv_cpu_cache", "CPU Cache Emulation", NULL,
      "SH-2 cache emulation level. 'Fast' skips instruction cache (recommended). 'Full' emulates both caches accurately but is slower. Restart required.", NULL, "performance",
      { {"data_cb","Fast (recommended)"},{"full","Full (accurate, slow)"},{NULL,NULL} }, "data_cb" },

   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL }
};

static struct retro_core_options_v2 options_us = {
   option_cats_us,
   option_defs_us
};

/*
 ********************************
 * Functions
 ********************************
 */

/* Handles configuration/setting of core options.
 * Should be called as early as possible - ideally inside
 * retro_set_environment(), and no later than retro_load_game()
 * (allows the frontend to set/get core options when displaying
 * the core options interface to the user).
 *
 * Down-converts the v2 (categorised) option definitions above to the v1 and
 * legacy (retro_variable) formats when the frontend only supports those, so the
 * definitions never need to be duplicated by hand. */
static void libretro_set_core_options(retro_environment_t environ_cb,
      bool *categories_supported)
{
   unsigned version  = 0;

   if (!environ_cb || !categories_supported)
      return;

   *categories_supported = false;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
            (void *)&options_us);
   }
   else
   {
      size_t i, j;
      size_t option_index              = 0;
      size_t num_options               = 0;
      struct retro_core_option_definition
            *option_v1_defs_us         = NULL;
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine total number of options */
      while (true)
      {
         if (option_defs_us[num_options].key)
            num_options++;
         else
            break;
      }

      if (version >= 1)
      {
         /* Allocate US array */
         option_v1_defs_us = (struct retro_core_option_definition *)
               calloc(num_options + 1, sizeof(struct retro_core_option_definition));

         if (!option_v1_defs_us)
            return;

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            const struct retro_core_option_v2_definition *option_def_us = &option_defs_us[i];
            const struct retro_core_option_value *option_values         = option_def_us->values;
            struct retro_core_option_definition *option_v1_def_us       = &option_v1_defs_us[i];
            struct retro_core_option_value *option_v1_values            = option_v1_def_us->values;

            option_v1_def_us->key           = option_def_us->key;
            option_v1_def_us->desc          = option_def_us->desc;
            option_v1_def_us->info          = option_def_us->info;
            option_v1_def_us->default_value = option_def_us->default_value;

            /* Values must be copied individually... */
            while (option_values->value)
            {
               option_v1_values->value = option_values->value;
               option_v1_values->label = option_values->label;

               option_values++;
               option_v1_values++;
            }
         }

         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_v1_defs_us);
      }
      else
      {
         /* Allocate arrays */
         variables  = (struct retro_variable *)calloc(num_options + 1,
               sizeof(struct retro_variable));
         values_buf = (char **)calloc(num_options, sizeof(char *));

         if (!variables || !values_buf)
            goto error;

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            const struct retro_core_option_v2_definition *option_def_us = &option_defs_us[i];
            const struct retro_core_option_value *option_values         = option_def_us->values;
            struct retro_variable *variable                             = &variables[option_index];
            size_t buf_len                                              = 3;
            size_t default_index                                        = 0;

            values_buf[i] = NULL;

            if (option_def_us->desc)
            {
               size_t num_values = 0;

               /* Determine number of values & compute buffer length */
               while (option_values[num_values].value)
               {
                  if (option_def_us->default_value &&
                      !strcmp(option_values[num_values].value, option_def_us->default_value))
                     default_index = num_values;

                  buf_len += strlen(option_values[num_values].value);
                  num_values++;
               }

               /* Build values string */
               if (num_values > 0)
               {
                  buf_len += num_values - 1;
                  buf_len += strlen(option_def_us->desc);

                  values_buf[i] = (char *)calloc(buf_len, sizeof(char));
                  if (!values_buf[i])
                     goto error;

                  strcpy(values_buf[i], option_def_us->desc);
                  strcat(values_buf[i], "; ");

                  /* Default value goes first */
                  strcat(values_buf[i], option_values[default_index].value);

                  /* Add remaining values */
                  for (j = 0; j < num_values; j++)
                  {
                     if (j != default_index)
                     {
                        strcat(values_buf[i], "|");
                        strcat(values_buf[i], option_values[j].value);
                     }
                  }
               }
            }

            variable->key   = option_def_us->key;
            variable->value = values_buf[i];
            option_index++;
         }

         /* Set variables */
         environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);

error:
         /* Clean up */
         if (values_buf)
         {
            for (i = 0; i < num_options; i++)
               free(values_buf[i]);

            free(values_buf);
            values_buf = NULL;
         }

         if (variables)
         {
            free(variables);
            variables = NULL;
         }
      }

      if (option_v1_defs_us)
      {
         free(option_v1_defs_us);
         option_v1_defs_us = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif
