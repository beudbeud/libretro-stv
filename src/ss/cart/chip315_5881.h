/* chip315_5881.h — Sega 315-5881 Encryption chip
 *
 * Ported from MAME: src/devices/machine/315-5881_crypt.cpp
 * Original authors: Andreas Naive, Olivier Galibert, David Haywood
 * License: BSD-3-Clause
 *
 * Two 4-round Feistel Networks operating in counter mode.
 * Optional LZ-style decompression of the decrypted stream.
 *
 * ST-V register map (iomap_le offsets from chip base 0x04000000):
 *   +0x08 write → addrlo_w  (source address low 16 bits)
 *   +0x0a write → addrhi_w  (source address high 16 bits, always 0 on ST-V)
 *   +0x0c write → subkey_w  (16-bit sequence key)
 *   +0x0e read  → decrypt_r (next decrypted 16-bit word, LE byte-swapped)
 */

#ifndef __MDFN_SS_CART_CHIP315_5881_H
#define __MDFN_SS_CART_CHIP315_5881_H

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <algorithm>

namespace MDFN_IEN_SS
{

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline int BIT(uint32_t v, int n) { return (v >> n) & 1; }

/* bitswap<16>: rearrange bits of val. The bit indices listed go to
 * positions 15 downto 0 (MSB first) in the output.                         */
template<int N>
static uint16_t bitswap(uint16_t val, int b15,int b14,int b13,int b12,
                         int b11,int b10,int b9, int b8,
                         int b7, int b6, int b5, int b4,
                         int b3, int b2, int b1, int b0)
{
 return (BIT(val,b15)<<15)|(BIT(val,b14)<<14)|(BIT(val,b13)<<13)|(BIT(val,b12)<<12)|
        (BIT(val,b11)<<11)|(BIT(val,b10)<<10)|(BIT(val,b9) <<9) |(BIT(val,b8) <<8) |
        (BIT(val,b7) <<7) |(BIT(val,b6) <<6) |(BIT(val,b5) <<5) |(BIT(val,b4) <<4) |
        (BIT(val,b3) <<3) |(BIT(val,b2) <<2) |(BIT(val,b1) <<1) |(BIT(val,b0) <<0);
}

/* ── S-Box / Feistel types (MAME-identical layout) ──────────────────────── */
struct sbox5881 {
 uint8_t table[64];
 int     inputs[6];   /* -1 = unused */
 int     outputs[2];
};

/* ── Compression tree node format:
 *  0xxxxxxx → next node index
 *  1a0bbccc → end node (a=fetch/repeat, bb=offset, ccc+1=count)
 *  11111111 → empty (padding)                                               */
/* MAME uses BUFFER_SIZE=2 (stream-like, one word at a time).
 * A larger buffer can mix data from two consecutive blocks when block_size < BUFFER_SIZE,
 * because enc_start() resets block_pos mid-loop without stopping the fill.    */
static constexpr int BUFFER_SIZE = 2;
static constexpr int LINE_SIZE   = 512;
static constexpr uint32_t FLAG_COMPRESSED = 0x20000;
static constexpr int FN1GK = 38;
static constexpr int FN2GK = 32;

struct Chip5881
{
 /* ── Feistel S-box tables (from MAME, verbatim) ──────────────────────── */
 static const sbox5881 fn1_sboxes[4][4];
 static const sbox5881 fn2_sboxes[4][4];
 static const int fn1_game_key_scheduling[FN1GK][2];
 static const int fn2_game_key_scheduling[FN2GK][2];
 static const int fn1_sequence_key_scheduling[20][2];
 static const int fn2_sequence_key_scheduling[16];
 static const int fn2_middle_result_scheduling[16];
 static const uint8_t trees[9][2][32];

 /* ── Chip state ──────────────────────────────────────────────────────── */
 const uint16_t *rom;
 uint32_t        rom_size_words;

 uint32_t prot_cur_address;
 uint16_t subkey;
 uint32_t game_key;
 uint32_t protenable;  /* bit 0x00010000 = chip enabled (MAME m_abus_protenable) */
 uint16_t a_bus[8];    /* mirror of last-written words at offsets 0x04FFFFF0-FE (MAME m_a_bus) */

 bool     enc_ready;

 uint16_t dec_hist;
 uint32_t dec_header;
 int      block_pos;
 int      block_numlines;
 int      block_size;
 int      done_compression;

 uint8_t  buffer[BUFFER_SIZE];
 int      buffer_pos;
 int      buffer_bit;

