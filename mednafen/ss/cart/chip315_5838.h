/* chip315_5838.h — Sega 315-5838 Compression and Encryption chip
 *
 * Ported from MAME: src/devices/machine/315-5838_317-0229_comp.cpp
 * Original authors: David Haywood, Samuel Neves, Peter Wilhelmsen,
 *                   Morten Shearman Kirkegaard
 * License: BSD-3-Clause
 *
 * Used by: Decathlete (ST-V) — full compression + encryption
 *
 * Algorithm:
 *   ROM word → decipher() → bit-by-bit → Huffman tree lookup →
 *   dictionary[j] → output byte
 *
 * The Huffman tree (12 entries) and dictionary (256 bytes) are uploaded
 * by the CPU via register writes before each decompression block.
 *
 * Hardware access (CS01, last 16 bytes of each 8MB ROM bank):
 *   Write (A&0x7FFFFF)==0x7FFFF0/2 → srcaddr_w (hi/lo 16-bit word)
 *   Write (A&0x7FFFFF)==0x7FFFF4   → set_table_upload_mode_w
 *   Write (A&0x7FFFFF)==0x7FFFF6   → upload_table_data_w
 *   Read  (A&0x7FFFFF)>=0x7FFFF8   → data_r()
 *
 * Validated: decipher(0x1533) = 0x16be (confirmed on real hardware)
 */

#ifndef __MDFN_SS_CART_CHIP315_5838_H
#define __MDFN_SS_CART_CHIP315_5838_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Define to enable debug trace — redirect to stderr.
 * Dumps: decipher self-test, srcaddr writes, first 32 source words,
 * first 32 decipher outputs, and first 64 decompressed bytes.        */
//#define CHIP5838_DEBUG 1

namespace MDFN_IEN_SS
{

/* ── Huffman tree entry ─────────────────────────────────────────────────── */
struct TreeEntry5838
{
 uint8_t  len;      /* bit length of this code */
 uint8_t  idx;      /* base index into dictionary */
 uint16_t pattern;  /* 12-bit base pattern (left-justified in 16 bits) */
};

/* ── Compression state (uploaded per block by CPU) ──────────────────────── */
struct Chip5838CompState
{
 /* 12 real entries + 1 zero sentinel.
  * When i==11 in the Huffman search, tree[12] is accessed as upper-bound
  * guard.  With sentinel.pattern=0 the check (val >= 0) always fires,
  * meaning entry 11 only matches when num_bits==12 (upper-bound check
  * skipped).  This matches MAME's sega_315_5838_comp_device tree[13]. */
 TreeEntry5838 tree[13];
 uint8_t       dictionary[256];
 uint16_t      mode;   /* upload mode: full 16-bit value; bit 7 selects tree(0)/dict(1) */
 uint16_t      id;     /* dictionary write index */
 uint8_t       it2;    /* tree write index (half-entries: 2 per tree entry) */
};

/* ── Chip state ─────────────────────────────────────────────────────────── */
struct Chip5838
{
 /* Single compression state (matches MAME): one Huffman tree and dictionary
  * for the entire chip.  The bank only selects which ROM region to read
  * compressed data from; the tree/dictionary are shared across all banks. */
 Chip5838CompState cs;
 int active_bank;           /* ROM bank currently selected (0-2) */

 uint32_t srcoffset;
 uint32_t srcstart;
 bool     abort;

 /* Decipher output bit-stream */
 uint16_t val_compressed;      /* current deciphered word */
 int      num_bits_compressed; /* remaining bits in val_compressed */

 /* Huffman accumulator */
 uint16_t val;      /* accumulated bits for current code */
 int      num_bits; /* number of bits accumulated */

 /* ROM pointers */
 const uint16_t *rom;       /* current bank pointer (for source_word_r) */
 const uint16_t *rom_base;  /* MPR0 base — used by CS2 BIOS check */
 const uint16_t *rom_phys;  /* ROM byte-0 base — used by CS01 bank select */
 uint32_t        rom_size_words;

