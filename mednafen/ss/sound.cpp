/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* sound.cpp - Sound Emulation
**  Copyright (C) 2015-2021 Mednafen Team
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

// TODO: Bus between SCU and SCSP looks to be 8-bit, maybe implement that, but
// first test to see how the bus access cycle(s) work with respect to reading from
// registers whose values may change between the individual byte reads.
// (May not be worth emulating if it could possibly trigger problems in games)

#include <mednafen/mednafen.h>
#include <mednafen/hw_cpu/m68k/m68k.h>
#include <mednafen/jump.h>
#include <mednafen/MThreading.h>
#include <atomic>

#ifndef MDFN_SSFPLAY_COMPILE
#include "ss.h"
#include "sound.h"
#include "scu.h"
#include "cdb.h"

namespace MDFN_IEN_SS
{

/* Forward declaration — defined in cart/acclaim_rax.cpp */
void RAX_MixSample(int16* left, int16* right);
#else
namespace MDFN_IEN_SSFPLAY
{
#endif

#include "scsp.h"

static SS_SCSP SCSP;
static void (*MIDI_Out)(uint8);

static M68K SoundCPU(true);
static int64 run_until_time;	// 32.32
static int32 next_scsp_time;

static uint32 clock_ratio;
static sscpu_timestamp_t lastts;

static MDFN_jmp_buf jbuf;

static int16 IBuffer[1024][2];
static uint32 IBufferCount;
static int last_rate;
static bool rax_active = false;

#ifndef MDFN_SSFPLAY_COMPILE
/*
** Sound thread. The 68K and SCSP already run behind the SH-2s: SOUND_Update()
** advances them in 128-cycle slices and the SH-2's SCSP register accesses are
** applied to that lagging state. A single-producer/single-consumer command
** queue keeps exactly that ordering (RUN slices and register writes in the
** order the main thread issued them) while a second core does the work.
** The main thread waits for the queue to drain only where it needs the
** results: register reads, the audio output at frame end, timestamp rebases
** and savestates. Sound RAM stays directly mapped for the SH-2s; the 68K may
** see one of their writes a few microseconds earlier than it would today,
** the same class of skew as the existing 128-cycle slicing.
*/
enum
{
 SCMD_RUN = 0,		/* arg32 = SH-2 timestamp */
 SCMD_WRITE8,		/* arg32 = address, arg16 = value */
 SCMD_WRITE16,
 SCMD_RESET,		/* arg16 = powering_up */
 SCMD_RESET68K,
 SCMD_RESETSCSP,
 SCMD_SET68KACTIVE,	/* arg16 = active */
 SCMD_CLOCKRATIO,	/* arg32 = ratio */
 SCMD_EXIT
};

struct SoundCmd
{
 uint32 arg32;
 uint16 arg16;
 uint8 cmd;
};

static const uint32 SQ_SIZE = 1 << 14;
static SoundCmd SQ[SQ_SIZE];
/* Producer- and consumer-written counters on their own cache lines. */
alignas(64) static std::atomic<uint32> SQ_Enq(0);	/* commands pushed (monotonic) */
alignas(64) static std::atomic<uint32> SQ_Done(0);	/* commands completed (monotonic) */
alignas(64) static std::atomic<bool> SThreadSleeping(false);
static uint32 SQ_DoneCached = 0;		/* producer's last view of SQ_Done */
/* RUN slices are batched: SOUND_Update() records the latest timestamp and
** only every 8th one is pushed, unless something that must stay ordered
** behind it (a register write, a drain) flushes it first. */
static sscpu_timestamp_t SPendingRunTS;
static bool SPendingRun = false;
static unsigned SRunBatch = 0;
/* SCSP->SCU interrupt transitions posted by the thread: bit 0 = rose, bit 1
** = fell, bit 2 = current level; the main thread replays rise then fall so a
** pulse inside one slice still reaches the SCU's edge latch. */
static std::atomic<unsigned> SMainIntEvents(0);
static bool SThreadWanted = false;		/* setting/env */
static bool SThreaded = false;			/* thread running and in use */
static MThreading::Thread* SThread = nullptr;
static MThreading::Sem* SSem = nullptr;

static INLINE void SoundThread_Push(uint8 cmd, uint32 arg32 = 0, uint16 arg16 = 0)
{
 const uint32 e = SQ_Enq.load(std::memory_order_relaxed);

 if(MDFN_UNLIKELY(e - SQ_DoneCached >= SQ_SIZE))
 {
  /* Queue possibly full: refresh the view, then wait for the thread. */
  while((SQ_DoneCached = SQ_Done.load(std::memory_order_acquire)), e - SQ_DoneCached >= SQ_SIZE)
  {
  }
 }
 SQ[e & (SQ_SIZE - 1)].arg32 = arg32;
 SQ[e & (SQ_SIZE - 1)].arg16 = arg16;
 SQ[e & (SQ_SIZE - 1)].cmd = cmd;
 SQ_Enq.store(e + 1, std::memory_order_release);

 if(MDFN_UNLIKELY(SThreadSleeping.load(std::memory_order_acquire)))
  MThreading::Sem_Post(SSem);
}

static INLINE void SoundThread_FlushRun(void)
{
 if(SPendingRun)
 {
  SPendingRun = false;
  SoundThread_Push(SCMD_RUN, (uint32)SPendingRunTS);
 }
}

static void SoundThread_Drain(void)
{
 if(!SThreaded)
  return;
 SoundThread_FlushRun();
 while(SQ_Done.load(std::memory_order_acquire) != SQ_Enq.load(std::memory_order_relaxed))
 {
  /* Spin: the thread is usually only microseconds behind. */
 }
}

#endif

static INLINE void SCSP_SoundIntChanged(SS_SCSP* s, unsigned level)
{
 SoundCPU.SetIPL(level);
}

static INLINE void SCSP_MainIntChanged(SS_SCSP* s, bool state)
{
 #ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded)
  if(state) SMainIntEvents.fetch_or(1 | 4, std::memory_order_release);
  else { SMainIntEvents.fetch_or(2, std::memory_order_release); SMainIntEvents.fetch_and(~4u, std::memory_order_release); }	/* applied by SOUND_Update() */
 else
  SCU_SetInt(SCU_INT_SCSP, state);
 #endif
}

#include "scsp.inc"

//
//
template<typename T, bool TA_STV>
static MDFN_FASTCALL T SoundCPU_BusRead(uint32 A);

template<bool TA_STV>
static MDFN_FASTCALL uint16 SoundCPU_BusReadInstr(uint32 A);

template<typename T, bool TA_STV>
static MDFN_FASTCALL void SoundCPU_BusWrite(uint32 A, T V);

template<bool TA_STV>
static MDFN_FASTCALL void SoundCPU_BusRMW(uint32 A, uint8 (MDFN_FASTCALL *cb)(M68K*, uint8));
static MDFN_FASTCALL unsigned SoundCPU_BusIntAck(uint8 level);
static MDFN_FASTCALL void SoundCPU_BusRESET(bool state);
//
//
void SOUND_SetMIDIOutput(void (*p)(uint8))
{
 MIDI_Out = p;
}

void SOUND_Init(bool stv_mapping)
{
 memset(IBuffer, 0, sizeof(IBuffer));
 IBufferCount = 0;

 last_rate = -1;

 run_until_time = 0;
 next_scsp_time = 0;
 lastts = 0;

 MIDI_Out = nullptr;

#if MDFN_SS_SCSP_DSP_JIT
 {
  // MDFN_SS_DSPJIT=verify|off|fuzz[:N] for debugging; the setting is the normal control.
  const char* dj = getenv("MDFN_SS_DSPJIT");
#ifdef MDFN_SSFPLAY_COMPILE
  bool enable = true;
#else
  bool enable = MDFN_GetSettingB("ss.scsp.dsp_jit");
#endif
  bool verify = false;
  if(dj)
  {
   if(!strcmp(dj, "off"))         enable = false;
   else if(!strcmp(dj, "verify")) verify = true;
   else if(!strncmp(dj, "fuzz", 4))
   {
    const unsigned n = (dj[4] == ':') ? (unsigned)atoi(dj + 5) : 2000;
    SS_SCSP::DSPJIT_Fuzz(n, 1);
   }
  }
  SCSP.SetDSPJIT(enable, verify);
 }
#endif

#ifndef MDFN_SSFPLAY_COMPILE
 {
  SThreadWanted = MDFN_GetSettingB("ss.sound.threaded");
  const char* st = getenv("MDFN_SS_SOUND_THREAD");
  if(st)
   SThreadWanted = atoi(st) != 0;
 }
#endif

 if(stv_mapping)
 {
  SoundCPU.BusRead8 = SoundCPU_BusRead<uint8, true>;
  SoundCPU.BusRead16 = SoundCPU_BusRead<uint16, true>;

  SoundCPU.BusWrite8 = SoundCPU_BusWrite<uint8, true>;
  SoundCPU.BusWrite16 = SoundCPU_BusWrite<uint16, true>;

  SoundCPU.BusReadInstr = SoundCPU_BusReadInstr<true>;

  SoundCPU.BusRMW = SoundCPU_BusRMW<true>;
 }
 else
 {
  SoundCPU.BusRead8 = SoundCPU_BusRead<uint8, false>;
  SoundCPU.BusRead16 = SoundCPU_BusRead<uint16, false>;

  SoundCPU.BusWrite8 = SoundCPU_BusWrite<uint8, false>;
  SoundCPU.BusWrite16 = SoundCPU_BusWrite<uint16, false>;

  SoundCPU.BusReadInstr = SoundCPU_BusReadInstr<false>;

  SoundCPU.BusRMW = SoundCPU_BusRMW<false>;
 }

 SoundCPU.BusIntAck = SoundCPU_BusIntAck;
 SoundCPU.BusRESET = SoundCPU_BusRESET;

 #ifndef MDFN_SSFPLAY_COMPILE
 SoundCPU.DBG_Warning = SS_DBG_Wrap<SS_DBG_WARNING | SS_DBG_M68K>;
 SoundCPU.DBG_Verbose = SS_DBG_Wrap<SS_DBG_M68K>;
 #endif

 SS_SetPhysMemMap(0x05A00000, 0x05A7FFFF, SCSP.GetRAMPtr(), 0x80000, true);
 // TODO: MEM4B: SS_SetPhysMemMap(0x05A00000, 0x05AFFFFF, SCSP.GetRAMPtr(), 0x40000, true);
}

uint8 SOUND_PeekRAM(uint32 A)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 return ne16_rbo_be<uint8>(SCSP.GetRAMPtr(), A & 0x7FFFF);
}

void SOUND_PokeRAM(uint32 A, uint8 V)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 ne16_wbo_be<uint8>(SCSP.GetRAMPtr(), A & 0x7FFFF, V);
}