 uint16_t buffer2a;
 uint8_t  buffer2[2];
 int      buffer_bit2;

 uint8_t  line_buffer[LINE_SIZE];
 uint8_t  line_buffer_prev[LINE_SIZE];
 int      line_buffer_pos;
 int      line_buffer_size;

 void reset()
 {
  memset(buffer,           0, sizeof(buffer));
  memset(line_buffer,      0, sizeof(line_buffer));
  memset(line_buffer_prev, 0, sizeof(line_buffer_prev));
  memset(a_bus, 0, sizeof(a_bus));
  prot_cur_address  = 0;
  subkey            = 0;
  protenable        = 0;
  dec_hist          = 0;
  dec_header        = 0;
  enc_ready         = false;
  buffer_pos        = 0;
  line_buffer_pos   = 0;
  line_buffer_size  = 0;
  buffer_bit        = 0;
  buffer_bit2       = 15;
  block_pos         = 0;
  block_numlines    = 0;
  block_size        = 0;
  done_compression  = 0;
 }

 /* ── Core block cipher ───────────────────────────────────────────────── */
 int feistel_function(int input, const sbox5881 *sboxes, uint32_t subkeys)
 {
  int result = 0;
  for(int m = 0; m < 4; m++) {
   int aux = 0;
   for(int k = 0; k < 6; k++)
    if(sboxes[m].inputs[k] != -1)
     aux |= BIT(input, sboxes[m].inputs[k]) << k;
   aux = sboxes[m].table[(aux ^ subkeys) & 0x3f];
   for(int k = 0; k < 2; k++)
    result |= BIT(aux, k) << sboxes[m].outputs[k];
   subkeys >>= 6;
  }
  return result;
 }

 uint16_t block_decrypt(uint32_t gkey, uint16_t seq_key, uint16_t counter, uint16_t data)
 {
  int j, aux, aux2;
  uint32_t fn1_subkeys[4] = {};
  uint32_t fn2_subkeys[4] = {};

  /* Game-key scheduling */
  for(j = 0; j < FN1GK; j++)
   if(BIT(gkey, fn1_game_key_scheduling[j][0])) {
    aux  = fn1_game_key_scheduling[j][1] % 24;
    aux2 = fn1_game_key_scheduling[j][1] / 24;
    fn1_subkeys[aux2] ^= (1 << aux);
   }
  for(j = 0; j < FN2GK; j++)
   if(BIT(gkey, fn2_game_key_scheduling[j][0])) {
    aux  = fn2_game_key_scheduling[j][1] % 24;
    aux2 = fn2_game_key_scheduling[j][1] / 24;
    fn2_subkeys[aux2] ^= (1 << aux);
   }

  /* Sequence-key scheduling */
  for(j = 0; j < 20; j++)
   if(BIT(seq_key, fn1_sequence_key_scheduling[j][0])) {
    aux  = fn1_sequence_key_scheduling[j][1] % 24;
    aux2 = fn1_sequence_key_scheduling[j][1] / 24;
    fn1_subkeys[aux2] ^= (1 << aux);
   }
  for(j = 0; j < 16; j++)
   if(BIT(seq_key, j)) {
    aux  = fn2_sequence_key_scheduling[j] % 24;
    aux2 = fn2_sequence_key_scheduling[j] / 24;
    fn2_subkeys[aux2] ^= (1 << aux);
   }

  /* First Feistel Network (counter → middle_result) */
  int A, B;
  aux = bitswap<16>(counter, 5,12,14,13,9,3,6,4,8,1,15,11,0,7,10,2);
  B = aux >> 8;
  A = (aux & 0xff) ^ feistel_function(B, fn1_sboxes[0], fn1_subkeys[0]);
  B ^= feistel_function(A, fn1_sboxes[1], fn1_subkeys[1]);
  A ^= feistel_function(B, fn1_sboxes[2], fn1_subkeys[2]);
  B ^= feistel_function(A, fn1_sboxes[3], fn1_subkeys[3]);
  int middle_result = (B << 8) | A;

  /* Middle-result scheduling */
  for(j = 0; j < 16; j++)
   if(BIT(middle_result, j)) {
    aux  = fn2_middle_result_scheduling[j] % 24;
    aux2 = fn2_middle_result_scheduling[j] / 24;
    fn2_subkeys[aux2] ^= (1 << aux);
   }

  /* Second Feistel Network (data → plaintext) */
  aux = bitswap<16>(data, 14,3,8,12,13,7,15,4,6,2,9,5,11,0,1,10);
  B = aux >> 8;
  A = (aux & 0xff) ^ feistel_function(B, fn2_sboxes[0], fn2_subkeys[0]);
  B ^= feistel_function(A, fn2_sboxes[1], fn2_subkeys[1]);
  A ^= feistel_function(B, fn2_sboxes[2], fn2_subkeys[2]);
  B ^= feistel_function(A, fn2_sboxes[3], fn2_subkeys[3]);
  aux = (B << 8) | A;
  return bitswap<16>((uint16_t)aux, 15,7,6,14,13,12,5,4,3,2,11,10,9,1,0,8);
 }