 bool     active;              /* true once srcaddr written */
 uint32_t pending_srcaddr_hi;  /* upper word of 32-bit srcaddr write */

 /* Set ROM bank for CS01 access (physical Saturn bank mapping).
  * Bank N = ROM byte (N * 0x800000) from rom_phys.                      */
 void set_bank(uint32_t bank)
 {
  if(rom_phys && bank < 4)
  {
   rom = rom_phys + (size_t)bank * 0x400000;
   active_bank = (int)bank;
  }
 }

 void reset()
 {
  srcoffset           = 0;
  srcstart            = 0;
  abort               = false;
  active              = false;
  val_compressed      = 0;
  num_bits_compressed = 0;
  val                 = 0;
  num_bits            = 0;
  pending_srcaddr_hi  = 0;
  active_bank         = 0;
  memset(&cs, 0, sizeof(cs));
#ifdef CHIP5838_DEBUG
  dbg_src_count  = 0;
  dbg_dec_count  = 0;
  dbg_byte_count = 0;
#endif
 }

#ifdef CHIP5838_DEBUG
 int dbg_src_count;
 int dbg_dec_count;
 int dbg_byte_count;
#endif

 /* ── decipher: 16-bit nonlinear permutation (Decathlete-specific) ─── */
 uint16_t decipher(uint16_t c)
 {
  uint16_t p = 0;
  uint16_t x[16];
  for(int b = 0; b < 16; b++)
   x[b] = (c >> b) & 1;

  p |= (x[7]^x[9]^x[14] ? 0 : x[5]^x[12]) ^ x[14];
  p |= (((x[7]^x[9])&(x[12]^x[14]^x[5])) ^ x[14] ^ 1) << 1;
  p |= ((x[6]&x[8]) ^ (x[6]&x[15]) ^ (x[8]&x[15]) ^ 1) << 2;
  p |= (x[11]^x[14]^1) << 3;
  p |= ((x[7]&(x[1]^x[8]^x[12])) ^ x[12]) << 4;
  p |= ((x[6]|x[8]) ^ (x[8]&x[15])) << 5;
  p |= (x[4] ^ (x[3]|x[10])) << 6;
  p |= ((x[14]&(x[5]^x[12])) ^ x[7] ^ x[9] ^ 1) << 7;
  p |= (x[4]^x[13]^1) << 8;
  p |= (x[6] ^ (x[8]|(x[15]^1))) << 9;
  p |= (x[7] ^ (x[12]|(x[1]^x[8]^x[7]^1))) << 10;
  p |= (x[3]^x[10]^1) << 11;
  p |= (x[0]^x[2]) << 12;
  p |= (x[8]^x[1] ? x[12] : x[7]) << 13;
  p |= (x[0]^x[11]^x[14]^1) << 14;
  p |= (x[10]^1) << 15;
  return p;
 }

