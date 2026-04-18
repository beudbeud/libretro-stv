/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* db.cpp:
**  Copyright (C) 2016-2022 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
 Grandia could use full cache emulation to fix a hang at the end of disc 1, but
 FMVs make the emulator CPU usage too high; there's also currently a timing bug in
 the VDP1 frame swap/draw start code that causes Grandia to glitch out during gameplay
 with full cache emulation enabled.
*/

#include <mednafen/mednafen.h>
#include <mednafen/FileStream.h>
#include <mednafen/hash/crc.h>

#include "ss.h"
#include "smpc.h"
#include "cart.h"
#include "db.h"

namespace MDFN_IEN_SS
{

/* regiondb — Saturn CD-ROM region overrides.
 * Not used by the STV-only core (region comes from STVGameInfo.area). */
static const struct
{
 uint8 id[16];
 unsigned area;
 const char* game_name;
} regiondb[] = {};

static const struct
{
 const char* sgid;
 const char* sgname;
 int cart_type;
 const char* game_name;
 const char* purpose;
 uint8 fd_id[16];
} cartdb[] =
{
 /* Saturn CD-ROM cart type overrides — not used by STV core. */
};

static const struct
{
 const char* sgid;
 const char* sgname;
 const char* sgarea;
 unsigned mode;
 const char* game_name;
 const char* purpose;
 uint8 fd_id[16];
} cemdb[] =
{
 /* Saturn CD-ROM CPU cache emulation overrides — not used by STV core. */
};

void DB_Lookup(const char* path, const char* sgid, const char* sgname, const char* sgarea, const uint8* fd_id, unsigned* const region, int* const cart_type, unsigned* const cpucache_emumode)
{
 for(auto& re : regiondb)
 {
  if(!memcmp(re.id, fd_id, 16))
  {
   *region = re.area;
   break;
  }
 }

 for(auto& ca : cartdb)
 {
  bool match;

  if(ca.sgid)
  {
   match = !strcmp(ca.sgid, sgid);

   if(ca.sgname)
    match &= !strcmp(ca.sgname, sgname);
  }
  else
   match = !memcmp(ca.fd_id, fd_id, 16);

  if(match)
  {
   *cart_type = ca.cart_type;
   break;
  }
 }

 for(auto& c : cemdb)
 {
  bool match;

  if(c.sgid)
  {
   match = !strcmp(c.sgid, sgid);

   if(c.sgname)
    match &= !strcmp(c.sgname, sgname);

   if(c.sgarea)
    match &= !strcmp(c.sgarea, sgarea);
  }
  else
   match = !memcmp(c.fd_id, fd_id, 16);

  if(match)
  {
   *cpucache_emumode = c.mode;
   break;
  }
 }
}

static const struct
{
 const char* sgid;
 unsigned horrible_hacks;
 const char* game_name;
 const char* purpose;
 uint8 fd_id[16];
} hhdb[] =
{
 /* Saturn CD-ROM horrible hacks — not used by STV core.
  * STV always uses HORRIBLEHACK_VDP1RWDRAWSLOWDOWN unconditionally. */
};

uint32 DB_LookupHH(const char* sgid, const uint8* fd_id)
{
 for(auto& hh : hhdb)
 {
  if((hh.sgid && !strcmp(hh.sgid, sgid)) || (!hh.sgid && !memcmp(hh.fd_id, fd_id, 16)))
  {
   return hh.horrible_hacks;
  }
 }

 return 0;
}

//
//
//
//
//
//
//
//
//
static const STVGameInfo STVGI[] =
{
 // Broken(encryption)
 {
  "Astra SuperStars",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_315_5881,
   0x052E2901,  /* Kronos/MAME key */
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "epr20825.13" },
   { 0x0200000, 0x0100000, STV_MAP_16LE,  "epr20825.13" },  /* GAME_BYTE_BLOB reload */
   { 0x0300000, 0x0100000, STV_MAP_16LE,  "epr20825.13" },  /* GAME_BYTE_BLOB reload */

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20827.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20828.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20829.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20830.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20831.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20826.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20832.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr20833.9" },
  }
 },

 {
  "Baku Baku Animal",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr17969.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr17970.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr17971.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17972.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr17973.5" },
  }
 },

 // Broken, needs extra sound board emulation
 {
  "Batman Forever",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0100000, STV_MAP_BYTE, "350-mpa1.u19" },
   { 0x0200001, 0x0100000, STV_MAP_BYTE, "350-mpa1.u16" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "gfx0.u1" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "gfx1.u3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "gfx2.u5" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "gfx3.u8" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "gfx4.u12" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "gfx5.u15" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "gfx6.u18" },
  }
 },

 // Broken
 {
  "Choro Q Hyper Racing 5",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22.bin", 0x4F4D6229 },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24.bin" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26.bin" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28.bin" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30.bin" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "ic32.bin" },
   { 0x0E00000, 0x0200000, STV_MAP_16LE, "ic34.bin" },
   { 0x1000000, 0x0200000, STV_MAP_16LE, "ic36.bin" },
  }
 },

 {
  "Columns '97",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr19553.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19554.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19555.3" },
  }
 },

 {
  "Cotton 2",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr20122.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20117.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20118.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20119.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20120.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20121.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20116.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20123.8" },
  }
 },

 {
  "Cotton Boomerang",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr21075.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr21070.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr21071.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr21072.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr21073.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr21074.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr21069.1" },
  }
 },

 {
  "Critter Crusher",
  SMPC_AREA_EU_PAL,
  STV_CONTROL_HAMMER,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr-18821.ic13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr-18821.ic13" },

   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr-18789.ic8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr-18788.ic9" },
  }
 },

 {
  "DaeJeon! SanJeon SuJeon",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_SANJEON,
  false,
  {
   { 0x0000001, 0x0200000, STV_MAP_BYTE, "ic11", 0x0D30DA34 },
   { 0x0400000, 0x0200000, STV_MAP_16BE, "ic13" },
   { 0x0600000, 0x0200000, STV_MAP_16BE, "ic14" },
   { 0x0800000, 0x0200000, STV_MAP_16BE, "ic15" },
   { 0x0A00000, 0x0200000, STV_MAP_16BE, "ic16" },
   { 0x0C00000, 0x0200000, STV_MAP_16BE, "ic17" },
   { 0x0E00000, 0x0200000, STV_MAP_16BE, "ic18" },
   { 0x1000000, 0x0200000, STV_MAP_16BE, "ic19" },
   { 0x1200000, 0x0200000, STV_MAP_16BE, "ic20" },
   { 0x1400000, 0x0200000, STV_MAP_16BE, "ic21" },
   { 0x1600000, 0x0200000, STV_MAP_16BE, "ic22" },
   { 0x1800000, 0x0400000, STV_MAP_16BE, "ic12" },
  }
 },

 {
  "Danchi de Hanafuda",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr21974.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr21970.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr21971.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr21972.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr21973.5" },
  }
 },

 // TODO: needs special controller remapping?
 {
  "Danchi de Quiz",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22", 0xD2CAACB5 },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "ic32" },
   { 0x0E00000, 0x0200000, STV_MAP_16LE, "ic34" },
   { 0x1000000, 0x0200000, STV_MAP_16LE, "ic36" },
   { 0x1200000, 0x0200000, STV_MAP_16LE, "ic23" },
   { 0x1400000, 0x0200000, STV_MAP_16LE, "ic25" },
  }
 },

 // Broken
 {
  "Dancing Fever Gold",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000000, 0x0080000, STV_MAP_16LE, "13" },
   { 0x0080000, 0x0080000, STV_MAP_16LE, "13" },
   { 0x0100000, 0x0080000, STV_MAP_16LE, "13" },