uint64 SOUND_PeekMPROG(uint32 A)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 return SCSP.PeekMPROG(A);
}

void SOUND_PokeMPROG(uint32 A, uint64 V)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 SCSP.PokeMPROG(A, V);
}

uint32 SOUND_PeekTEMPRel(uint32 A)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 return SCSP.PeekTEMPRel(A);
}

void SOUND_PokeTEMPRel(uint32 A, uint32 V)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 SCSP.PokeTEMPRel(A, V);
}

uint32 SOUND_PeekMEMS(uint32 A)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 return SCSP.PeekMEMS(A);
}

void SOUND_PokeMEMS(uint32 A, uint32 V)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 SCSP.PokeMEMS(A, V);
}


static INLINE void ResetTS_68K(void)
{
 next_scsp_time -= SoundCPU.timestamp;
 run_until_time -= (int64)SoundCPU.timestamp << 32;
 SoundCPU.timestamp = 0;
}

void SOUND_AdjustTS(const int32 delta)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 ResetTS_68K();
 //
 //
 lastts += delta;
}

static INLINE void DoReset(bool powering_up)
{
 SCSP.Reset(powering_up);
 SoundCPU.Reset(powering_up);
}

void SOUND_Reset(bool powering_up)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded) { SoundThread_FlushRun(); SoundThread_Push(SCMD_RESET, 0, powering_up); return; }
#endif
 DoReset(powering_up);
}