 /* ── ROM access ──────────────────────────────────────────────────────── */
 uint16_t rom_read(uint32_t word_addr)
 {
  if(word_addr >= rom_size_words) return 0xFFFF;
  /* Kronos m_read (decrypt.c):
   *   dat = DMAMappedMemoryReadWord(0x02000000 + 2*addr)
   *       = T1ReadWord(rom_T2, addr*2)        [T1ReadWord on LE = bswap16(*(u16*))]
   *       = bswap16(rom_T2[addr])             [rom_T2 loaded via T2WriteByte = XOR-1]
   *       = bswap16(bswap16(file_word))       = file_word_BE
   *   enc = bswap16(dat)                      = bswap16(file_word_BE) = file_word_LE
   * Wait — net result enc = bswap16(T1ReadWord(T2_rom)) :
   *   T2WriteByte stores [A,B] as [B,A] in mem
   *   T1ReadWord on LE: bswap16(*(u16*)) = bswap16(B|(A<<8)) = A|(B<<8) = file_BE
   *   m_read bswap16(file_BE) = file_LE? No...
   *   Let file=[0x12,0x34]: T2→mem=[0x34,0x12], T1Read=bswap16(0x1234)=0x3412,
   *   m_read bswap=bswap16(0x3412)=0x1234 = (0x12<<8)|0x34 = BE interpretation
   * Mednafen: STV_MAP_16LE→mem=[0x12,0x34], rom[0]=0x3412 (LE)
   *   bswap16(0x3412) = 0x1234 ← matches Kronos enc!                          */
  return __builtin_bswap16(rom[word_addr]);
 }

 uint16_t get_decrypted_16()
 {
  uint16_t enc = rom_read(prot_cur_address);
  uint16_t dec = block_decrypt(game_key, subkey, (uint16_t)prot_cur_address, enc);
  uint16_t res = (dec & 3) | (dec_hist & 0xfffc);
  dec_hist = dec;
  prot_cur_address++;
  return res;
 }

 /* ── Decompressor ────────────────────────────────────────────────────── */
 void enc_start()
 {
  block_pos        = 0;
  done_compression = 0;
  buffer_pos       = BUFFER_SIZE;

  if(buffer_bit2 < 14)
  {
   /* Reusing leftover bits from the current decompression word — do NOT
    * reset dec_hist: the history from the previous get_decrypted_16() call
    * must be preserved for the lower-word read that follows. */
   dec_header = (uint32_t)(buffer2a & 0x0003) << 16;
  }
  else
  {
   /* Starting a fresh stream: reset history so the previous block cannot
    * bleed into this one.  MAME resets only here (not unconditionally).
    * Required by Astra SuperStars (MAME comment: "seems to be needed by
    * astrass at least otherwise any call after the first one will be
    * influenced by the one before it"). */
   dec_hist = 0;
   dec_header = (uint32_t)get_decrypted_16() << 16;
  }
  dec_header |= get_decrypted_16();

  block_numlines = ((dec_header & 0x000000ff) >> 0) + 1;
  int blocky     = ((dec_header & 0x0001ff00) >> 8) + 1;
  block_size     = block_numlines * blocky;

  if(dec_header & FLAG_COMPRESSED) {
   line_buffer_size = blocky;
   line_buffer_pos  = line_buffer_size;
   buffer_bit       = 7;
   buffer_bit2      = 15;
  }
  enc_ready = true;
 }

 void enc_fill()
 {
  assert(buffer_pos == BUFFER_SIZE);
  for(int i = 0; i < BUFFER_SIZE; i += 2) {
   uint16_t val  = get_decrypted_16();
   buffer[i]     = (uint8_t)val;
   buffer[i + 1] = (uint8_t)(val >> 8);
   block_pos    += 2;
   if(!(dec_header & FLAG_COMPRESSED) && block_pos == block_size)
    enc_start();
  }
  buffer_pos = 0;
 }

