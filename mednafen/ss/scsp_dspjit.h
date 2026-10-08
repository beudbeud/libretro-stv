/******************************************************************************/
/* SCSP DSP microprogram JIT (AArch64)                                        */
/******************************************************************************/
/*
** Compiles the 128-step DSP program (already decoded and liveness-analysed by
** SS_SCSP::DecodeMPROG) into straight-line native code: every selector and
** flag is folded at compile time, dead steps are dropped, and the DSP's
** carried registers (INPUTS, SFT_REG, FRC_REG, Y_REG, ADRS_REG, MDEC_CT) live
** in machine registers for the whole program. The generated code reads and
** writes the same DSPS fields as the interpreter, so savestates, the 68K and
** the mixer see no difference; it is bit-exact with SS_SCSP::RunDSP().
**
** Only the memory pipeline (MRT/MWT) touches memory beyond the DSPS struct
** and the sound RAM. Its one-step latency is static (it depends on the
** previous live step's flags), except for the first live step of a run,
** which checks ReadPending/WritePending dynamically because the previous
** program may have been different.
*/

#ifndef __MDFN_SS_SCSP_DSPJIT_H
#define __MDFN_SS_SCSP_DSPJIT_H

// -DMDFN_SS_SCSP_DSP_JIT=0/1 overrides the platform default (the encoder
// self-test builds the emitter on x86 hosts this way).
#ifndef MDFN_SS_SCSP_DSP_JIT
 #if defined(__aarch64__) && (defined(__linux__) || defined(__ANDROID__))
  #define MDFN_SS_SCSP_DSP_JIT 1
 #else
  #define MDFN_SS_SCSP_DSP_JIT 0
 #endif
#endif

#if MDFN_SS_SCSP_DSP_JIT


#include <stddef.h>

class SS_SCSP;

class SS_SCSP_DSPJIT
{
 public:
 SS_SCSP_DSPJIT();
 ~SS_SCSP_DSPJIT();

 // Builds code for the program in scsp->DSP.MPROG_Decoded. Returns false
 // (and leaves no usable code) if executable memory is unavailable.
 bool Compile(SS_SCSP* scsp);

 // Runs one sample's worth of DSP program. Only valid after a successful
 // Compile(); the caller handles MPROG_Dirty.
 void Run(SS_SCSP* scsp);

 bool Ready(void) const { return Code != nullptr; }

 private:
 void* Pool;
 size_t PoolSize;
 void (*Code)(void* dsp, unsigned short* ram, unsigned rbl, unsigned rbp, unsigned short* exts);

 // Emitter state (valid during Compile only).
 unsigned* P;
 unsigned* PEnd;
 bool Overflow;

 void W(unsigned v);
 void Patch(unsigned* at, unsigned v);

 void EmitProgram(SS_SCSP* scsp);
};

#endif /* MDFN_SS_SCSP_DSP_JIT */

#endif