void SOUND_Reset68K(void)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded) { SoundThread_FlushRun(); SoundThread_Push(SCMD_RESET68K); return; }
#endif
 SoundCPU.Reset(false);
}

void SOUND_ResetSCSP(void)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded) { SoundThread_FlushRun(); SoundThread_Push(SCMD_RESETSCSP); return; }
#endif
 SCSP.Reset(false);
}

#ifndef MDFN_SSFPLAY_COMPILE
static void SoundThread_Stop(void)
{
 if(!SThread)
  return;
 SoundThread_FlushRun();
 SoundThread_Push(SCMD_EXIT);
 MThreading::Thread_Wait(SThread, nullptr);
 SThread = nullptr;
 SThreaded = false;
 MThreading::Sem_Destroy(SSem);
 SSem = nullptr;
}
#endif

void SOUND_Kill(void)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Stop();
#endif
}

void SOUND_Set68KActive(bool active)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded) { SoundThread_FlushRun(); SoundThread_Push(SCMD_SET68KACTIVE, 0, active); return; }
#endif
 SoundCPU.SetExtHalted(!active);
}

void SOUND_SetRAXActive(bool active)
{
 rax_active = active;
}

/*
** SH-2 side accesses while threaded (games poll sound RAM and the SCSP
** control block hundreds of times per frame, so waiting for the thread on
** every read would serialize the two):
**  - sound RAM (A < 0x100000) is accessed directly, like the fast-mapped
**    path already does; this also keeps SH-2 read-modify-write sequences
**    coherent, which queued writes would not;
**  - register reads are served from the live state without waiting, except
**    the MIDI input register whose read pops a FIFO;
**  - register writes are queued so they stay ordered with the RUN slices.
*/
uint16 SOUND_Read16(uint32 A)
{
 uint16 ret;

#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded && A >= 0x100000 && ((A & 0xFFE) == 0x404))
  SoundThread_Drain();
#endif
 SCSP.RW<uint16, false>(A, ret);

 return ret;
}