 int get_compressed_bit()
 {
  if(buffer_bit2 == 15) {
   buffer_bit2 = 0;
   buffer2a    = get_decrypted_16();
   buffer2[0]  = (uint8_t)buffer2a;
   buffer2[1]  = (uint8_t)(buffer2a >> 8);
   buffer_pos  = 0;
  } else {
   buffer_bit2++;
  }
  int res = (buffer2[(buffer_pos & 1) ^ 1] >> buffer_bit) & 1;
  buffer_bit--;
  if(buffer_bit == -1) {
   buffer_bit = 7;
   buffer_pos++;
  }
  return res;
 }

 void line_fill()
 {
  assert(line_buffer_pos == line_buffer_size);

  /* lp = current line (source for copy-from-prev-line operations).
   * lc = scratch target for the new line being built.
   * After filling lc, swap the arrays so line_buffer holds the new data
   * and line_buffer_prev holds the old data for the next fill.
   *
   * MAME uses unique_ptr::swap() to swap pointer ownership; here we use
   * plain arrays, so we fill into line_buffer_prev then memcpy-swap. */
  uint8_t *lp = line_buffer;
  uint8_t *lc = line_buffer_prev;
  line_buffer_pos = 0;

  for(int i = 0; i < line_buffer_size; ) {
   int slot = i ? (i < line_buffer_size - 7 ? 1 : (i & 7) + 1) : 0;
   uint32_t tmp = 0;
   while(!(tmp & 0x80))
    tmp = get_compressed_bit() ? trees[slot][1][tmp] : trees[slot][0][tmp];

   if(tmp != 0xff) {
    int count = (tmp & 7) + 1;
    if(tmp & 0x40) {
     static const int offsets[4] = {0, 1, 0, -1};
     int offset = offsets[(tmp & 0x18) >> 3];
     for(int j = 0; j < count; j++) {
      lc[i ^ 1] = lp[((i + offset) % line_buffer_size) ^ 1];
      i++;
     }
    } else {
     uint8_t byte = 0;
     for(int k = 0; k < 8; k++)
      byte = (byte << 1) | get_compressed_bit();
     for(int j = 0; j < count; j++)
      lc[(i++) ^ 1] = byte;
    }
   }
  }
  /* Swap arrays so line_buffer contains the newly filled data.
   * lc (= old line_buffer_prev) holds the new line; copy it into
   * line_buffer and save the old line into line_buffer_prev. */
  {
   uint8_t tmp_buf[LINE_SIZE];
   memcpy(tmp_buf,          line_buffer,      LINE_SIZE);
   memcpy(line_buffer,      line_buffer_prev, LINE_SIZE);
   memcpy(line_buffer_prev, tmp_buf,          LINE_SIZE);
  }

  block_pos++;
  if(block_numlines == block_pos)
   done_compression = 1;
 }

 /* ── Public register interface ───────────────────────────────────────── */
 void set_addr_low(uint16_t data)
 {
  prot_cur_address = (prot_cur_address & 0xffff0000) | data;
  enc_ready = false;
 }

 void set_addr_high(uint16_t data)
 {
  prot_cur_address = (prot_cur_address & 0x0000ffff) | ((uint32_t)data << 16);
  enc_ready  = false;
  buffer_bit  = 7;
  buffer_bit2 = 15;
 }

 void set_subkey(uint16_t data)
 {
  subkey    = data;
  enc_ready = false;
 }

 uint16_t decrypt_be_r()
 {
  if(!enc_ready) enc_start();

  uint8_t *base;
  uint16_t retval;

  if(dec_header & FLAG_COMPRESSED) {
   if(line_buffer_pos == line_buffer_size) {
    if(done_compression == 1) enc_start();
    line_fill();
   }
   base = line_buffer + line_buffer_pos;
   line_buffer_pos += 2;
  } else {
   if(buffer_pos == BUFFER_SIZE) enc_fill();
   base = buffer + buffer_pos;
   buffer_pos += 2;
  }
  retval = ((uint16_t)base[0] << 8) | base[1];
  return retval;
 }

