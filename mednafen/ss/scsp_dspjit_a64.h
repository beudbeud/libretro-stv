/******************************************************************************/
/* SCSP DSP JIT: AArch64 instruction encoders                                 */
/******************************************************************************/
/* Pure functions, no emulator dependencies: tests/dspjit_enc_test.cpp checks
** them against GNU as (tests/dspjit_enc.s). See scsp_dspjit.inc for the
** register conventions.
*/

#ifndef __MDFN_SS_SCSP_DSPJIT_A64_H
#define __MDFN_SS_SCSP_DSPJIT_A64_H

#include <assert.h>
#include <stddef.h>

enum : unsigned
{
 R_DSP = 19, R_RAM = 20,
 R_INPUTS = 21, R_SFT = 22, R_FRC = 23, R_Y = 24, R_ADRS = 25, R_MDEC = 26,
 R_TEMP = 27, R_EXTS = 28,
 R_MASK = 14, R_RBP12 = 15,
 R_SP = 31, R_ZR = 31, R_FP = 29, R_LR = 30
};

enum : unsigned { CC_EQ = 0, CC_NE = 1, CC_HS = 2, CC_LO = 3, CC_HI = 8, CC_LS = 9, CC_GE = 10, CC_LT = 11, CC_GT = 12, CC_LE = 13 };



