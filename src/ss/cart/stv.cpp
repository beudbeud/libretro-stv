/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* stv.cpp - ST-V Cart Emulation
**  Copyright (C) 2022 Mednafen Team
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
 TODO:
	315-5881 decryption support
*/


#include "common.h"
#include "stv.h"
#include "../db.h"
#include "chip315_5838.h"
#include "chip315_5881.h"

#include <mednafen/hash/sha256.h>
#include <mednafen/Time.h>

namespace MDFN_IEN_SS
{

static unsigned ECChip;

static uint16* ROM;
#ifdef MDFN_ENABLE_DEV_BUILD
static uint8 ROM_Mapped[0x3000000 / sizeof(uint16)];
#endif

static uint8 rsg_thingy;
static uint8 rsg_counter;

/* 315-5838 decompression chip state (Decathlete) */
static Chip5838 chip5838;

/* 315-5881 encryption chip state (Astra, Final Fight, Steep Slope, Tecmo, Elan) */
static Chip5881 chip5881;

static MDFN_HOT void ROM_Read(uint32 A, uint16* DB)
{
 /* 315-5838: MAME decathlt_prot_r at 0x27FFFF8-B, 0x2FFFFF8-B, 0x37FFFF8-B.
  * MAME sets the ROM bank only on WRITE, never on READ — reads just clock
  * out whatever has been decompressed from the bank last selected by a
  * srcaddr write.  Setting the bank on read would corrupt an in-flight
  * decompression if the CPU reads from a different bank mirror than the
  * one used for srcaddr. */
 if(MDFN_UNLIKELY(ECChip == STV_EC_CHIP_315_5838 && chip5838.active))
 {
  const uint32 off = (A - 0x02000000) & 0x7FFFFF;
  if(MDFN_UNLIKELY(off >= 0x7FFFF8))
  {
   *DB = chip5838.data_r();
   return;
  }
 }

 /* 315-5881 chip read — MAME common_prot_r at 0x04FFFFF0-0x04FFFFFF.
  * Only offset 3 (0x04FFFFFC/FE) returns decrypted data when chip is enabled.
  * Other offsets return the last value written (mirrored) or ROM fallback. */
 if(MDFN_UNLIKELY(ECChip == STV_EC_CHIP_315_5881 && A >= 0x04FFFFF0 && A <= 0x04FFFFFF))
 {
  const uint32 dw_off = (A - 0x04FFFFF0) >> 2;  /* DWORD offset 0-3 */
  if(chip5881.protenable & 0x00010000)            /* chip enabled */
  {
   if(dw_off == 3)                                /* offset 3 = data read */
   {
    *DB = chip5881.decrypt_le_r();
   }
   else
   {
    /* MAME common_prot_r returns m_a_bus[offset] (last-written word).
     * Returning 0xFFFF was wrong — the game may read back registers to
     * verify chip state, causing re-init with garbage parameters. */
    const uint32 word_idx = (A - 0x04FFFFF0) >> 1;
    *DB = chip5881.a_bus[word_idx & 7];
   }
  }
  else
  {
   /* Chip disabled: MAME returns ROM data at 0x02FFFFF0 + offset */
   *DB = *(uint16*)((uint8*)ROM + ((A - 0x2000000) & 0x3FFFFFE));  }
  return;
 }

 *DB = *(uint16*)((uint8*)ROM + ((A - 0x2000000) & 0x3FFFFFE));

 //printf("ROM %08x %04x\n", A, *DB);

 //if(A >= 0x04FFFFF0)
 // printf("Unknown read %08x\n", A);

 if(A >= 0x04FFFFFC && rsg_thingy)
 {
  *DB = (((((rsg_counter & 0x7F) << 1) + 0) << 8) | ((((rsg_counter & 0x7F) << 1) + 1) << 0)) & (0xF0F0 >> ((rsg_counter & 0x80) >> 5));
  rsg_counter++;
 }

#ifdef MDFN_ENABLE_DEV_BUILD
 if(!ROM_Mapped[((A - 0x2000000) & 0x3FFFFFE) >> 1])
 {
  SS_DBG(SS_DBG_WARNING, "[CART-STV] Unmapped ROM: %08x %04x\n", A, *DB);
 }
#endif
}

uint8 CART_STV_PeekROM(uint32 A)
{
 assert(A < 0x3000000);

 return ne16_rbo_be<uint8>(ROM, A);
}

template<typename T>
static MDFN_HOT void Write(uint32 A, uint16* DB)
{
 if(ECChip == STV_EC_CHIP_315_5838)
 {
  /* 315-5838 (Decathlete) — MAME decathlt_prot_srcaddr_w:
   * Bank is set from the WRITE address bits [24:23] on every write to
   * the cart range, independent of whether the offset targets a chip
   * register.  Register dispatch is at 4-byte aligned offsets 0x7FFFF0
   * (srcaddr) and 0x7FFFF4 (data_w); SH-2 32-bit writes are split by
   * the bus into two 16-bit writes so the HI/LO halves land at
   *   0x7FFFF0 (hi) / 0x7FFFF2 (lo) → srcaddr combine
   *   0x7FFFF4 (hi) → mode_w,  0x7FFFF6 (lo) → data_w                 */
  if(sizeof(T) == 2)
  {
   const uint32 off  = (A - 0x02000000) & 0x7FFFFF;
   const uint32 bank = ((A - 0x02000000) & 0x1800000) >> 23;
   chip5838.set_bank(bank);

   if(MDFN_UNLIKELY(off == 0x7FFFF0))
   {
    chip5838.pending_srcaddr_hi = (uint32)*DB << 16;
    return;
   }
   if(MDFN_UNLIKELY(off == 0x7FFFF2))
   {
    uint32 full = (chip5838.pending_srcaddr_hi | *DB) & 0x007FFFFF;
    chip5838.pending_srcaddr_hi = 0;
    chip5838.srcaddr_w16(full);
    return;
   }
   if(MDFN_UNLIKELY(off == 0x7FFFF4))
   {
    chip5838.set_table_upload_mode_w(*DB);
    return;
   }
   if(MDFN_UNLIKELY(off == 0x7FFFF6))
   {
    chip5838.upload_table_data_w(*DB);
    return;
   }
  }
  return;
 }

 if(ECChip == STV_EC_CHIP_315_5881)
 {
  /* 315-5881 — MAME common_prot_w at 0x04FFFFF0-0x04FFFFFF:
   *  offset 0 (0x04FFFFF0-3) → protenable  (bit 0x00010000 = enable)
   *  offset 2 (0x04FFFFF8-B) → addr_low / addr_high
   *  offset 3 (0x04FFFFFC-F) → subkey
   * SH-2 may use byte writes (MOV.B). In mednafen big-endian byte convention:
   *   odd  address (A&1=1): byte = (DB >> 8) & 0xFF  (high byte of DB)
   *   even address (A&1=0): byte = DB & 0xFF          (low byte of DB)
   * Byte N (A&3=N) maps to bits [(3-N)*8 +7 : (3-N)*8] of the 32-bit word. */
  if(A >= 0x04FFFFF0 && A <= 0x04FFFFFF)
  {
   const uint32 dw_off = (A - 0x04FFFFF0) >> 2;
   uint32 val32 = 0;
   uint32 mask32 = 0;

   if(sizeof(T) == 2)
   {
    /* 16-bit write: update high or low half of the 32-bit word */
    const bool hi = !((A >> 1) & 1);
    if(hi) { val32 = (uint32)*DB << 16; mask32 = 0xFFFF0000; }
    else   { val32 = *DB;               mask32 = 0x0000FFFF; }
    /* Mirror into a_bus (word granularity, matches MAME m_a_bus) */
    const uint32 word_idx = (A - 0x04FFFFF0) >> 1;
    chip5881.a_bus[word_idx & 7] = *DB;
   }
   else  /* sizeof(T) == 1: byte write */
   {
    /* odd addr → high byte of DB; even addr → low byte of DB */
    const uint8 bval = (A & 1) ? ((*DB >> 8) & 0xFF) : (*DB & 0xFF);
    const uint32 shift = (3 - (A & 3)) * 8;  /* BE bit position */
    val32  = (uint32)bval << shift;
    mask32 = 0xFFU << shift;
    /* Mirror byte into a_bus word — update the relevant byte in the 16-bit slot */
    const uint32 word_idx = (A - 0x04FFFFF0) >> 1;
    const uint32 byte_in_word = (A & 1) ^ 1;  /* BE: addr&1=0 → high byte, addr&1=1 → low byte */
    chip5881.a_bus[word_idx & 7] =
     (chip5881.a_bus[word_idx & 7] & ~(0xFF << (byte_in_word * 8))) |
     ((uint16_t)bval << (byte_in_word * 8));
   }

   if(dw_off == 0)  /* protenable */
   {
    chip5881.protenable = (chip5881.protenable & ~mask32) | val32;
   }
   else if(dw_off == 2)  /* source address */
   {
    if(mask32 & 0xFFFF0000)  /* affects high half → addr_low */
    {
     chip5881.set_addr_low((val32 >> 16) & 0xFFFF);    }
    if(mask32 & 0x0000FFFF)  /* affects low half → addr_high */
    {
     chip5881.set_addr_high(val32 & 0xFFFF);    }
   }
   else if(dw_off == 3)  /* subkey */
   {
    if(mask32 & 0xFFFF0000)
    {
     chip5881.set_subkey((val32 >> 16) & 0xFFFF);    }
   }
   else
   {   }
  }
  return;
 }

 if(A >= 0x04FFFFF0 && ECChip == STV_EC_CHIP_RSG)
 {
  if(sizeof(T) == 2 || (A & 1))
  {
   if((A & ~1) == 0x04FFFFF0)
   {
    rsg_thingy = *DB & 0x1;
    rsg_counter = 0;
   }
  }
 }
}

static CartInfo* g_CartPtr = nullptr;  /* Save cart pointer for diagnostics */

/* Exposed to scu.inc dual dispatch.
 * MAME connects the 315-5838 on CS01 only — CS2 accesses must NOT be
 * routed to the chip, otherwise routine I/O-board polls (inputs, coin,
 * test switch) during gameplay would consume Huffman stream words and
 * corrupt sprite decompression.  Always return false. */
bool CART_STV_Chip5838IsActive(void) noexcept
{
 return false;
}

static void Reset(bool powering_up)
{
 rsg_thingy = 0;
 rsg_counter = 0;
 if(ECChip == STV_EC_CHIP_315_5838)
  chip5838.reset();
 if(ECChip == STV_EC_CHIP_315_5881)
  chip5881.reset();
 /* Diagnostic: verify is_stv flag */
 if(g_CartPtr)
  MDFN_printf("[CART-STV] Reset: is_stv=%d ECChip=%u\n",
   (int)g_CartPtr->is_stv, ECChip);
}

static void StateAction(StateMem* sm, const unsigned load, const bool data_only)
{
 SFORMAT StateRegs[] =
 {
  SFVAR(rsg_thingy),
  SFVAR(rsg_counter),
  /* 315-5838 chip state */
  SFVAR(chip5838.srcoffset),
  SFVAR(chip5838.srcstart),
  SFVAR(chip5838.abort),
  SFVAR(chip5838.val_compressed),
  SFVAR(chip5838.num_bits_compressed),
  SFVAR(chip5838.val),
  SFVAR(chip5838.num_bits),
  SFVAR(chip5838.active_bank),
  SFPTR8N((uint8*)&chip5838.cs, sizeof(chip5838.cs), "chip5838_cs"),

  /* 315-5881 chip state */
  SFVAR(chip5881.prot_cur_address),
  SFVAR(chip5881.subkey),
  SFVAR(chip5881.protenable),
  SFVAR(chip5881.enc_ready),
  SFVAR(chip5881.dec_hist),
  SFVAR(chip5881.dec_header),
  SFVAR(chip5881.buffer_pos),
  SFVAR(chip5881.buffer_bit),
  SFVAR(chip5881.buffer_bit2),
  SFVAR(chip5881.buffer2a),
  SFVAR(chip5881.line_buffer_pos),
  SFVAR(chip5881.line_buffer_size),
  SFPTR8N(chip5881.buffer, BUFFER_SIZE, "chip5881_buf"),
  SFPTR8N(chip5881.line_buffer, LINE_SIZE, "chip5881_lb"),
  SFPTR8N(chip5881.line_buffer_prev, LINE_SIZE, "chip5881_lbp"),
  SFPTR16N(chip5881.a_bus, 8, "chip5881_abus"),

  SFEND
 };

 MDFNSS_StateAction(sm, load, data_only, StateRegs, "STV_CART");

 if(load)
 {

 }
}

static void Kill(void)
{
 if(ROM)
 {
  delete[] ROM;
  ROM = nullptr;
 }
}



/* ── A-bus CS2 handlers (0x05800000-0x058FFFFF) ─────────────────────────
 *
 * 315-5838 register map discovered empirically (Decathlete):
 *   CS2 offset 0x08  (idx 4) : srcaddr_w16 / data_r
 *   CS2 offset 0x0a  (idx 5) : srcaddr_w16 second half / data_r
 *   CS2 offset 0x18  (idx 12): set_table_upload_mode_w
 *   CS2 offset 0x1a-0x26 (13-19): upload_table_data_w / data_r
 *
 * All reads return decompressed output (data_r).
 * Writes to even offsets (0x18,0x1c,0x20,0x24) = set_table_upload_mode_w
 * Writes to odd  offsets (0x1a,0x1e,0x22,0x26) = upload_table_data_w     */


/* CS2 handlers — the ST-V protection chips (315-5838, 315-5881) live on
 * CS01 in MAME, not CS2.  CS2 at 0x05800000+ is the A-bus I/O board
 * (coin, test, service, player inputs) and must be left untouched.
 * These stubs just return open-bus for reads and ignore writes. */
static MDFN_HOT void CS2_Read_diag(uint32 A, uint16* DB)
{
 *DB = 0xFFFF;
}

static MDFN_HOT void CS2_Write8_diag(uint32 A, uint16* DB)
{
}

static MDFN_HOT void CS2_Write16_diag(uint32 A, uint16* DB)
{
}

void CART_STV_Init(CartInfo* c, GameFile* gf, const STVGameInfo* sgi)
{
 assert(gf && sgi);

 try
 {
  const std::string fname = gf->fbase + (gf->ext.size() ? "." : "") + gf->ext;
  sha256_hasher h;

  ECChip = sgi->ec_chip;
  c->is_stv = true;
  g_CartPtr = c;
  MDFN_printf("[CART-STV] Init: is_stv=%d ECChip=%u\n", (int)c->is_stv, ECChip);

  ROM = new uint16[0x3000000 / sizeof(uint16)];
  memset(ROM, 0xFF, 0x3000000);
#ifdef MDFN_ENABLE_DEV_BUILD
  memset(ROM_Mapped, 0x00, sizeof(ROM_Mapped));
#endif

  for(size_t i = 0; i < sizeof(sgi->rom_layout) / sizeof(sgi->rom_layout[0]) && sgi->rom_layout[i].size; i++)
  {
   const STVROMLayout* rle = &sgi->rom_layout[i];
   const STVROMLayout* prev_rle = i ? &sgi->rom_layout[i - 1] : nullptr;
   const bool gf_fname_match = !MDFN_strazicmp(fname, rle->fname);
   const std::string fpath = gf->vfs->eval_fip(gf->dir, gf_fname_match ? fname : rle->fname);
   /* prev_match: same file already loaded — copy instead of re-reading.
    * Require same map type so that RELOAD_PLAIN (STV_MAP_16LE) after a
    * STV_MAP_BYTE entry loads fresh from the ZIP (= plain sequential bytes). */
   const bool prev_match = prev_rle && !strcmp(rle->fname, prev_rle->fname)
                           && rle->map == prev_rle->map && rle->size == prev_rle->size;
   std::unique_ptr<Stream> ns;
   Stream* s;

   if(gf_fname_match)
   {
    s = gf->stream;
    s->rewind();
   }
   else
   {
    ns.reset(gf->vfs->open(fpath, VirtualFS::MODE_READ));
    s = ns.get();
   }

#ifdef MDFN_ENABLE_DEV_BUILD
   memset(ROM_Mapped + (rle->offset >> 1), 0xFF, rle->size >> (rle->map != STV_MAP_BYTE));
#endif

   if(prev_match)
   {
    assert(rle->size == prev_rle->size);
    assert(rle->map == prev_rle->map);

    if(rle->map == STV_MAP_BYTE)
    {
     for(uint32 j = 0; j < rle->size; j++)
     {
      uint8 tmp = ne16_rbo_be<uint8>(ROM, prev_rle->offset + (j << 1));

      ne16_wbo_be<uint8>(ROM, rle->offset + (j << 1), tmp); 
     }
    }
    else
     memmove((uint8*)ROM + rle->offset, (uint8*)ROM + prev_rle->offset, rle->size);
   }
   else if(rle->map == STV_MAP_BYTE)
   {
    for(uint32 j = 0; j < rle->size; j++)
    {
     uint8 tmp;

     if(s->read(&tmp, 1, false) != 1)
      throw MDFN_Error(0, _("ROM image file %s is %u bytes smaller than the required size of %u bytes."), gf->vfs->get_human_path(fpath).c_str(), rle->size - j, rle->size);

     h.process(&tmp, 1);

     ne16_wbo_be<uint8>(ROM, rle->offset + (j << 1), tmp);
    }
   }
   else
   {
    assert(!(rle->offset & 1));
    assert(!(rle->size & 1));
    //
    uint8* dest = (uint8*)ROM + rle->offset;
    uint32 size = rle->size;
    uint32 dr;

    if((dr = s->read(dest, size, false)) != size)
     throw MDFN_Error(0, _("ROM image file %s is %u bytes smaller than the required size of %u bytes."), gf->vfs->get_human_path(fpath).c_str(), rle->size - dr, rle->size);

    h.process(dest, size);

    if(rle->map == STV_MAP_16LE)
     Endian_A16_NE_LE(dest, size >> 1);
    else
     Endian_A16_NE_BE(dest, size >> 1);
   }

   if(!prev_match)
   {
    const uint64 extra_data = s->read_discard();

    if(extra_data)
     throw MDFN_Error(0, _("ROM image file %s is %llu bytes larger than the required size of %u bytes."), gf->vfs->get_human_path(fpath).c_str(), (unsigned long long)extra_data, rle->size);
   }
  }

  if(sgi->romtwiddle == STV_ROMTWIDDLE_SANJEON)
  {
   for(uint32 i = 0; i < 0x3000000 / sizeof(uint64); i++)
   {
    uint64 tmp = MDFN_densb<uint64>((uint8*)ROM + (i << 3));

    tmp = ~tmp;
    tmp = ((tmp & 0x0404040404040404ULL) >> 2) | ((tmp & 0x0101010101010101ULL) << 6) | (tmp & 0x2020202020202020ULL) | ((tmp & 0x1010101010101010ULL) >> 3) | ((tmp & 0x4040404040404040ULL) << 1) | ((tmp & 0x0808080808080808ULL) >> 1) | ((tmp & 0x8080808080808080ULL) >> 3) | ((tmp & 0x0202020202020202ULL) << 2);

    MDFN_ennsb<uint64>((uint8*)ROM + (i << 3), tmp);
   }
  }

  {
   sha256_digest dig = h.digest();

   memcpy(MDFNGameInfo->MD5, dig.data(), 16);
  }

  SS_SetPhysMemMap (0x02000000, 0x04FFFFFF, ROM, 0x3000000, false);
  c->CS01_SetRW8W16(0x02000000, 0x04FFFFFF, ROM_Read, Write<uint8>, Write<uint16>);

  /* Protection chips are in A-bus CS2 (0x05800000-0x058FFFFF).
   * CS2M offset = (A >> 1) & 0x1F, A = 0x05800000 + offset*2.
   * Register map (both 5838 and 5881 use CS2):
   *   0x05800000 (offset 0x00): 5838 srcaddr / 5881 addrlo
   *   0x05800002 (offset 0x02): 5838 (unused) / 5881 addrhi
   *   0x05800004 (offset 0x04): 5838 data_r / 5881 subkey
   *   0x05800006 (offset 0x06): 5838 table_mode / 5881 data_r
   * These are discovered via diagnostic; correct if needed.              */
  /* CS2 (A-bus CS2, 0x05800000-0x058FFFFF) — diagnostic handlers.
   * Uses static wrapper functions defined below to avoid PIC relocation
   * issues when storing function pointers from the static lib.            */
   c->CS2M_SetRW8W16(0x00, 0x3F, CS2_Read_diag, CS2_Write8_diag, CS2_Write16_diag);

  /* 315-5838: connect chip to full cart ROM space, matching MAME protbank layout:
   *   bank 0 = ROM[0x0000000..0x7FFFFF]  (epr area + mpr18968.2)
   *   bank 1 = ROM[0x0800000..0xFFFFFF]  (mpr18969.3 + mpr18970.4)
   *   bank 2 = ROM[0x1000000..0x17FFFFF] (mpr18971.5 + mpr18972.6)
   * srcoffset is a 23-bit index into the 8MB bank window. */
  if(ECChip == STV_EC_CHIP_315_5838)
  {
   chip5838.reset();
   chip5838.rom            = ROM;                        /* bank 0 default */
   chip5838.rom_base       = ROM;
   chip5838.rom_phys       = ROM;                        /* bank N = ROM + N*0x400000 words */
   chip5838.rom_size_words = 0x3000000 / sizeof(uint16); /* full ROM array */
   chip5838.pending_srcaddr_hi = 0;
   MDFN_printf(_("[CART-STV] 315-5838 decipher chip enabled (Decathlete)\n"));
  }

  /* 315-5881: initialize with per-game key and connect to cart ROM */
  if(ECChip == STV_EC_CHIP_315_5881)
  {
   chip5881.reset();
   chip5881.rom            = ROM;
   chip5881.rom_size_words = 0x3000000 / sizeof(uint16);
   /* Pre-load game key. The game will also write subkey via register.
    * game_key is fixed per-game; subkey comes from the game at runtime. */
   chip5881.game_key = sgi->ec_key;
   MDFN_printf(_("[CART-STV] 315-5881 encryption chip enabled (key=0x%08X)\n"), sgi->ec_key);
  }

  c->StateAction = StateAction;
  c->Reset = Reset;
  c->Kill = Kill;
 }
 catch(...)
 {
  Kill();
  throw;
 }
}

} // namespace MDFN_IEN_SS