 /* LE variant: byte-swap result (SH-2 is big-endian, but ST-V bus is different) */
 uint16_t decrypt_le_r()
 {
  uint16_t v = decrypt_be_r();
  return ((v & 0xff00) >> 8) | ((v & 0x00ff) << 8);
 }
};

/* ── Static table definitions ────────────────────────────────────────────── */
/* All values verbatim from MAME 315-5881_crypt.cpp                           */

const sbox5881 Chip5881::fn1_sboxes[4][4] = {
 {  /* 1st round */
  {{0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3},{3,4,5,7,-1,-1},{0,4}},
  {{2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2},{0,1,2,5,6,7},{1,6}},
  {{0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3},{0,2,5,6,7,-1},{2,3}},
  {{3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3},{1,2,3,4,6,7},{5,7}},
 },
 {  /* 2nd round */
  {{3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3},{0,1,3,4,5,7},{0,4}},
  {{2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1},{0,1,3,4,6,-1},{1,5}},
  {{2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2},{1,2,5,6,7,6},{2,7}},
  {{0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0},{2,3,4,5,6,7},{3,6}},
 },
 {  /* 3rd round */
  {{0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1},{1,2,3,4,5,7},{0,5}},
  {{1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1},{0,2,4,5,6,7},{1,6}},
  {{0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255},{0,2,4,6,7,-1},{2,3}},
  {{0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3},{0,1,2,3,6,-1},{4,7}},
 },
 {  /* 4th round */
  {{0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1},{0,1,3,4,5,6},{0,5}},
  {{0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3},{0,1,2,3,5,7},{1,7}},
  {{0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0},{2,3,4,5,6,7},{2,3}},
  {{0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1},{0,1,2,4,6,7},{4,6}},
 },
};

const sbox5881 Chip5881::fn2_sboxes[4][4] = {
 {  /* 1st round */
  {{3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2},{1,3,4,5,6,7},{0,7}},
  {{0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0},{0,3,5,6,5,0},{1,2}},
  {{0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2},{0,2,3,4,7,-1},{3,4}},
  {{2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1},{1,2,5,6,-1,-1},{5,6}},
 },
 {  /* 2nd round */
  {{2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0},{0,1,4,5,6,7},{0,7}},
  {{0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3},{0,2,3,6,7,-1},{1,5}},
  {{1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3},{2,3,4,6,7,2},{2,3}},
  {{2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0},{1,2,3,4,5,6},{4,6}},
 },
 {  /* 3rd round */
  {{0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0},{1,4,5,6,7,5},{0,5}},
  {{0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2},{0,2,3,4,6,7},{1,7}},
  {{2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255},{1,2,4,7,2,-1},{2,4}},
  {{0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3},{0,1,2,3,5,7},{3,6}},
 },
 {  /* 4th round */
  {{0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1},{0,1,2,4,7,-1},{0,5}},
  {{2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1},{0,1,3,5,6,-1},{1,3}},
  {{0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1},{0,2,3,4,5,7},{2,7}},
  {{0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2},{2,3,4,5,6,7},{4,6}},
 },
};

const int Chip5881::fn1_game_key_scheduling[FN1GK][2] = {
 {1,29},{1,71},{2,4},{2,54},{3,8},{4,56},{4,73},{5,11},
 {6,51},{7,92},{8,89},{9,9},{9,39},{9,58},{10,90},{11,6},
 {12,64},{13,49},{14,44},{15,40},{16,69},{17,15},{18,23},{18,43},
 {19,82},{20,81},{21,32},{22,5},{23,66},{24,13},{24,45},{25,12},
 {25,35},{26,61},{27,10},{27,59},{28,25},{29,86}
};

const int Chip5881::fn2_game_key_scheduling[FN2GK][2] = {
 {0,0},{1,3},{2,11},{3,20},{4,22},{5,23},{6,29},{7,38},
 {8,39},{9,55},{9,86},{9,87},{10,50},{11,57},{12,59},{13,61},
 {14,63},{15,67},{16,72},{17,83},{18,88},{19,94},{20,35},{21,17},
 {22,6},{23,85},{24,16},{25,25},{26,92},{27,47},{28,28},{29,90}
};

const int Chip5881::fn1_sequence_key_scheduling[20][2] = {
 {0,52},{1,34},{2,17},{3,36},{4,84},{4,88},{5,57},{6,48},
 {6,68},{7,76},{8,83},{9,30},{10,22},{10,41},{11,38},{12,55},
 {13,74},{14,19},{14,80},{15,26}
};

const int Chip5881::fn2_sequence_key_scheduling[16] = {
 77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64
};

const int Chip5881::fn2_middle_result_scheduling[16] = {
 1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58
};

const uint8_t Chip5881::trees[9][2][32] = {
 {{0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff},
  {0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff}},
 {{0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff},
  {0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
 {{0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
  {0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
};

} /* namespace MDFN_IEN_SS */
#endif