 /* ── ROM word reader (XOR-2 addressing, auto-increment srcoffset) ────── */
 uint16_t source_word_r()
 {
  const uint32_t byte_addr = (srcoffset * 2) ^ 2;
  uint16_t word = 0xFFFF;

  if(byte_addr + 1 < rom_size_words * 2)
   word = *(const uint16_t*)((const uint8_t*)rom + byte_addr);

  /* MAME's device_rom_interface defaults to ENDIANNESS_LITTLE, so its
   * read_word() is also a LE read.  However MAME's cart is ROM_REGION32_BE:
   *   ROM_LOAD16_WORD_SWAP stores (MSB, LSB) → LE read = (LSB<<8)|MSB
   *   ROM_RELOAD_PLAIN    stores (f0,  f1 ) → LE read = (f1<<8)|f0
   * libretro stores the same data with opposite byte layout in the ROM array
   * (STV_MAP_16LE for WORD_SWAP, STV_MAP_16BE for RELOAD_PLAIN), so both
   * raw LE reads come out byte-swapped vs MAME.  Swap here to compensate. */
#ifdef CHIP5838_DEBUG
  const uint16_t raw = word;
#endif
  word = (uint16_t)((word >> 8) | (word << 8));

  srcoffset = (srcoffset + 1) & 0x007FFFFF;
  if(srcoffset == srcstart)
   abort = true;

#ifdef CHIP5838_DEBUG
  if(dbg_src_count < 32)
  {
   const uint32_t abs_byte = (uint32_t)(((const uint8_t*)rom - (const uint8_t*)rom_phys) + byte_addr);
   fprintf(stderr, "[5838] src[%2d] bank=%d srcoff=0x%06X byte_addr=0x%06X raw=0x%04X bswap=0x%04X\n",
    dbg_src_count, active_bank, (unsigned)(srcoffset ? srcoffset-1 : 0x7FFFFF),
    (unsigned)byte_addr, (unsigned)raw, (unsigned)word);
   dbg_src_count++;
   if(dbg_src_count == 32)
    fprintf(stderr, "[5838] (source_word_r trace complete)\n");
  }
#endif

  return word;
 }

 /* ── Full decompressor: decipher → Huffman → dictionary ────────────── */
 uint8_t get_decompressed_byte()
 {
  for(;;)
  {
   if(abort)
    return 0xFF;

   if(num_bits_compressed == 0)
   {
    uint16_t raw_src = source_word_r();
    val_compressed      = decipher(raw_src);
    num_bits_compressed = 16;
#ifdef CHIP5838_DEBUG
    if(dbg_dec_count < 32)
    {
     fprintf(stderr, "[5838] dec[%2d] raw=0x%04X decipher=0x%04X\n",
      dbg_dec_count, (unsigned)raw_src, (unsigned)val_compressed);
     dbg_dec_count++;
    }
#endif
   }

   /* Extract one bit from the deciphered stream (MSB first) */
   num_bits_compressed--;
   val <<= 1;
   val |= 1 & (val_compressed >> num_bits_compressed);
   num_bits++;

   /* Search Huffman tree for a matching code */
   for(int i = 0; i < 12; i++)
   {
    if(num_bits != cs.tree[i].len) continue;
    if(val < (cs.tree[i].pattern >> (12 - num_bits))) continue;
    if((num_bits < 12) &&
       (val >= (cs.tree[i+1].pattern >> (12 - num_bits)))) continue;

    int j = cs.tree[i].idx + val - (cs.tree[i].pattern >> (12 - num_bits));

    val      = 0;
    num_bits = 0;

#ifdef CHIP5838_DEBUG
    if(dbg_byte_count < 64)
    {
     fprintf(stderr, "[5838] byte[%2d] dict[%d]=0x%02X\n",
      dbg_byte_count, j, (unsigned)cs.dictionary[j]);
     dbg_byte_count++;
     if(dbg_byte_count == 64)
      fprintf(stderr, "[5838] (decompressed byte trace complete)\n");
    }
#endif

    return cs.dictionary[j];
   }
  }
 }

 /* ── Public register interface ──────────────────────────────────────── */

 /* Raw decipher output (no Huffman): used for BIOS/protection check via CS2.
  * Returns decipher(ROM_word) directly as a 16-bit value.
  * In hardware, the CS2 read path bypasses the Huffman decoder.           */
 uint16_t data_r_raw()
 {
  return decipher(source_word_r());
 }

 /* Huffman decompressor output: used for sprite decompression via CS01.   */
 uint16_t data_r()
 {
  uint8_t hi = get_decompressed_byte();
  uint8_t lo = get_decompressed_byte();
  return ((uint16_t)hi << 8) | lo;
 }