void SOUND_Write8(uint32 A, uint8 V)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded && A >= 0x100000) { SoundThread_FlushRun(); SoundThread_Push(SCMD_WRITE8, A, V); return; }
#endif
 SCSP.RW<uint8, true>(A, V);
}

void SOUND_Write16(uint32 A, uint16 V)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded && A >= 0x100000) { SoundThread_FlushRun(); SoundThread_Push(SCMD_WRITE16, A, V); return; }
#endif
 SCSP.RW<uint16, true>(A, V);
}

static NO_INLINE void RunSCSP(void)
{
 CDB_GetCDDA(SCSP.GetEXTSPtr());
 //
 //
 int16* const bp = IBuffer[IBufferCount];
 SCSP.RunSample(bp, MIDI_Out);
 //bp[0] = rand();
 //bp[1] = rand();
 bp[0] = (bp[0] * 27 + 16) >> 5;
 bp[1] = (bp[1] * 27 + 16) >> 5;

#ifndef MDFN_SSFPLAY_COMPILE
 if(MDFN_UNLIKELY(rax_active))
 {
  int16 rl = 0, rr = 0;
  RAX_MixSample(&rl, &rr);
  // RAX firmware output is low amplitude; boost to match SCSP levels
  int32 ml = (int32)bp[0] + (rl * 3);
  int32 mr = (int32)bp[1] + (rr * 3);
  bp[0] = (ml < -32768) ? -32768 : (ml > 32767) ? 32767 : (int16)ml;
  bp[1] = (mr < -32768) ? -32768 : (mr > 32767) ? 32767 : (int16)mr;
 }
#endif

/*
 // TODO?  Need to measure frequency response more reliably first, ideally after capacitor
 // replacement.  Should probably be controlled by a boolean setting, too.
 for(unsigned lr = 0; lr < 2; lr++)
 {
  static int32 filt[2];
  filt[lr] += (((int64)(int32)((uint32)bp[lr] << 16) - filt[lr]) * 60500) >> 16;
  bp[lr] = filt[lr] >> 16;
 }
*/

 IBufferCount = (IBufferCount + 1) & 1023;
 next_scsp_time += 256;
}

// Ratio between SH-2 clock and 68K clock (sound clock / 2)
void SOUND_SetClockRatio(uint32 ratio)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded) { SoundThread_FlushRun(); SoundThread_Push(SCMD_CLOCKRATIO, ratio); return; }
#endif
 clock_ratio = ratio;
}

