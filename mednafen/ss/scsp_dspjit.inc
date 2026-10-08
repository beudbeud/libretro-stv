/******************************************************************************/
/* SCSP DSP microprogram JIT (AArch64)                                        */
/******************************************************************************/
/* Included from scsp.inc inside the module namespace, like the rest of the
** SCSP. See scsp_dspjit.h for the design. The reference semantics are the
** interpreter (SS_SCSP::RunDSP_Interp); every emitted sequence below is
** annotated with the interpreter statement it implements, in the same order.
**
** Register map (all callee-saved, so nothing is spilled between steps):
**   x19  DSPS*            x20  sound RAM (uint16*)
**   w21  INPUTS           w22  SFT_REG         w23  FRC_REG
**   w24  Y_REG            w25  ADRS_REG        w26  MDEC_CT
**   x27  &DSP.TEMP[0]     x28  &EXTS[0]
** Caller-saved, set once in the prologue (the code makes no calls):
**   w14  ring buffer mask ((0x2000 << RBL) - 1)     w15  RBP << 12
** Everything else (w0-w13, w16, w17) is scratch within a step.
*/

#include <sys/mman.h>
#include "scsp_dspjit_a64.h"

// DSPS/DSPStep are private to SS_SCSP; the member functions below are friends.
#define DOFF(m) ((unsigned)offsetof(SS_SCSP::DSPS, m))


//
//
//
SS_SCSP_DSPJIT::SS_SCSP_DSPJIT() : Pool(nullptr), PoolSize(0), Code(nullptr), P(nullptr), PEnd(nullptr), Overflow(false)
{

}

SS_SCSP_DSPJIT::~SS_SCSP_DSPJIT()
{
 if(Pool)
  munmap(Pool, PoolSize);
}

void SS_SCSP_DSPJIT::W(unsigned v)
{
 if(P < PEnd)
  *P++ = v;
 else
  Overflow = true;
}

void SS_SCSP_DSPJIT::Patch(unsigned* at, unsigned v)
{
 *at = v;
}

static void EmitMov32(SS_SCSP_DSPJIT* j, void (SS_SCSP_DSPJIT::*w)(unsigned), unsigned rd, unsigned imm)
{
 (j->*w)(E_MOVZ(rd, imm & 0xFFFF, 0));
 if(imm >> 16)
  (j->*w)(E_MOVK(rd, imm >> 16, 1));
}