 /* Set source ROM word address (23-bit) and activate chip.
  * Accepts uint32_t so the full 23-bit value from CS01 is preserved.     */
 void srcaddr_w16(uint32_t v)
 {
  srcoffset           = (uint32_t)v & 0x007FFFFF;
  srcstart            = srcoffset;
  abort               = false;
  active              = true;
  val_compressed      = 0;
  num_bits_compressed = 0;
  val                 = 0;
  num_bits            = 0;
#ifdef CHIP5838_DEBUG
  dbg_src_count  = 0;
  dbg_dec_count  = 0;
  dbg_byte_count = 0;
  /* One-time decipher self-test */
  static bool selftest_done = false;
  if(!selftest_done) {
   selftest_done = true;
   uint16_t tv = decipher(0x1533);
   fprintf(stderr, "[5838] SELFTEST: decipher(0x1533)=0x%04X  expected=0x16BE  %s\n",
    (unsigned)tv, tv == 0x16BE ? "PASS" : "FAIL !!!");
  }
  fprintf(stderr, "[5838] srcaddr_w16: bank=%d srcoffset=0x%06X\n",
   active_bank, (unsigned)srcoffset);
  /* Dump full tree state so we can verify against MAME */
  for(int _i = 0; _i < 12; _i++)
   fprintf(stderr, "[5838]   tree[%2d] len=%2u idx=%3u pattern=0x%04X\n",
    _i, cs.tree[_i].len, cs.tree[_i].idx, cs.tree[_i].pattern);
#endif
 }

 /* Set table upload mode.
  * Stores full 16-bit value (matching MAME); only bit 7 is used for the
  * tree(0) / dictionary(1) selection.  Resets the appropriate write index. */
 void set_table_upload_mode_w(uint16_t v)
 {
  cs.mode = v;
  if(!(cs.mode & 0x80))
   cs.it2 = 0;
  else
   cs.id = 0;
#ifdef CHIP5838_DEBUG
  fprintf(stderr, "[5838] upload_mode: 0x%04X → %s (reset index)\n",
   v, (v & 0x80) ? "DICT" : "TREE");
#endif
 }

 /* Upload one 16-bit value into tree or dictionary */
 void upload_table_data_w(uint16_t v)
 {
  if(!(cs.mode & 0x80))
  {
   /* Tree upload: alternating (len/idx) and (pattern) entries.
    * Writes to entries 0-11 only; entry 12 is the zero sentinel. */
   if(cs.it2 / 2 >= 12) return;
   if((cs.it2 & 1) == 0)
   {
    cs.tree[cs.it2/2].len     = (v & 0xFF00) >> 8;
    cs.tree[cs.it2/2].idx     = (v & 0x00FF);
#ifdef CHIP5838_DEBUG
    fprintf(stderr, "[5838] tree[%2d] len=0x%02X idx=0x%02X\n",
     cs.it2/2, cs.tree[cs.it2/2].len, cs.tree[cs.it2/2].idx);
#endif
   }
   else
   {
    cs.tree[cs.it2/2].pattern = v;
#ifdef CHIP5838_DEBUG
    fprintf(stderr, "[5838] tree[%2d] pattern=0x%04X\n", cs.it2/2, v);
#endif
   }
   cs.it2++;
  }
  else
  {
   /* Dictionary upload: two bytes per 16-bit write.
    * Guard: id must be ≤ 254 so both bytes (id, id+1) fit in [0..255]. */
   if(cs.id >= 255) return;
#ifdef CHIP5838_DEBUG
   if(cs.id < 32 || cs.id >= 250)
    fprintf(stderr, "[5838] dict[%3d]=0x%02X dict[%3d]=0x%02X\n",
     cs.id, (v>>8)&0xFF, cs.id+1, v&0xFF);
   else if(cs.id == 32)
    fprintf(stderr, "[5838] dict[...] (skipping middle entries)\n");
#endif
   cs.dictionary[cs.id++] = (v & 0xFF00) >> 8;
   cs.dictionary[cs.id++] = (v & 0x00FF);
  }
 }
};

} /* namespace MDFN_IEN_SS */

#endif /* __MDFN_SS_CART_CHIP315_5838_H */