/* Advances the 68K and SCSP to the given SH-2 timestamp. Runs on the sound
** thread when threaded, inline otherwise. */
static void DoRun(sscpu_timestamp_t timestamp)
{
 run_until_time += ((uint64)(timestamp - lastts) * clock_ratio);
 lastts = timestamp;
 //
 //
 MDFN_setjmp(jbuf);

 if(MDFN_LIKELY(SoundCPU.timestamp < (run_until_time >> 32)))
 {
  do
  {
   int32 next_time = std::min<int32>(next_scsp_time, run_until_time >> 32);

   SoundCPU.Run(next_time);

   if(SoundCPU.timestamp >= next_scsp_time)
    RunSCSP();
  } while(MDFN_LIKELY(SoundCPU.timestamp < (run_until_time >> 32)));
 }
 else
 {
  while(next_scsp_time < (run_until_time >> 32))
   RunSCSP();
 }
}

#ifndef MDFN_SSFPLAY_COMPILE
static int SoundThreadEntry(void* data)
{
 unsigned idle = 0;

 for(;;)
 {
  const uint32 d = SQ_Done.load(std::memory_order_relaxed);

  if(SQ_Enq.load(std::memory_order_acquire) == d)
  {
   /* Spin briefly (commands normally arrive every few microseconds), then
   ** sleep until the producer wakes us. Re-check after announcing the sleep
   ** so a push that raced with it is not missed. */
   if(++idle < 4000)
    continue;
   SThreadSleeping.store(true, std::memory_order_release);
   if(SQ_Enq.load(std::memory_order_acquire) == d)
    MThreading::Sem_TimedWait(SSem, 1);
   SThreadSleeping.store(false, std::memory_order_release);
   continue;
  }
  idle = 0;

  const SoundCmd c = SQ[d & (SQ_SIZE - 1)];
  switch(c.cmd)
  {
   case SCMD_RUN: DoRun((sscpu_timestamp_t)c.arg32); break;
   case SCMD_WRITE8: { uint8 v = c.arg16; SCSP.RW<uint8, true>(c.arg32, v); } break;
   case SCMD_WRITE16: { uint16 v = c.arg16; SCSP.RW<uint16, true>(c.arg32, v); } break;
   case SCMD_RESET: DoReset(c.arg16); break;
   case SCMD_RESET68K: SoundCPU.Reset(false); break;
   case SCMD_RESETSCSP: SCSP.Reset(false); break;
   case SCMD_SET68KACTIVE: SoundCPU.SetExtHalted(!c.arg16); break;
   case SCMD_CLOCKRATIO: clock_ratio = c.arg32; break;
   case SCMD_EXIT: SQ_Done.store(d + 1, std::memory_order_release); return 0;
  }
  SQ_Done.store(d + 1, std::memory_order_release);
 }
}

static void SoundThread_Start(void)
{
 SQ_Enq.store(0); SQ_Done.store(0); SQ_DoneCached = 0;
 SPendingRun = false; SRunBatch = 0;
 SThreadSleeping.store(false);
 SMainIntEvents.store(0);
 SSem = MThreading::Sem_Create();
 SThread = MThreading::Thread_Create(SoundThreadEntry, nullptr, "MDFN SS Sound");
 SThreaded = true;

 const uint64 affinity = MDFN_GetSettingUI("ss.affinity.sound");
 if(affinity)
 {
  try { MThreading::Thread_SetAffinity(SThread, affinity); }
  catch(std::exception& e) { MDFN_printf("%s\n", e.what()); }
 }
}
#endif

sscpu_timestamp_t SOUND_Update(sscpu_timestamp_t timestamp)
{
#ifndef MDFN_SSFPLAY_COMPILE
 if(SThreaded)
 {
  SPendingRunTS = timestamp;
  SPendingRun = true;
  if(++SRunBatch >= 8)
  {
   SRunBatch = 0;
   SoundThread_FlushRun();
  }

  const unsigned ev = SMainIntEvents.exchange(0, std::memory_order_acq_rel);
  if(ev & 1)
   SCU_SetInt(SCU_INT_SCSP, true);
  if((ev & 2) && !(ev & 4))
   SCU_SetInt(SCU_INT_SCSP, false);

  return timestamp + 128;
 }
 /* The RAX cart mixes its own DSP into RunSCSP() and is driven from the main
 ** thread, so it keeps the inline path. Decided here, once the cart is up. */
 if(SThreadWanted && !SThread && !rax_active)
 {
  SoundThread_Start();
  return SOUND_Update(timestamp);
 }
#endif
 DoRun(timestamp);

 return timestamp + 128;	// FIXME
}