//
// Instruction encoders. 32-bit (w) forms unless the name says x.
//
static inline unsigned E_MOVZ(unsigned rd, unsigned imm16, unsigned hw) { return 0x52800000 | (hw << 21) | (imm16 << 5) | rd; }
static inline unsigned E_MOVK(unsigned rd, unsigned imm16, unsigned hw) { return 0x72800000 | (hw << 21) | (imm16 << 5) | rd; }
static inline unsigned E_ADDI(unsigned rd, unsigned rn, unsigned imm12) { return 0x11000000 | (imm12 << 10) | (rn << 5) | rd; }
static inline unsigned E_SUBI(unsigned rd, unsigned rn, unsigned imm12) { return 0x51000000 | (imm12 << 10) | (rn << 5) | rd; }
static inline unsigned E_ADDXI(unsigned rd, unsigned rn, unsigned imm12) { return 0x91000000 | (imm12 << 10) | (rn << 5) | rd; }
static inline unsigned E_ADDR(unsigned rd, unsigned rn, unsigned rm) { return 0x0B000000 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_SUBR(unsigned rd, unsigned rn, unsigned rm) { return 0x4B000000 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_NEG(unsigned rd, unsigned rm) { return E_SUBR(rd, R_ZR, rm); }
static inline unsigned E_CMPI(unsigned rn, unsigned imm12) { return 0x71000000 | (imm12 << 10) | (rn << 5) | R_ZR; }
static inline unsigned E_CMPR(unsigned rn, unsigned rm) { return 0x6B000000 | (rm << 16) | (rn << 5) | R_ZR; }
static inline unsigned E_CSEL(unsigned rd, unsigned rn, unsigned rm, unsigned cond) { return 0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd; }
static inline unsigned E_CSET(unsigned rd, unsigned cond) { return 0x1A800400 | (R_ZR << 16) | ((cond ^ 1) << 12) | (R_ZR << 5) | rd; }
static inline unsigned E_ANDR(unsigned rd, unsigned rn, unsigned rm) { return 0x0A000000 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_ORRR(unsigned rd, unsigned rn, unsigned rm) { return 0x2A000000 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_ORRR_LSL(unsigned rd, unsigned rn, unsigned rm, unsigned sh) { return 0x2A000000 | (rm << 16) | (sh << 10) | (rn << 5) | rd; }
static inline unsigned E_EORR(unsigned rd, unsigned rn, unsigned rm) { return 0x4A000000 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_MOVR(unsigned rd, unsigned rm) { return 0x2A0003E0 | (rm << 16) | rd; }
static inline unsigned E_MOVXR(unsigned rd, unsigned rm) { return 0xAA0003E0 | (rm << 16) | rd; }
// UBFM/SBFM-based shifts and field extracts.
static inline unsigned E_LSLI(unsigned rd, unsigned rn, unsigned s) { return 0x53000000 | (((32 - s) & 31) << 16) | ((31 - s) << 10) | (rn << 5) | rd; }
static inline unsigned E_LSRI(unsigned rd, unsigned rn, unsigned s) { return 0x53000000 | (s << 16) | (31 << 10) | (rn << 5) | rd; }
static inline unsigned E_ASRI(unsigned rd, unsigned rn, unsigned s) { return 0x13000000 | (s << 16) | (31 << 10) | (rn << 5) | rd; }
static inline unsigned E_SBFX(unsigned rd, unsigned rn, unsigned lsb, unsigned width) { return 0x13000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd; }
static inline unsigned E_UBFX(unsigned rd, unsigned rn, unsigned lsb, unsigned width) { return 0x53000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd; }
static inline unsigned E_ASRV(unsigned rd, unsigned rn, unsigned rm) { return 0x1AC02800 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_LSLV(unsigned rd, unsigned rn, unsigned rm) { return 0x1AC02000 | (rm << 16) | (rn << 5) | rd; }
static inline unsigned E_CLZ(unsigned rd, unsigned rn) { return 0x5AC01000 | (rn << 5) | rd; }
static inline unsigned E_SMULL(unsigned xd, unsigned wn, unsigned wm) { return 0x9B207C00 | (wm << 16) | (wn << 5) | xd; }
static inline unsigned E_ASRXI(unsigned xd, unsigned xn, unsigned s) { return 0x9340FC00 | (s << 16) | (xn << 5) | xd; }
// Loads/stores, unsigned scaled immediate offset (bytes).
static inline unsigned E_LDR(unsigned rt, unsigned xn, unsigned off) { return 0xB9400000 | ((off / 4) << 10) | (xn << 5) | rt; }
static inline unsigned E_STR(unsigned rt, unsigned xn, unsigned off) { return 0xB9000000 | ((off / 4) << 10) | (xn << 5) | rt; }
static inline unsigned E_LDRH(unsigned rt, unsigned xn, unsigned off) { return 0x79400000 | ((off / 2) << 10) | (xn << 5) | rt; }
static inline unsigned E_STRH(unsigned rt, unsigned xn, unsigned off) { return 0x79000000 | ((off / 2) << 10) | (xn << 5) | rt; }
static inline unsigned E_LDRB(unsigned rt, unsigned xn, unsigned off) { return 0x39400000 | (off << 10) | (xn << 5) | rt; }
static inline unsigned E_STRB(unsigned rt, unsigned xn, unsigned off) { return 0x39000000 | (off << 10) | (xn << 5) | rt; }
// Loads/stores, register offset [xn, wm, UXTW #scale].
static inline unsigned E_LDR_UXTW2(unsigned rt, unsigned xn, unsigned wm) { return 0xB8605800 | (wm << 16) | (xn << 5) | rt; }
static inline unsigned E_STR_UXTW2(unsigned rt, unsigned xn, unsigned wm) { return 0xB8205800 | (wm << 16) | (xn << 5) | rt; }
static inline unsigned E_LDRH_UXTW1(unsigned rt, unsigned xn, unsigned wm) { return 0x78605800 | (wm << 16) | (xn << 5) | rt; }
static inline unsigned E_STRH_UXTW1(unsigned rt, unsigned xn, unsigned wm) { return 0x78205800 | (wm << 16) | (xn << 5) | rt; }
// Pairs (64-bit), signed offset in bytes.
static inline unsigned E_STPX(unsigned rt, unsigned rt2, unsigned xn, int off) { return 0xA9000000 | (((off / 8) & 0x7F) << 15) | (rt2 << 10) | (xn << 5) | rt; }
static inline unsigned E_LDPX(unsigned rt, unsigned rt2, unsigned xn, int off) { return 0xA9400000 | (((off / 8) & 0x7F) << 15) | (rt2 << 10) | (xn << 5) | rt; }
static inline unsigned E_STPX_PRE(unsigned rt, unsigned rt2, unsigned xn, int off) { return 0xA9800000 | (((off / 8) & 0x7F) << 15) | (rt2 << 10) | (xn << 5) | rt; }
static inline unsigned E_LDPX_POST(unsigned rt, unsigned rt2, unsigned xn, int off) { return 0xA8C00000 | (((off / 8) & 0x7F) << 15) | (rt2 << 10) | (xn << 5) | rt; }
static inline unsigned E_RET(void) { return 0xD65F03C0; }
// Branches; offsets in instructions.
static inline unsigned E_B(int off) { return 0x14000000 | ((unsigned)off & 0x3FFFFFF); }
static inline unsigned E_CBZ(unsigned rt, int off) { return 0x34000000 | (((unsigned)off & 0x7FFFF) << 5) | rt; }
static inline unsigned E_CBNZ(unsigned rt, int off) { return 0x35000000 | (((unsigned)off & 0x7FFFF) << 5) | rt; }
static inline unsigned E_TBNZ(unsigned rt, unsigned bit, int off) { return 0x37000000 | ((bit & 31) << 19) | (((unsigned)off & 0x3FFF) << 5) | rt; }

// Logical immediate (32-bit element forms, N=0). Returns false if the value
// is not encodable; callers only pass masks that are.
static bool LogImm32(unsigned v, unsigned* out)
{
 if(v == 0 || v == 0xFFFFFFFF)
  return false;

 unsigned e = 32;
 // Smallest repeating element size.
 for(unsigned t = 2; t < 32; t <<= 1)
 {
  const unsigned m = (1u << t) - 1;
  bool rep = true;
  for(unsigned i = t; i < 32; i += t)
   if(((v >> i) & m) != (v & m)) { rep = false; break; }
  if(rep) { e = t; break; }
 }
 const unsigned em = (e == 32) ? 0xFFFFFFFF : ((1u << e) - 1);
 const unsigned x = v & em;
 const unsigned ones = __builtin_popcount(x);
 if(ones == 0 || ones == e)
  return false;
 const unsigned run = (ones == 32) ? 0xFFFFFFFF : ((1u << ones) - 1);
 for(unsigned r = 0; r < e; r++)
 {
  // ROR(run, r) within e bits
  const unsigned rot = r ? (((run >> r) | (run << (e - r))) & em) : run;
  if(rot == x)
  {
   unsigned imms;
   switch(e)
   {
    case 32: imms = ones - 1; break;
    case 16: imms = 0x20 | (ones - 1); break;
    case  8: imms = 0x30 | (ones - 1); break;
    case  4: imms = 0x38 | (ones - 1); break;
    default: imms = 0x3C | (ones - 1); break;
   }
   *out = (r << 16) | (imms << 10);
   return true;
  }
 }
 return false;
}
static inline unsigned E_ANDI(unsigned rd, unsigned rn, unsigned imm) { unsigned li = 0; bool ok = LogImm32(imm, &li); assert(ok); (void)ok; return 0x12000000 | li | (rn << 5) | rd; }
static inline unsigned E_ORRI(unsigned rd, unsigned rn, unsigned imm) { unsigned li = 0; bool ok = LogImm32(imm, &li); assert(ok); (void)ok; return 0x32000000 | li | (rn << 5) | rd; }


static inline size_t DSPJIT_EncodeSelfTest(unsigned* out, size_t max)
{
 // Keep in sync with tests/dspjit_enc.s.
 const unsigned v[] =
 {
  E_MOVZ(3, 0xFFFF, 0), E_MOVK(3, 0x7F, 1), E_ADDI(5, 26, 0x55), E_SUBI(14, 14, 1), E_ADDXI(27, 19, 3072),
  E_ADDR(22, 8, 9), E_SUBR(13, 11, 13), E_NEG(9, 7), E_CMPI(11, 12), E_CMPR(2, 3),
  E_CSEL(2, 3, 2, CC_GT), E_CSEL(12, 13, 12, CC_LO), E_CSET(13, CC_EQ), E_ANDR(10, 10, 11), E_ORRR_LSL(10, 10, 12, 11),
  E_EORR(12, 12, 13), E_MOVR(9, 31), E_MOVXR(19, 0), E_LSLI(21, 21, 4), E_LSRI(23, 2, 11),
  E_ASRI(11, 10, 31), E_SBFX(0, 21, 0, 24), E_UBFX(1, 24, 4, 12), E_ASRV(12, 12, 11), E_LSLV(14, 14, 2),
  E_CLZ(12, 12), E_SMULL(8, 1, 7), E_ASRXI(8, 8, 12), E_LDR(21, 19, 3072 + 128), E_STR(10, 19, 3072 + 12),
  E_LDRH(23, 19, 3600), E_STRH(10, 19, 3602), E_LDRB(10, 19, 3611), E_STRB(31, 19, 3611), E_LDR_UXTW2(5, 27, 5),
  E_STR_UXTW2(2, 27, 11), E_LDRH_UXTW1(10, 20, 10), E_STRH_UXTW1(11, 20, 10), E_STPX_PRE(29, 30, 31, -96), E_STPX(19, 20, 31, 16),
  E_LDPX(27, 28, 31, 80), E_LDPX_POST(29, 30, 31, 96), E_RET(), E_B(5), E_CBZ(10, 7),
  E_CBNZ(26, 2), E_TBNZ(10, 18, 3), E_ANDI(2, 2, 0xFFFFFF), E_ANDI(22, 22, 0x3FFFFFF), E_ANDI(5, 5, 0x7F),
  E_ANDI(23, 2, 0xFFF), E_ANDI(12, 10, 0x7FF), E_ORRI(13, 12, 0x800), E_ANDI(13, 13, 0xC0000000), E_ORRI(12, 12, 0x80000),
  E_ANDI(12, 12, 0xFFFF), E_ANDI(12, 12, 0x7FFFF), E_ANDI(10, 12, 0xFFFFFF),
 };
 const size_t n = sizeof(v) / sizeof(v[0]);
 for(size_t i = 0; i < n && i < max; i++)
  out[i] = v[i];
 return n;
}


#endif