void SS_SCSP_DSPJIT::EmitProgram(SS_SCSP* scsp)
{
 // Every DSPS field the JIT touches must fit the unsigned 12-bit immediate of
 // its access size (byte/add: 4095, halfword: 8190, word: 16380); the encoders
 // would silently truncate a larger offset.
 static_assert(offsetof(SS_SCSP::DSPS, TEMP) <= 4095, "TEMP beyond ADD immediate range");
 static_assert(offsetof(SS_SCSP::DSPS, ReadPending) <= 4095 && offsetof(SS_SCSP::DSPS, WritePending) <= 4095, "pending flags beyond LDRB/STRB range");
 static_assert(offsetof(SS_SCSP::DSPS, MADRS) + 2 * 32 <= 8190 && offsetof(SS_SCSP::DSPS, COEF) + 2 * 64 <= 8190 && offsetof(SS_SCSP::DSPS, EFREG) + 2 * 16 <= 8190 && offsetof(SS_SCSP::DSPS, WriteValue) <= 8190 && offsetof(SS_SCSP::DSPS, FRC_REG) <= 8190 && offsetof(SS_SCSP::DSPS, ADRS_REG) <= 8190 && offsetof(SS_SCSP::DSPS, MDEC_CT) <= 8190, "halfword field beyond LDRH/STRH range");
 static_assert(offsetof(SS_SCSP::DSPS, MEMS) + 4 * 32 <= 16380 && offsetof(SS_SCSP::DSPS, MIXS) + 4 * 16 <= 16380 && offsetof(SS_SCSP::DSPS, ReadValue) <= 16380 && offsetof(SS_SCSP::DSPS, RWAddr) <= 16380, "word field beyond LDR/STR range");

 typedef SS_SCSP::DSPStep DSPStep;
 const DSPStep* const steps = scsp->DSP.MPROG_Decoded;
 #define EMIT(x) W(x)
 #define MOV32(rd, imm) EmitMov32(this, &SS_SCSP_DSPJIT::W, (rd), (imm))

 //
 // Prologue
 //
 EMIT(E_STPX_PRE(R_FP, R_LR, R_SP, -96));
 EMIT(E_STPX(19, 20, R_SP, 16));
 EMIT(E_STPX(21, 22, R_SP, 32));
 EMIT(E_STPX(23, 24, R_SP, 48));
 EMIT(E_STPX(25, 26, R_SP, 64));
 EMIT(E_STPX(27, 28, R_SP, 80));
 EMIT(E_MOVXR(R_DSP, 0));
 EMIT(E_MOVXR(R_RAM, 1));
 EMIT(E_MOVXR(R_EXTS, 4));
 EMIT(E_LDR (R_INPUTS, R_DSP, DOFF(INPUTS)));
 EMIT(E_LDR (R_SFT,    R_DSP, DOFF(SFT_REG)));
 EMIT(E_LDRH(R_FRC,    R_DSP, DOFF(FRC_REG)));
 EMIT(E_LDR (R_Y,      R_DSP, DOFF(Y_REG)));
 EMIT(E_LDRH(R_ADRS,   R_DSP, DOFF(ADRS_REG)));
 EMIT(E_LDRH(R_MDEC,   R_DSP, DOFF(MDEC_CT)));
 EMIT(E_ADDXI(R_TEMP, R_DSP, DOFF(TEMP)));
 // w14 = (0x2000 << RBL) - 1 ; w15 = RBP << 12
 MOV32(R_MASK, 0x2000);
 EMIT(E_LSLV(R_MASK, R_MASK, 2));
 EMIT(E_SUBI(R_MASK, R_MASK, 1));
 EMIT(E_LSLI(R_RBP12, 3, 12));

 // Memory pipeline state (the interpreter's ReadPending/WritePending), tracked
 // statically so the tests can be elided: rp 0/1/2 (2 = NOFL), wp 0/1, and -1
 // for "unknown" (at entry: the previous program may differ, and a pending
 // write survives as long as reads keep taking priority over it). The memory
 // copies of both flags are kept exact at every change, so a run can stop
 // anywhere with the same state the interpreter would leave.
 int rp = -1, wp = -1;
 int last_live = -1;
 for(unsigned step = 0; step < 128; step++)
  if(steps[step].live)
   last_live = step;

 for(unsigned step = 0; step < 128; step++)
 {
  const DSPStep& s = steps[step];
  if(!s.live)
   continue;
  const unsigned f = s.flags;
  const unsigned shft0 = (f >> 7) & 1;
  const unsigned shft1 = (f >> 8) & 1;
  const unsigned IRA = s.IRA;

  // --- INPUTS selection (interpreter: the IRA if/else ladder) ---
  if(!(IRA & 0x20))
   EMIT(E_LDR(R_INPUTS, R_DSP, DOFF(MEMS) + (IRA & 0x1F) * 4));
  else if(!(IRA & 0x10))
  {
   EMIT(E_LDR(R_INPUTS, R_DSP, DOFF(MIXS) + (IRA & 0xF) * 4));
   EMIT(E_LSLI(R_INPUTS, R_INPUTS, 4));
  }
  else if(!(IRA & 0xE))
  {
   EMIT(E_LDRH(R_INPUTS, R_EXTS, (IRA & 1) * 2));
   EMIT(E_LSLI(R_INPUTS, R_INPUTS, 8));
  }
  // else: INPUTS unchanged.

  // w0 = INPUTS sign-extended from 24 bits
  EMIT(E_SBFX(0, R_INPUTS, 0, 24));

  // w1 = Y_SEL_Inputs[YSEL] as signed 13-bit (uses Y_REG before YRL)
  switch(s.YSEL)
  {
   case 0: EMIT(E_SBFX(1, R_FRC, 0, 13)); break;
   case 1: EMIT(E_LDRH(1, R_DSP, DOFF(COEF) + s.CRA * 2)); EMIT(E_SBFX(1, 1, 0, 13)); break;
   case 2: EMIT(E_SBFX(1, R_Y, 11, 13)); break;
   default: EMIT(E_UBFX(1, R_Y, 4, 12)); break;	// 12-bit value: sign bit of the 13-bit field is 0
  }

  // if(f & DSPF_YRL) Y_REG = INPUTS & 0xFFFFFF;
  if(f & SS_SCSP::DSPF_YRL)
   EMIT(E_ANDI(R_Y, R_INPUTS, 0xFFFFFF));

  // Shifter: w2 = ShifterOutput (24-bit, unsigned storage)
  EMIT(E_SBFX(2, R_SFT, 0, 26));
  if(shft0 ^ shft1)
   EMIT(E_LSLI(2, 2, 1));
  if(!shft1)
  {
   MOV32(3, 0x7FFFFF);
   EMIT(E_CMPR(2, 3));
   EMIT(E_CSEL(2, 3, 2, CC_GT));
   MOV32(3, 0xFF800000);
   EMIT(E_CMPR(2, 3));
   EMIT(E_CSEL(2, 3, 2, CC_LT));
  }
  EMIT(E_ANDI(2, 2, 0xFFFFFF));

  // if(f & DSPF_FRCL) FRC_REG = F_SEL_Inputs[shft0 & shft1];
  if(f & SS_SCSP::DSPF_FRCL)
  {
   if(shft0 & shft1)
    EMIT(E_ANDI(R_FRC, 2, 0xFFF));
   else
    EMIT(E_LSRI(R_FRC, 2, 11));
  }

  // Multiply/accumulate into SFT_REG
  {
   const bool xsel = (f & SS_SCSP::DSPF_XSEL) != 0;
   const bool bsel = (f & SS_SCSP::DSPF_BSEL) != 0;
   // w7 = TEMP (sign-extended 24-bit) when needed
   if(!xsel || !bsel)
   {
    EMIT(E_ADDI(5, R_MDEC, s.TRA));
    EMIT(E_ANDI(5, 5, 0x7F));
    EMIT(E_LDR_UXTW2(5, R_TEMP, 5));
    EMIT(E_SBFX(7, 5, 0, 24));
   }
   // x8 = ((int64)Y * X) >> 12
   EMIT(E_SMULL(8, 1, xsel ? 0 : 7));
   EMIT(E_ASRXI(8, 8, 12));
   // w9 = SGAOutput
   if(f & SS_SCSP::DSPF_ZERO)
    EMIT(E_MOVR(9, R_ZR));
   else
   {
    const unsigned src = bsel ? R_SFT : 7;
    if(f & SS_SCSP::DSPF_NEGB)
     EMIT(E_NEG(9, src));
    else
     EMIT(E_MOVR(9, src));
   }
   EMIT(E_ADDR(R_SFT, 8, 9));
   EMIT(E_ANDI(R_SFT, R_SFT, 0x3FFFFFF));
  }

  // if(f & DSPF_EWT) EFREG[EWA] = ShifterOutput >> 8;
  if(f & SS_SCSP::DSPF_EWT)
  {
   EMIT(E_LSRI(10, 2, 8));
   EMIT(E_STRH(10, R_DSP, DOFF(EFREG) + s.EWA * 2));
  }
  // if(f & DSPF_TWT) TEMP[(TWA + MDEC_CT) & 0x7F] = ShifterOutput;
  if(f & SS_SCSP::DSPF_TWT)
  {
   EMIT(E_ADDI(11, R_MDEC, s.TWA));
   EMIT(E_ANDI(11, 11, 0x7F));
   EMIT(E_STR_UXTW2(2, R_TEMP, 11));
  }
  // if(f & DSPF_IWT) MEMS[IWA] = ReadValue;
  if(f & SS_SCSP::DSPF_IWT)
  {
   EMIT(E_LDR(10, R_DSP, DOFF(ReadValue)));
   EMIT(E_STR(10, R_DSP, DOFF(MEMS) + s.IWA * 4));
  }

  // --- Memory pipeline, interpreter order: consume the access started by an
  // earlier step (read has priority; a write then waits), then address
  // generation, then start this step's access. ---
  {
   // w10 = RAM[RWAddr] converted per NOFL -> ReadValue; ReadPending = 0.
   auto emit_read = [&](bool nofl)
   {
    EMIT(E_LDR(10, R_DSP, DOFF(RWAddr)));
    EMIT(E_LDRH_UXTW1(10, R_RAM, 10));
    if(nofl)
     EMIT(E_LSLI(10, 10, 8));
    else
    {
     // dspfloat_to_int(w10) -> w10
     EMIT(E_UBFX(11, 10, 11, 4));		// exp
     EMIT(E_ANDI(12, 10, 0x7FF));		// mantissa
     EMIT(E_CMPI(11, 12));
     EMIT(E_ORRI(13, 12, 0x800));
     EMIT(E_CSEL(12, 13, 12, CC_LO));		// exp < 12: hidden bit
     EMIT(E_LSLI(12, 12, 19));
     EMIT(E_SBFX(13, 10, 15, 1));		// sign ? -1 : 0
     EMIT(E_ANDI(13, 13, 0xC0000000));		// sign_xor
     EMIT(E_EORR(12, 12, 13));
     MOV32(13, 11);
     EMIT(E_CMPI(11, 11));
     EMIT(E_CSEL(11, 13, 11, CC_HI));		// min(11, exp)
     EMIT(E_ADDI(11, 11, 8));
     EMIT(E_ASRV(12, 12, 11));
     EMIT(E_ANDI(10, 12, 0xFFFFFF));
    }
    EMIT(E_STR(10, R_DSP, DOFF(ReadValue)));
    EMIT(E_STRB(R_ZR, R_DSP, DOFF(ReadPending)));
   };
   // if(!(RWAddr & 0x40000)) RAM[RWAddr] = WriteValue; WritePending = 0.
   auto emit_write = [&]()
   {
    EMIT(E_LDR(10, R_DSP, DOFF(RWAddr)));
    unsigned* br = P;
    EMIT(E_TBNZ(10, 18, 0));
    EMIT(E_LDRH(11, R_DSP, DOFF(WriteValue)));
    EMIT(E_STRH_UXTW1(11, R_RAM, 10));
    if(br < PEnd) Patch(br, E_TBNZ(10, 18, (int)(P - br)));
    EMIT(E_STRB(R_ZR, R_DSP, DOFF(WritePending)));
   };
   // Dynamic: if(WritePending) write.
   auto emit_write_check = [&]()
   {
    EMIT(E_LDRB(10, R_DSP, DOFF(WritePending)));
    unsigned* br = P;
    EMIT(E_CBZ(10, 0));
    emit_write();
    if(br < PEnd) Patch(br, E_CBZ(10, (int)(P - br)));
   };

   if(rp < 0)
   {
    // Dynamic: if(ReadPending) read (NOFL = ReadPending == 2) else if(WritePending) write.
    EMIT(E_LDRB(10, R_DSP, DOFF(ReadPending)));
    unsigned* b_nord = P; EMIT(E_CBZ(10, 0));
    EMIT(E_CMPI(10, 2));
    EMIT(E_CSET(16, CC_EQ));
    unsigned* b_sel = P; EMIT(E_CBNZ(16, 0));
    emit_read(false);
    unsigned* b_done1 = P; EMIT(E_B(0));
    if(b_sel < PEnd) Patch(b_sel, E_CBNZ(16, (int)(P - b_sel)));
    emit_read(true);
    if(b_done1 < PEnd) Patch(b_done1, E_B((int)(P - b_done1)));
    unsigned* b_done2 = P; EMIT(E_B(0));
    if(b_nord < PEnd) Patch(b_nord, E_CBZ(10, (int)(P - b_nord)));
    emit_write_check();
    if(b_done2 < PEnd) Patch(b_done2, E_B((int)(P - b_done2)));
    rp = 0;
    wp = -1;	// survived if a read was pending
   }
   else if(rp > 0)
   {
    emit_read(rp == 2);
    rp = 0;	// a pending write (if any) waits
   }
   else if(wp > 0)
   {
    emit_write();
    wp = 0;
   }
   else if(wp < 0)
   {
    emit_write_check();
    wp = 0;
   }
  }

  // Address generation: the interpreter does it every step; it is only
  // observable when this step starts an access, when a write may still be
  // consumed by the next step (it uses this step's RWAddr), or in the final
  // state after the last step.
  if((f & (SS_SCSP::DSPF_MRT | SS_SCSP::DSPF_MWT)) || wp != 0 || (int)step == last_live)
  {
   // uint16 addr = MADRS[MASA] (+1) (+ sext12(ADRS_REG)) ; if(!TABLE) { addr += MDEC_CT; addr &= mask; }
   EMIT(E_LDRH(12, R_DSP, DOFF(MADRS) + s.MASA * 2));
   if(f & SS_SCSP::DSPF_NXADDR)
    EMIT(E_ADDI(12, 12, 1));
   if(f & SS_SCSP::DSPF_ADRGB)
   {
    EMIT(E_SBFX(13, R_ADRS, 0, 12));
    EMIT(E_ADDR(12, 12, 13));
   }
   if(!(f & SS_SCSP::DSPF_TABLE))
   {
    EMIT(E_ADDR(12, 12, R_MDEC));
    EMIT(E_ANDR(12, 12, R_MASK));
   }
   EMIT(E_ANDI(12, 12, 0xFFFF));		// uint16 addr
   // RWAddr = (addr + (RBP << 12)) & 0x7FFFF
   EMIT(E_ADDR(12, 12, R_RBP12));
   EMIT(E_ANDI(12, 12, 0x7FFFF));
   EMIT(E_STR(12, R_DSP, DOFF(RWAddr)));
  }

  if(f & SS_SCSP::DSPF_MRT)
  {
   rp = (f & SS_SCSP::DSPF_NOFL) ? 2 : 1;
   MOV32(10, (unsigned)rp);
   EMIT(E_STRB(10, R_DSP, DOFF(ReadPending)));
  }
  if(f & SS_SCSP::DSPF_MWT)
  {
   wp = 1;
   // WriteValue = NOFL ? (ShifterOutput >> 8) : int_to_dspfloat(ShifterOutput)
   if(f & SS_SCSP::DSPF_NOFL)
    EMIT(E_LSRI(10, 2, 8));
   else
   {
    EMIT(E_LSLI(10, 2, 8));			// invsl8
    EMIT(E_ASRI(11, 10, 31));			// sign_xor
    EMIT(E_EORR(12, 10, 11));
    EMIT(E_LSLI(12, 12, 1));
    EMIT(E_ORRI(12, 12, 0x80000));
    EMIT(E_CLZ(12, 12));			// exp
    EMIT(E_CMPI(12, 12));
    EMIT(E_CSET(13, CC_EQ));
    EMIT(E_SUBR(13, 12, 13));			// shift
    MOV32(11, 19);
    EMIT(E_SUBR(13, 11, 13));			// 19 - shift
    EMIT(E_ASRV(10, 10, 13));
    MOV32(11, 0x87FF);
    EMIT(E_ANDR(10, 10, 11));
    EMIT(E_ORRR_LSL(10, 10, 12, 11));		// |= exp << 11
   }
   EMIT(E_STRH(10, R_DSP, DOFF(WriteValue)));
   MOV32(10, 1);
   EMIT(E_STRB(10, R_DSP, DOFF(WritePending)));
  }

  // if(f & DSPF_ADRL) ADRS_REG = A_SEL_Inputs[shft0 & shft1];
  if(f & SS_SCSP::DSPF_ADRL)
  {
   if(shft0 & shft1)
    EMIT(E_LSRI(R_ADRS, 2, 12));
   else
    EMIT(E_UBFX(R_ADRS, 0, 16, 12));
  }
 }

 //
 // Epilogue: MDEC_CT update, carried registers back to the struct (the
 // pending flags in memory are already exact).
 //
 // if(!MDEC_CT) MDEC_CT = (0x2000 << RBL); MDEC_CT--;   (uint16)
 {
  unsigned* br = P;
  EMIT(E_CBNZ(R_MDEC, 0));
  EMIT(E_ADDI(R_MDEC, R_MASK, 1));
  if(br < PEnd) Patch(br, E_CBNZ(R_MDEC, (int)(P - br)));
  EMIT(E_SUBI(R_MDEC, R_MDEC, 1));
 }
 EMIT(E_STR (R_INPUTS, R_DSP, DOFF(INPUTS)));
 EMIT(E_STR (R_SFT,    R_DSP, DOFF(SFT_REG)));
 EMIT(E_STRH(R_FRC,    R_DSP, DOFF(FRC_REG)));
 EMIT(E_STR (R_Y,      R_DSP, DOFF(Y_REG)));
 EMIT(E_STRH(R_ADRS,   R_DSP, DOFF(ADRS_REG)));
 EMIT(E_STRH(R_MDEC,   R_DSP, DOFF(MDEC_CT)));
 EMIT(E_LDPX(19, 20, R_SP, 16));
 EMIT(E_LDPX(21, 22, R_SP, 32));
 EMIT(E_LDPX(23, 24, R_SP, 48));
 EMIT(E_LDPX(25, 26, R_SP, 64));
 EMIT(E_LDPX(27, 28, R_SP, 80));
 EMIT(E_LDPX_POST(R_FP, R_LR, R_SP, 96));
 EMIT(E_RET());

 #undef EMIT
 #undef MOV32
}

bool SS_SCSP_DSPJIT::Compile(SS_SCSP* scsp)
{
 if(!Pool)
 {
  PoolSize = 128 * 1024;
  void* p = mmap(nullptr, PoolSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(p == MAP_FAILED)
   return false;
  Pool = p;
 }
 else if(mprotect(Pool, PoolSize, PROT_READ | PROT_WRITE) != 0)
 {
  Code = nullptr;
  return false;
 }

 P = (unsigned*)Pool;
 PEnd = P + PoolSize / 4;
 Overflow = false;
 EmitProgram(scsp);

 if(Overflow || mprotect(Pool, PoolSize, PROT_READ | PROT_EXEC) != 0)
 {
  Code = nullptr;
  return false;
 }
 __builtin___clear_cache((char*)Pool, (char*)P);
 Code = (void (*)(void*, unsigned short*, unsigned, unsigned, unsigned short*))Pool;
 return true;
}

void SS_SCSP_DSPJIT::Run(SS_SCSP* scsp)
{
 Code(&scsp->DSP, scsp->RAM, scsp->RBL, scsp->RBP, scsp->EXTS);
}
#undef DOFF