void SOUND_StartFrame(double rate, uint32 quality)
{
 (void)quality;
 last_rate = (int)rate;
}

int32 SOUND_FlushOutput(int16* SoundBuf, const int32 SoundBufMaxSize, const bool reverse)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 if(SoundBuf && reverse)
 {
  for(unsigned lr = 0; lr < 2; lr++)
  {
   int16* p0 = &IBuffer[0][lr];
   int16* p1 = &IBuffer[IBufferCount - 1][lr];
   unsigned count = IBufferCount >> 1;

   while(MDFN_LIKELY(count--))
   {
    std::swap(*p0, *p1);

    p0 += 2;
    p1 -= 2;
   }
  }
 }
 
 int32 ret = IBufferCount;

 if(SoundBuf)
  memcpy(SoundBuf, IBuffer, IBufferCount * 2 * sizeof(int16));
 IBufferCount = 0;

 return ret;
}

void SOUND_StateAction(StateMem* sm, const unsigned load, const bool data_only)
{
#ifndef MDFN_SSFPLAY_COMPILE
 SoundThread_Drain();
#endif
 SFORMAT StateRegs[] =
 {
  SFVAR(next_scsp_time),
  SFVAR(run_until_time),

  SFEND
 };

 //
 next_scsp_time -= SoundCPU.timestamp;
 run_until_time -= (int64)SoundCPU.timestamp << 32;

 MDFNSS_StateAction(sm, load, data_only, StateRegs, "SOUND");

 next_scsp_time += SoundCPU.timestamp;
 run_until_time += (int64)SoundCPU.timestamp << 32;
 //

 SoundCPU.StateAction(sm, load, data_only, "M68K");
 SCSP.StateAction(sm, load, data_only, "SCSP");
}

//
//
//
template<typename T, bool TA_STV>
static MDFN_FASTCALL T SoundCPU_BusRead(uint32 A)
{
 if(MDFN_UNLIKELY(A & (0xE00000 | (sizeof(T) - 1))))
 {
  SoundCPU.timestamp += 4;

  if(A & (sizeof(T) - 1))
   SoundCPU.SignalAddressError(A, 0x3);
  else if(TA_STV && !(A & 0x800000))
  {
   SS_DBG(SS_DBG_WARNING, "[M68K BUS] Unknown %u-byte read from 0x%06x\n", (unsigned)sizeof(T), A & 0xFFFFFF);

   return (T)-1;
  }
  else
   SoundCPU.SignalDTACKHalted(A);

  MDFN_longjmp(jbuf);
 }
 //
 T ret;

 SoundCPU.timestamp += 4;

 if(MDFN_UNLIKELY(SoundCPU.timestamp >= next_scsp_time))
  RunSCSP();

 SCSP.RW<T, false>(A & 0x1FFFFF, ret);

 SoundCPU.timestamp += 2;

 return ret;
}

template<bool TA_STV>
static MDFN_FASTCALL uint16 SoundCPU_BusReadInstr(uint32 A)
{
 if(MDFN_UNLIKELY(A & 0xE00001))
 {
  SoundCPU.timestamp += 4;

  if(A & 1)
   SoundCPU.SignalAddressError(A, 0x2);
  else if(TA_STV && !(A & 0x800000))
  {
   SS_DBG(SS_DBG_WARNING, "[M68K BUS] Unknown %u-byte read from 0x%06x\n", (unsigned)sizeof(uint16), A & 0xFFFFFF);

   return 0xFFFF;
  }
  else
   SoundCPU.SignalDTACKHalted(A);

  MDFN_longjmp(jbuf);
 }
 //
 uint16 ret;

 SoundCPU.timestamp += 4;

 //if(MDFN_UNLIKELY(SoundCPU.timestamp >= next_scsp_time))
 // RunSCSP();

 SCSP.RW<uint16, false>(A & 0x1FFFFF, ret);

 SoundCPU.timestamp += 2;

 return ret;
}