#if 1
   { 0x0400000, 0x0400000, STV_MAP_16LE, "2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "1" },
#else
   { 0x0400000, 0x0400000, STV_MAP_16LE, "1" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "2" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "3" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "4" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "5" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "6" },
#endif
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "11" },
   { 0x2C00000, 0x0400000, STV_MAP_16LE, "12" },
  }
 },

 // Broken(encryption+compression)
 {
  "Decathlete (V1.000)",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_315_5838,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "epr18967.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18968.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18969.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18970.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18971.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18972.6" },
  }
 },

 // Broken(encryption+compression)
 {
  "Decathlete (V1.001)",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_315_5838,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "epr18967a.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18968.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18969.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18970.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18971.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18972.6" },
  }
 },

 {
  "Die Hard Arcade",
  SMPC_AREA_NA,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr19119.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19115.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19116.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19117.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr19118.5" },
  }
 },

 {
  "Dynamite Deka",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr19114.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19115.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19116.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19117.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr19118.5" },
  }
 },

 {
  "Ejihon Tantei Jimusyo",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr18137.13" },
   { 0x1000001, 0x0080000, STV_MAP_BYTE,  "epr18137.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18138.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18139.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18140.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18141.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18142.6" },
  }
 },

 {
  "Final Arch",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "finlarch.13" },
   { 0x0200001, 0x0100000, STV_MAP_BYTE,  "finlarch.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18257.2" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18257.2" },

   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18258.3" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr18258.3" },

   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18259.4" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr18259.4" },

   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18260.5" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr18260.5" },
  }
 },


 {
  "Final Fight Revenge",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_315_5881,
   0x0524AC01,  /* Kronos/MAME key */
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "ffr110.ic35" },

   { 0x0200000, 0x0200000, STV_MAP_16LE, "opr21872.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr21873.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr21874.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr21875.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr21876.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr21877.6" },
   { 0x1800000, 0x0200000, STV_MAP_16LE, "opr21878.1" },
  }
 },

 {
  "Find Love",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "epr20424.13" },

   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr20431.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20426.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20427.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20428.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20429.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20430.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20425.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20432.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr20433.9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr20434.10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr20435.11" },
   { 0x2C00000, 0x0400000, STV_MAP_16LE, "mpr20436.12" }
  }
 },

 {
  "Funky Head Boxers",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fr18541a.13" },
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr18538.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18533.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18534.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18535.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18536.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18537.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr18532.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr18539.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr18540.9" },
  }
 },

 {
  "Golden Axe: The Duel",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr17766.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr17766.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr17768.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr17769.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17770.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr17771.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr17772.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr17767.1" },
  }
 },

 {
  "Groove on Fight: Gouketsuji Ichizoku 3",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0100000, STV_MAP_16LE, "mpr19820.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19815.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19816.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19817.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr19818.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr19819.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr19814.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr19821.8" },
   { 0x2000000, 0x0200000, STV_MAP_16LE, "mpr19822.9" }
  }
 },

 {
  "Guardian Force",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr20844.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20839.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20840.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20841.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20842.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20843.6" },
  }
 },

 // Broken
 {
  "Hashire Patrol Car",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22.bin", 0x635EB1AF },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24.bin" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26.bin" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28.bin" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30.bin" },
  }
 },

 {
  "Karaoke Quiz Intro Don Don!",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE, "epr18937.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE, "epr18937.13" },
   { 0x0200000, 0x0100000, STV_MAP_16LE, "mpr18944.7" }, 
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18939.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18940.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18941.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18942.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18943.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr18938.1" },
  }
 },

 // Broken
 {
  "Magical Zunou Power",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "flash.ic13" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr-19354.ic2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr-19355.ic3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr-19356.ic4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr-19357.ic5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr-19358.ic6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr-19359.ic1" },
  }
 },

 {
  "Maru-Chan de Goo!",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "epr20416.13" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20417.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20418.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20419.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20420.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20421.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20422.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20423.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr20443.9" }
  }
 },

 {
  "Mausuke no Ojama the World",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "ic13.bin" },
   { 0x0200001, 0x0100000, STV_MAP_BYTE, "ic13.bin" },

   { 0x0400000, 0x0200000, STV_MAP_16LE, "mcj-00.2" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "mcj-01.3" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "mcj-02.4" },
   { 0x1000000, 0x0200000, STV_MAP_16LE, "mcj-03.5" },
   { 0x1400000, 0x0200000, STV_MAP_16LE, "mcj-04.6" },
   { 0x1800000, 0x0200000, STV_MAP_16LE, "mcj-05.1" },
   { 0x1C00000, 0x0200000, STV_MAP_16LE, "mcj-06.8" },
  }
 },

 // Broken
 {
  "Microman Battle Charge",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22", 0x83523F5E },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "ic32" },
   { 0x1000000, 0x0200000, STV_MAP_16LE, "ic34" },
   { 0x1200000, 0x0200000, STV_MAP_16LE, "ic36" },
  }
 },

 // Broken
 {
  "Nerae Super Goal",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22.bin", 0xC7B1A30B },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24.bin" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26.bin" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28.bin" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30.bin" },
  }
 },

 {
  "Othello Shiyouyo",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr20967.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20963.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20964.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20965.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20966.5" },
  }
 },

 {
  "Pebble Beach: The Great Shot",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr18852.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr18852.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18853.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18854.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18855.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18856.5" },
  }
 },

 // Broken
 {
  "Pro Mahjong Kiwame S",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr18737.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr18737.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18738.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18739.3" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "mpr18740.4" },
  }
 },

 {
  "Purikura Daisakusen",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr19337.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19333.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19334.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19335.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr19336.5" },
  }
 },

 {
  "Puyo Puyo Sun",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr19531.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr19531.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19533.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19534.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19535.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr19536.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr19537.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr19532.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr19538.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr19539.9" },
  }
 },

 {
  "Puzzle & Action: BoMulEul Chajara",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0080000, STV_MAP_BYTE,  "2.ic13_2" },
   { 0x0200001, 0x0080000, STV_MAP_BYTE,  "1.ic13_1" },

   { 0x0400000, 0x0400000, STV_MAP_16BE, "bom210-10.ic2" },
   { 0x1C00000, 0x0400000, STV_MAP_16BE, "bom210-10.ic2" },

   { 0x0800000, 0x0400000, STV_MAP_16BE, "bom210-11.ic3" },
   { 0x2000000, 0x0400000, STV_MAP_16BE, "bom210-11.ic3" },

   { 0x0C00000, 0x0400000, STV_MAP_16BE, "bom210-12.ic4" },
   { 0x2400000, 0x0400000, STV_MAP_16BE, "bom210-12.ic4" },

   { 0x1000000, 0x0400000, STV_MAP_16BE, "bom210-13.ic5" },
   { 0x2800000, 0x0400000, STV_MAP_16BE, "bom210-13.ic5" },
  }
 },

 {
  "Puzzle & Action: Sando-R",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "sando-r.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr18635.8" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr18635.8" },

   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr18636.9" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr18636.9" },

   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18637.10" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr18637.10" },

   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18638.11" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr18638.11" },
  }
 },

 {
  "Puzzle & Action: Treasure Hunt",
  SMPC_AREA_NA,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0080000, STV_MAP_BYTE,  "th-ic7_2.stv" },
   { 0x0200001, 0x0080000, STV_MAP_BYTE,  "th-ic7_1.stv" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "th-e-2.ic2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "th-e-3.ic3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "th-e-4.ic4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "th-e-5.ic5" },
  }
 },

 // 0x0600EDBC
 {
  "Radiant Silvergun",
  SMPC_AREA_JP,
  STV_CONTROL_RSG,
  STV_EC_CHIP_RSG,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr20958.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20959.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20960.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20961.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20962.5" },
  }
 },

 {
  "Sakura Taisen: Hanagumi Taisen Columns",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0100000, STV_MAP_16LE, "mpr20143.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20138.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20139.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20140.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20141.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20142.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20137.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20144.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr20145.9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr20146.10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr20147.11" },
   { 0x2C00000, 0x0400000, STV_MAP_16LE, "mpr20148.12" }
  }
 },

 {
  "Sea Bass Fishing",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "seabassf.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20551.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20552.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20553.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20554.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20555.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20550.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20556.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr20557.9" },
  }
 },

 {
  "Shanghai: The Great Wall",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr18341.7" },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "mpr18340.2" },
  }
 },

 {
  "Shienryu",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  true,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr19631.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19632.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19633.3" },
  }
 },

 // Broken
 {
  "Sky Challenger",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22.bin", 0x6AE68F06 },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24.bin" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26.bin" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28.bin" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30.bin" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "ic32.bin" },
  }
 },

 // Broken
 {
  "Soreyuke Anpanman Crayon Kids",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22.bin", 0x8483A390 },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24.bin" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26.bin" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28.bin" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30.bin" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "ic32.bin" },
   { 0x0E00000, 0x0200000, STV_MAP_16LE, "ic34.bin" },
   { 0x1000000, 0x0200000, STV_MAP_16LE, "ic36.bin" },
  }
 },

 {
  "Soukyu Gurentai",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr19188.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19189.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19190.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19191.4" },
   { 0x1000000, 0x0200000, STV_MAP_16LE, "mpr19192.5" },
  }
 },

 // Broken, needs custom BIOS and CD?
 {
  "Sport Fishing 2",
  SMPC_AREA_NA,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "epr-18427.ic13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr-18273.ic2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr-18274.ic3" },
   { 0x0C00000, 0x0200000, STV_MAP_16LE, "mpr-18275.ic4" },
  }
 },

 // Broken(encryption)
 {
  "Steep Slope Sliders",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_315_5881,
   0x052B6901,  /* Kronos/MAME key (Steep Slope Sliders) */
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr21488.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr21488.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr21489.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr21490.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr21491.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr21492.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr21493.6" },
  }
 },

 // Broken
 {
  "Stress Busters",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "epr-21300a.ic13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr-21290.ic2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr-21291.ic3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr-21292.ic4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr-21293.ic5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr-21294.ic6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr-21289.ic1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr-21296.ic8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr-21297.ic9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr-21298.ic10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr-21299.ic11" },
  }
 },

 {
  "Suiko Enbu",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr17834.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr17836.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr17837.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17838.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr17839.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr17840.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr17835.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr17841.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr17842.9" },
  }
 },

 {
  "Super Major League",
  SMPC_AREA_NA,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr18777.13" },
   { 0x1000001, 0x0080000, STV_MAP_BYTE,  "epr18777.13" },

   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr18778.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr18779.9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr18780.10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr18781.11" },
   { 0x2C00000, 0x0200000, STV_MAP_16LE, "mpr18782.12" },
  }
 },

 {
  "Taisen Tanto-R Sashissu!!",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE, "epr20542.13" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20544.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20545.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20546.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20547.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20548.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20543.1" },
  }
 },

 {
  "Tatacot",
  SMPC_AREA_JP,
  STV_CONTROL_HAMMER,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr-18790.ic13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr-18790.ic13" },

   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr-18789.ic8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr-18788.ic9" },
  }
 },


 // Broken
 {
  "Technical Bowling",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "ic22", 0xD426412C },
   { 0x0400000, 0x0200000, STV_MAP_16LE, "ic24" },
   { 0x0600000, 0x0200000, STV_MAP_16LE, "ic26" },
   { 0x0800000, 0x0200000, STV_MAP_16LE, "ic28" },
   { 0x0A00000, 0x0200000, STV_MAP_16LE, "ic30" },
  }
 },

 // Broken(encryption)
 {
  "Tecmo World Cup '98",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_315_5881,
   0x05200913,  /* Kronos/MAME key */
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "epr20819.24" },
   { 0x0200000, 0x0100000, STV_MAP_16LE,  "epr20819.24" },  /* ROM_RELOAD_PLAIN */
   { 0x0300000, 0x0100000, STV_MAP_16LE,  "epr20819.24" },  /* ROM_RELOAD_PLAIN */

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20821.12" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20822.13" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20823.14" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20824.15" },
  }
 },

 {
  "Tecmo World Soccer '98",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_315_5881,
   0x05200913,  /* Kronos/MAME key */
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "epr-20820.ic24" },
   { 0x0200000, 0x0100000, STV_MAP_16LE,  "epr-20820.ic24" },  /* ROM_RELOAD_PLAIN */
   { 0x0300000, 0x0100000, STV_MAP_16LE,  "epr-20820.ic24" },  /* ROM_RELOAD_PLAIN */

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20821.12" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20822.13" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20823.14" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20824.15" },
  }
 },

 {
  "Suiko Enbu / Outlaws of the Lost Dynasty",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr17834.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr17836.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr17837.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17838.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr17839.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr17840.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr17835.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr17841.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr17842.9" },
  }
 },

 // Broken(encryption)
 {
  "Touryuu Densetsu Elan Doree",
  SMPC_AREA_JP,
  STV_CONTROL_6B,
  STV_EC_CHIP_315_5881,
   0x05226D41,  /* Kronos/MAME key */
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr21307.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr21301.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr21302.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr21303.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr21304.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr21305.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr21306.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr21308.8" },
  }
 },

 {
  "Virtua Fighter Kids",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr18914.13" },

   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr18916.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr18917.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr18918.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr18915.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr18919.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr18920.9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr18921.10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr18922.11" },
   { 0x2C00000, 0x0400000, STV_MAP_16LE, "mpr18923.12" },
  }
 },

 {
  "Virtua Fighter Remix",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0080000, STV_MAP_BYTE,  "epr17944.13" },
   { 0x0100001, 0x0080000, STV_MAP_BYTE,  "epr17944.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr17946.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr17947.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17948.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr17949.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr17950.6" },
   { 0x1800000, 0x0200000, STV_MAP_16LE, "mpr17945.1" }
  }
 },

 // Broken(needs special controller)
 {
  "Virtual Mahjong",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr19620.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr19615.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr19616.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19617.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr19618.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr19619.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr19614.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr19621.8" },
  }
 },

 // Broken(needs special controller)
 {
  "Virtual Mahjong 2: My Fair Lady",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0200000, 0x0200000, STV_MAP_16LE, "mpr21000.7" },
   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20995.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20996.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20997.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20998.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20999.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20994.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr21001.8" },
  }
 },

 {
  "Winter Heat",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "fpr20108.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20110.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20111.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20112.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20113.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20114.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20109.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20115.8" },
  }
 },

 // Broken
 {
  "Yatterman Plus",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   //{ 0x0000000, 0x0080000, STV_MAP_16LE,  "epr-21122.ic13" },
   //{ 0x0080000, 0x0080000, STV_MAP_16LE,  "epr-21122.ic13" },
   //{ 0x0080001, 0x0020000, STV_MAP_BYTE,   "epr-21121.bin" },
   //{ 0x0030001, 0x0020000, STV_MAP_BYTE,   "epr-21121.bin" },
   { 0x0200000, 0x0080000, STV_MAP_16LE,  "epr-21122.ic13" },
   { 0x0280000, 0x0080000, STV_MAP_16LE,  "epr-21122.ic13" },


   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr-21125.ic02" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr-21130.ic03" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr-21126.ic04" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr-21131.ic05" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr-21127.ic06" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr-21132.ic07" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr-21128.ic08" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr-21133.ic09" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr-21129.ic10" },
   { 0x2800000, 0x0400000, STV_MAP_16LE, "mpr-21124.ic11" },
   { 0x2C00000, 0x0400000, STV_MAP_16LE, "mpr-21123.ic12" },
  }
 },

 {
  "Zen Nippon Pro-Wrestling Featuring Virtua",
  SMPC_AREA_JP,
  STV_CONTROL_3B,
  STV_EC_CHIP_NONE,
   0x00000000,
  STV_ROMTWIDDLE_NONE,
  false,
  {
   { 0x0000001, 0x0100000, STV_MAP_BYTE,  "epr20398.13" },

   { 0x0400000, 0x0400000, STV_MAP_16LE, "mpr20400.2" },
   { 0x0800000, 0x0400000, STV_MAP_16LE, "mpr20401.3" },
   { 0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20402.4" },
   { 0x1000000, 0x0400000, STV_MAP_16LE, "mpr20403.5" },
   { 0x1400000, 0x0400000, STV_MAP_16LE, "mpr20404.6" },
   { 0x1800000, 0x0400000, STV_MAP_16LE, "mpr20399.1" },
   { 0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20405.8" },
   { 0x2000000, 0x0400000, STV_MAP_16LE, "mpr20406.9" },
   { 0x2400000, 0x0400000, STV_MAP_16LE, "mpr20407.10" },
  }
 },
};

const STVGameInfo* DB_LookupSTV(const std::string& fname, Stream* s,
                                 VirtualFS* vfs, const std::string& dir)
{
 uint8 tmp[0x80];
 uint32 dr;
 uint32 head_crc32;

 dr = s->read(tmp, sizeof(tmp), false);

 s->rewind();

 head_crc32 = crc32_zip(0, tmp, dr);

 //printf("%s 0x%08X\n", fname.c_str(), head_crc32);

 /* Pass 1: original behaviour — match against first rom_layout entry */
 for(const STVGameInfo& e : STVGI)
 {
  auto const& rle = e.rom_layout[0];

  if(!MDFN_strazicmp(fname, rle.fname))
  {
   if(!rle.head_crc32 || head_crc32 == rle.head_crc32)
    return &e;
  }
 }

 /* Pass 2: match against any secondary rom_layout entry.
  * When the ZIP presents a shared ROM first (e.g. mpr20822.13 shared by
  * twcup98 and twsoc98), we confirm identity by checking whether the
  * game's identifying first ROM (EPR) exists in the same archive/dir.  */
 if(vfs)
 {
  for(const STVGameInfo& e : STVGI)
  {
   const size_t n = sizeof(e.rom_layout) / sizeof(e.rom_layout[0]);

   for(size_t ri = 1; ri < n && e.rom_layout[ri].size; ri++)
   {
    if(!MDFN_strazicmp(fname, e.rom_layout[ri].fname))
    {
     /* Try to open the identifying first entry (EPR) to disambiguate */
     const std::string epr_path = vfs->eval_fip(dir, e.rom_layout[0].fname);
     try
     {
      std::unique_ptr<Stream> es(vfs->open(epr_path, VirtualFS::MODE_READ));
      return &e;   /* EPR exists → this is the right game */
     }
     catch(...) {}  /* EPR not present → wrong game, try next */
     break;
    }
   }
  }
 }

 return nullptr;
}

static std::string FDIDToString(const uint8 (&fd_id)[16])
{
 return MDFN_sprintf("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", fd_id[0], fd_id[1], fd_id[2], fd_id[3], fd_id[4], fd_id[5], fd_id[6], fd_id[7], fd_id[8], fd_id[9], fd_id[10], fd_id[11], fd_id[12], fd_id[13], fd_id[14], fd_id[15]);
}

uint32 DB_GetSTVHacks(const STVGameInfo* sgi)
{
 /* Games whose sprite rendering suffers from VDP1 time-sliced drawing being
  * aborted mid-frame by the framebuffer swap. HORRIBLEHACK_VDP1INSTANT makes
  * the command list execute atomically at the start of each frame, so
  * partially-drawn sprites cannot leak into the displayed buffer. */
 static const char* const vdp1_instant_draw[] =
 {
  "Astra SuperStars",  /* 315-5881: stripes caused by time-sliced drawing vs FB swap timing */
 };

 /* Puzzle, board, quiz, card, fishing, golf, and other low-sprite-throughput
  * games do not trigger the VDP1 bus-contention issue that
  * HORRIBLEHACK_VDP1RWDRAWSLOWDOWN compensates for.  Every other title
  * (fighters, shooters, beat-em-ups, sports with heavy sprite use) keeps the
  * hack so that VDP1 rendering stays in sync with the real hardware cadence. */
 static const char* const no_vdp1_slowdown[] =
 {
  "Astra SuperStars",  /* 315-5881: dense VRAM writes from ROM texture transfers exhaust VDP1 CycleCounter */
  "Baku Baku Animal",
  "Columns '97",
  "Critter Crusher",
  "DaeJeon! SanJeon SuJeon",
  "Danchi de Hanafuda",
  "Danchi de Quiz",
  "Ejihon Tantei Jimusyo",
  "Find Love",
  "Karaoke Quiz Intro Don Don!",
  "Magical Zunou Power",
  "Maru-Chan de Goo!",
  "Mausuke no Ojama the World",
  "Othello Shiyouyo",
  "Pebble Beach: The Great Shot",
  "Pro Mahjong Kiwame S",
  "Purikura Daisakusen",
  "Puyo Puyo Sun",
  "Puzzle & Action: BoMulEul Chajara",
  "Puzzle & Action: Sando-R",
  "Puzzle & Action: Treasure Hunt",
  "Sakura Taisen: Hanagumi Taisen Columns",
  "Sea Bass Fishing",
  "Shanghai: The Great Wall",
  "Sport Fishing 2",
  "Super Major League",
  "Taisen Tanto-R Sashissu!!",
  "Tatacot",
  "Technical Bowling",
  "Virtual Mahjong",
  "Virtual Mahjong 2: My Fair Lady",
 };

 uint32 hacks = HORRIBLEHACK_VDP1RWDRAWSLOWDOWN;

 for(const char* name : no_vdp1_slowdown)
  if(!strcmp(sgi->name, name))
  {
   hacks = 0;
   break;
  }

 for(const char* name : vdp1_instant_draw)
  if(!strcmp(sgi->name, name))
  {
   hacks |= HORRIBLEHACK_VDP1INSTANT;
   break;
  }

 return hacks;
}

std::string DB_GetHHDescriptions(const uint32 hhv)
{
 std::string sv;

 if(hhv & HORRIBLEHACK_NOSH2DMALINE106)
  sv += "Block SH-2 DMA on last line of frame. ";

 if(hhv & HORRIBLEHACK_NOSH2DMAPENALTY)
  sv += "Disable slowing down of SH-2 CPU reads/writes during SH-2 DMA. ";

 if(hhv & HORRIBLEHACK_VDP1VRAM5000FIX)
  sv += "Patch VDP1 VRAM to break an infinite loop. ";

 if(hhv & HORRIBLEHACK_VDP1RWDRAWSLOWDOWN)
  sv += "SH-2 reads/writes from/to VDP1 slow down command execution. ";

 if(hhv & HORRIBLEHACK_VDP1INSTANT)
  sv += "Execute VDP1 commands instantly. ";

/*
 if(hhv & HORRIBLEHACK_SCUINTDELAY)
  sv += "Delay SCU interrupt generation after a write to SCU IMS unmasks a pending interrupt. ";
*/

 return sv;
}

void DB_GetInternalDB(std::vector<GameDB_Database>* databases)
{
 databases->push_back({
	"region",
	gettext_noop("Region"),
	gettext_noop("This database is used in conjunction with a game's internal header and the \"\5ss.region_default\" setting to automatically select the region of Saturn to emulate when the \"\5ss.region_autodetect\" setting is set to \"1\", the default.")
	});

 for(auto& re : regiondb)
 {
  const char* sv = nullptr;

  switch(re.area)
  {
   default: assert(0); break;
   case SMPC_AREA_JP: sv = _("Japan"); break;
   case SMPC_AREA_ASIA_NTSC: sv = _("Asia NTSC"); break;
   case SMPC_AREA_NA: sv = _("North America"); break;
   case SMPC_AREA_CSA_NTSC: sv = _("Brazil"); break;
   case SMPC_AREA_KR: sv = _("South Korea"); break;
   case SMPC_AREA_ASIA_PAL: sv = _("Asia PAL"); break;
   case SMPC_AREA_EU_PAL: sv = _("Europe"); break;
  }
  //
  //
  GameDB_Entry e;

  e.GameID = FDIDToString(re.id);
  e.GameIDIsHash = true;
  e.Name = re.game_name;
  e.Setting = sv;
  e.Purpose = ""; //ca.purpose ? _(ca.purpose) : "";

  databases->back().Entries.push_back(e);
 }
 //
 //
 //
 databases->push_back({
	"cart",
	gettext_noop("Cart"),
	gettext_noop("This database is used to automatically select the type of cart to emulate when the \"\5ss.cart\" setting is set to \"auto\", the default.  If a game is not found in the database when auto selection is enabled, then the cart used is specified by the \"\5ss.cart.auto_default\" setting, default \"backup\"(a backup memory cart).")
	});

 for(auto& ca : cartdb)
 {
  const char* sv = nullptr;

  switch(ca.cart_type)
  {
   default: assert(0); break;
   case CART_NONE: sv = "None"; break;
   case CART_BACKUP_MEM: sv = "Backup Memory"; break;
   case CART_EXTRAM_1M: sv = "1MiB Extended RAM"; break;
   case CART_EXTRAM_4M: sv = "4MiB Extended RAM"; break;
   case CART_KOF95: sv = "King of Fighters 95 ROM"; break;
   case CART_ULTRAMAN: sv = "Ultraman ROM"; break;
   case CART_NLMODEM: sv = "Netlink Modem"; break;
   case CART_CS1RAM_16M: sv = "16MiB A-bus CS1 RAM"; break;
  }
  //
  //
  GameDB_Entry e;

  if(ca.sgid)
  {
   unsigned lfcount = 0;

   e.GameIDIsHash = false;
   e.GameID = ca.sgid;

   if(ca.sgname)
   {
    for(; lfcount < 1; lfcount++)
     e.GameID += '\n';
    e.GameID += ca.sgname;
   }
  }
  else
  {
   e.GameIDIsHash = true;
   e.GameID = FDIDToString(ca.fd_id);
  }

  e.Name = ca.game_name;
  e.Setting = sv;
  e.Purpose = ca.purpose ? _(ca.purpose) : "";

  databases->back().Entries.push_back(e);
 }
 //
 //
 //
 databases->push_back({
	"cachemode",
	gettext_noop("Cache Mode"),
	gettext_noop("This database is used to automatically select cache emulation mode, to fix various logic and timing issues in games.  The default cache mode is data-only(with no high-level bypass).\n\nThe cache mode \"Data-only, with high-level bypass\" is a hack of sorts, to work around cache coherency bugs in games.  These bugs are typically masked on a real Saturn due to the effects of instruction fetches on the cache, but become a problem when only data caching is emulated.\n\nFull cache emulation is not enabled globally primarily due to the large increase in host CPU usage.\n\nFor ST-V games, this database is not used, and instead full cache emulation is always enabled.")
	});
 for(auto& c : cemdb)
 {
  const char* sv = nullptr;

  switch(c.mode)
  {
   default: assert(0); break;
   case CPUCACHE_EMUMODE_DATA_CB: sv = _("Data only, with high-level bypass"); break;
   case CPUCACHE_EMUMODE_FULL: sv = _("Full"); break;
  }
  GameDB_Entry e;

  if(c.sgid)
  {
   unsigned lfcount = 0;

   e.GameIDIsHash = false;
   e.GameID = c.sgid;

   if(c.sgname)
   {
    for(; lfcount < 1; lfcount++)
     e.GameID += '\n';
    e.GameID += c.sgname;
   }

   if(c.sgarea)
   {
    for(; lfcount < 2; lfcount++)
     e.GameID += '\n';
    e.GameID += c.sgarea;
   }
  }
  else
  {
   e.GameIDIsHash = true;
   e.GameID = FDIDToString(c.fd_id);
  }
  e.Name = c.game_name;
  e.Setting = sv;
  e.Purpose = c.purpose ? _(c.purpose) : "";

  databases->back().Entries.push_back(e);
 }
 //
 //
 //
 databases->push_back({
	"horriblehacks",
	gettext_noop("Horrible Hacks"),
	gettext_noop("This database is used to automatically enable various horrible hacks to fix issues in certain games.\n\nNote that slowing down VDP1 command execution due to SH-2 reads/writes isn't a horrible hack per-se, but it's activated on a per-game basis to avoid the likelihood of breaking some games due to overall Saturn emulation timing inaccuracies.\n\nFor ST-V games, this database is not used, and instead VDP1 command slowdown on SH-2 reads/writes is always enabled.")
	});
 for(auto& hh : hhdb)
 {
  std::string sv = DB_GetHHDescriptions(hh.horrible_hacks);
  GameDB_Entry e;

  e.GameID = hh.sgid ? hh.sgid : FDIDToString(hh.fd_id);
  e.GameIDIsHash = !hh.sgid;
  e.Name = hh.game_name;
  e.Setting = sv;
  e.Purpose = hh.purpose ? _(hh.purpose) : "";

  databases->back().Entries.push_back(e);
 }
}

}