template<typename T, bool TA_STV>
static MDFN_FASTCALL void SoundCPU_BusWrite(uint32 A, T V)
{
 if(MDFN_UNLIKELY(A & (0xE00000 | (sizeof(T) - 1))))
 {
  SoundCPU.timestamp += 4;

  if(A & (sizeof(T) - 1))
   SoundCPU.SignalAddressError(A, 0x1);
  else if(TA_STV && !(A & 0x800000))
  {
   SS_DBG(SS_DBG_WARNING, "[M68K BUS] Unknown %u-byte write of 0x%0*x to 0x%06x\n", (unsigned)sizeof(T), (int)sizeof(T) * 2, V, A & 0xFFFFFF);

   return;
  }
  else
   SoundCPU.SignalDTACKHalted(A);

  MDFN_longjmp(jbuf);
 }
 //
 SoundCPU.timestamp += 2;

 if(MDFN_UNLIKELY(SoundCPU.timestamp >= next_scsp_time))
  RunSCSP();

 SoundCPU.timestamp += 2;

 SCSP.RW<T, true>(A & 0x1FFFFF, V);
 SoundCPU.timestamp += 2;
}

template<bool TA_STV>
static MDFN_FASTCALL void SoundCPU_BusRMW(uint32 A, uint8 (MDFN_FASTCALL *cb)(M68K*, uint8))
{
 if(MDFN_UNLIKELY(A & 0xE00000))
 {
  if(TA_STV && !(A & 0x800000))
  {
   uint8 tmp;

   SS_DBG(SS_DBG_WARNING, "[M68K BUS] Unknown RMW from/to 0x%06x\n", A & 0xFFFFFF);

   SoundCPU.timestamp += 2;
   //
   tmp = 0xFF;
   //
   tmp = cb(&SoundCPU, tmp);

   SoundCPU.timestamp += 4;
   //
   
   //
   SoundCPU.timestamp += 2;

   return;
  }

  SoundCPU.timestamp += 4;
  SoundCPU.SignalDTACKHalted(A);
  MDFN_longjmp(jbuf);
 }
 //
 uint8 tmp;

 SoundCPU.timestamp += 4;

 if(MDFN_UNLIKELY(SoundCPU.timestamp >= next_scsp_time))
  RunSCSP();

 SCSP.RW<uint8, false>(A & 0x1FFFFF, tmp);

 tmp = cb(&SoundCPU, tmp);

 SoundCPU.timestamp += 6;

 SCSP.RW<uint8, true>(A & 0x1FFFFF, tmp);

 SoundCPU.timestamp += 2;
}

static MDFN_FASTCALL unsigned SoundCPU_BusIntAck(uint8 level)
{
 SoundCPU.timestamp += 10;

 return M68K::BUS_INT_ACK_AUTO;
}

static MDFN_FASTCALL void SoundCPU_BusRESET(bool state)
{
 //SS_DBG(SS_DBG_WARNING, "[M68K] RESET: %d @ time %d\n", state, SoundCPU.timestamp);
 if(state)
 {
  SoundCPU.Reset(false);
 }
}

uint32 SOUND_GetSCSPRegister(const unsigned id, char* const special, const uint32 special_len)
{
 return SCSP.GetRegister(id, special, special_len);
}

void SOUND_SetSCSPRegister(const unsigned id, const uint32 value)
{
 SCSP.SetRegister(id, value);
}

uint32 SOUND_GetM68KRegister(const unsigned id, char* const special, const uint32 special_len)
{
 return SoundCPU.GetRegister(id, special, special_len);
}

void SOUND_SetM68KRegister(const unsigned id, const uint32 value)
{
 SoundCPU.SetRegister(id, value);
}



#ifndef MDFN_SSFPLAY_COMPILE
// Entry point for tests/dspjit_fuzz_test.cpp (headless, no frontend).
unsigned SOUND_DSPJIT_Fuzz(unsigned iterations, unsigned seed, uint64 clear_mask, uint64 set_mask)
{
 return SS_SCSP::DSPJIT_Fuzz(iterations, seed, clear_mask, set_mask);
}
#endif

}
