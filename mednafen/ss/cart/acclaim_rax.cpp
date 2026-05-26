/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* acclaim_rax.cpp - Acclaim RAX Sound Board LLE
**  Copyright (C) 2024 Mednafen Team
**
** Low-level emulation of the Acclaim RAX board (Batman Forever STV).
** Runs the actual ADSP-2181 firmware (350snda1.u52) and feeds stereo PCM
** from the SPORT0 autobuffer ring to the Saturn audio mixer.
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
*/

#include "common.h"
#include "acclaim_rax.h"
#include "adsp2181.h"

/* Per-second ring-write diagnostics from adsp2181.cpp */
extern uint32_t g_ring_total_per_sec;
extern uint32_t g_ring_nz_per_sec;
extern uint32_t g_ring_nz_pc[8];
extern uint32_t g_ring_nz_pc_cnt;
extern int      g_mix_trace_armed;
extern uint32_t g_mix_trace_budget;
extern uint32_t g_synth2e90_per_sec;
extern uint32_t g_copy008c_per_sec;
extern uint32_t g_mainloop_per_sec;
extern uint32_t g_synth26ba_per_sec;
extern uint32_t g_syngate_cmd_per_sec;
extern uint32_t g_pm_ring_write_min;
extern uint32_t g_pm_ring_write_max;

namespace MDFN_IEN_SS
{

/* ── ROM ─────────────────────────────────────────────────────────────────── */

static uint8* RAX_ROM = nullptr;

/* ── ADSP-2181 CPU ───────────────────────────────────────────────────────── */

static ADSP2181 rax_cpu;

/* ── Control register indices (DM addresses 0x3FE0-0x3FFF) ──────────────── */

enum
{
 IDMA_CONTROL_REG    = 0,   /* 3fe0 */
 BDMA_INT_ADDR_REG   = 1,   /* 3fe1 */
 BDMA_EXT_ADDR_REG   = 2,   /* 3fe2 */
 BDMA_CONTROL_REG    = 3,   /* 3fe3 */
 BDMA_WORD_COUNT_REG = 4,   /* 3fe4 */
 PROG_FLAG_DATA_REG  = 5,   /* 3fe5 */
 PROG_FLAG_CTRL_REG  = 6,   /* 3fe6 */
 /* 7-14 unused */
 S1_AUTOBUF_REG      = 15,  /* 3fef */
 S1_RFSDIV_REG       = 16,  /* 3ff0 */
 S1_SCLKDIV_REG      = 17,  /* 3ff1 */
 S1_CONTROL_REG      = 18,  /* 3ff2 */
 S0_AUTOBUF_REG      = 19,  /* 3ff3 */
 S0_RFSDIV_REG       = 20,  /* 3ff4 */
 S0_SCLKDIV_REG      = 21,  /* 3ff5 */
 S0_CONTROL_REG      = 22,  /* 3ff6 */
 S0_MCTXLO_REG       = 23,  /* 3ff7 */
 S0_MCTXHI_REG       = 24,  /* 3ff8 */
 S0_MCRXLO_REG       = 25,  /* 3ff9 */
 S0_MCRXHI_REG       = 26,  /* 3ffa */
 TIMER_SCALE_REG     = 27,  /* 3ffb */
 TIMER_COUNT_REG     = 28,  /* 3ffc */
 TIMER_PERIOD_REG    = 29,  /* 3ffd */
 WAITSTATES_REG      = 30,  /* 3ffe */
 SYSCONTROL_REG      = 31   /* 3fff */
};

/* ── Board state ─────────────────────────────────────────────────────────── */

static uint16 ctrl_regs[32];
static uint16 data_out_latch;   /* ADSP → host */
static uint8  adsp_snd_pf0;     /* 1 = idle/waiting, 0 = processing */
static int    rom_bank;
static int    data_bank;

/* Command FIFO: SH-2 pushes commands faster than the ADSP can read them
 * (both cmd[0]=0x80E6 and cmd[1]=0x2328 arrive within the same 378-cycle
 * window); a single latch causes cmd[0] to be overwritten before the ADSP
 * ISR fires. */
#define CMD_FIFO_SIZE 8
static uint16_t cmd_fifo[CMD_FIFO_SIZE];
static int      cmd_fifo_rd;
static int      cmd_fifo_wr;

/* ── Audio ring buffer state (set by SPORT0 TX callback) ─────────────────── */

static bool    audio_valid;
static int     audio_ireg;      /* I-reg index carrying the ring pointer */
static int32_t audio_incs;      /* M-reg stride between DM samples        */
static int32_t audio_size;      /* L-reg: ring buffer size in DM words     */
static uint32_t audio_base;     /* base DM address of the ring             */

/* Output FIFO: SPORT0 TX samples are pushed here; RAX_MixSample pops L+R pairs */
#define AUDIO_FIFO_SIZE 4096
static int16_t audio_fifo[AUDIO_FIFO_SIZE];
static int     audio_fifo_wr;
static int     audio_fifo_rd;

static uint32_t sport_tx_call_count;  /* diagnostic: incremented each sport_tx_cb call */
static uint32_t ring_write_count;     /* diagnostic: CPU writes to 0x3000-0x33FF */
static uint32_t ring_last_addr;       /* last address written in ring range */
static uint16_t ring_last_val;        /* last value written in ring range */
static uint32_t sport_tx_countdown;   /* samples until next SPORT0_TX IRQ pulse */

/* dm_upper write tracker (per-second) */
static uint32_t dmu_wr_count;
static uint32_t dmu_wr_min;    /* min DM address written this second */
static uint32_t dmu_wr_max;    /* max DM address written this second */
static uint32_t dmu_wr_last;   /* last DM address written this second */
static uint16_t dmu_wr_lval;   /* last value written */

/* ── ADSP clock / cycles per output sample ───────────────────────────────── */

/* 16,670,000 Hz ADSP / 44,100 Hz audio ≈ 378 cycles/sample */
static const int ADSP_CYCLES_PER_SAMPLE = 378;

/* ── ROM file table ──────────────────────────────────────────────────────── */

struct RaxROMFile
{
 const char* fname;
 uint32      offset;
 uint32      size;
};

static const RaxROMFile rax_rom_files[] =
{
 { "350snda1.u52", 0x000000, 0x080000 },
 { "snd0.u48",     0x400000, 0x200000 },
 { "snd1.u49",     0x600000, 0x200000 },
 { "snd2.u50",     0x800000, 0x200000 },
 { "snd3.u51",     0xa00000, 0x200000 },
};

/* ── DM helpers ──────────────────────────────────────────────────────────── */

static inline uint16_t dm_read_addr(uint32_t addr)
{
 addr &= 0x3FFF;
 uint16_t val;
 if(addr < 0x2000)
  val = rax_cpu.dm_active_bank[addr];
 else if(addr < 0x3FE0)
  val = rax_cpu.dm_upper[addr - 0x2000];
 else
  val = 0;
 if(addr == 0x3467) {
  static uint32_t rd3467_cnt = 0;
  if(rd3467_cnt++ < 64)
   fprintf(stderr, "[RAX] DM[3467] RD val=%04X PC=%04X (#%u)\n",
           val, (unsigned)rax_cpu.pc, rd3467_cnt);
 }
 return val;
}

static inline void dm_write_addr(uint32_t addr, uint16_t val)
{
 addr &= 0x3FFF;
 if(addr >= 0x3000 && addr < 0x3400) {
  ring_write_count++;
  ring_last_addr = addr;
  ring_last_val  = val;
 }
 /* trace first 64 DM writes from synthesis PC range [0x2600..0x2900] */
 {
  uint32_t pc = (uint32_t)rax_cpu.pc;
  if(pc >= 0x2600 && pc < 0x2900) {
   static uint32_t syn_dm_cnt = 0;
   if(syn_dm_cnt++ < 64)
    fprintf(stderr, "[SYN_DM] DM[%04X]=%04X PC=%04X (#%u)\n",
            addr, val, pc, syn_dm_cnt);
  }
 }
 /* trace writes to DM[0x3467] (command-queue sentinel) */
 if(addr == 0x3467) {
  static uint32_t wr3467_cnt = 0;
  if(wr3467_cnt++ < 64)
   fprintf(stderr, "[RAX] DM[3467] WR val=%04X PC=%04X (#%u)\n",
           val, (unsigned)rax_cpu.pc, wr3467_cnt);
 }
 /* trace any write to DM[0x1FE5] (voice active counter) */
 if(addr == 0x1FE5) {
  static uint32_t fe5_wr_count = 0;
  if(fe5_wr_count++ < 32)
   fprintf(stderr, "[RAX] DM[1FE5] write val=%04X PC=%04X bank=%d\n",
           val, (unsigned)rax_cpu.pc, rax_cpu.dm_active_bank_idx);
 }
 /* unconditional trace for writes to DM[0x3BDA..0x3BDE] (voice state cluster) */
 if(addr == 0x3BDC || addr == 0x3BDB || addr == 0x3BDE || addr == 0x3BDA) {
  static uint32_t rax_bdc_wr = 0;
  rax_bdc_wr++;
  fprintf(stderr, "[RAX_BDC] DM[%04X]=%04X PC=%04X (#%u)\n",
          addr, val, (unsigned)rax_cpu.pc, rax_bdc_wr);
 }
 if(addr < 0x2000)
  rax_cpu.dm_active_bank[addr] = val;
 else if(addr < 0x3FE0) {
  rax_cpu.dm_upper[addr - 0x2000] = val;
  /* track per-second write range in dm_upper */
  dmu_wr_count++;
  if(addr < dmu_wr_min) dmu_wr_min = addr;
  if(addr > dmu_wr_max) dmu_wr_max = addr;
  dmu_wr_last = addr;
  dmu_wr_lval = val;
 }
}

/* ── Control register write (forward declared for io callback) ───────────── */

static void rax_ctrl_write(ADSP2181* cpu, uint32_t offset, uint16_t data);

/* ── IO callbacks ────────────────────────────────────────────────────────── */

static uint16_t rax_io_read(ADSP2181* cpu, uint32_t addr)
{
 if(addr < 0x800)
 {
  /* IO port 3 = host_r: dequeue one command word, clear IRQL0 unconditionally.
   * Matches MAME rax.cpp host_r: CLEAR_LINE on every read.
   * If more commands remain in the FIFO, re-assert IRQL0 so the ISR fires
   * again after RTI (safe because imask masks IRQL0 during ISR execution). */
  if(addr == 3)
  {
   uint16_t val = 0;
   if(cmd_fifo_rd != cmd_fifo_wr)
   {
    val = cmd_fifo[cmd_fifo_rd];
    cmd_fifo_rd = (cmd_fifo_rd + 1) % CMD_FIFO_SIZE;
   }
   { static uint32_t deq_cnt = 0;
     if(deq_cnt++ < 32)
      fprintf(stderr, "[RAX] host_r[%u] val=0x%04X remain=%d\n",
              deq_cnt-1, val,
              (cmd_fifo_wr - cmd_fifo_rd + CMD_FIFO_SIZE) % CMD_FIFO_SIZE); }
   /* Always clear IRQL0 on read (MAME model) */
   ADSP2181_SetIRQ(cpu, ADSP2181_IRQL0, 0);
   if(cmd_fifo_rd != cmd_fifo_wr)
   {
    /* More commands pending: re-assert IRQL0 so ISR fires again after RTI.
     * imask is cleared during ISR execution, preventing immediate re-entry. */
    ADSP2181_SetIRQ(cpu, ADSP2181_IRQL0, 1);
   }
   /* MAME host_r: does NOT change pf0 — command read does not affect output-ready status */
   return val;
  }
 }
 else
 {
  /* DM control registers routed here with offset = addr - 0x800 */
  uint32_t off = addr - 0x800;
  if(off < 32)
  {
   if(off == PROG_FLAG_DATA_REG)
    return adsp_snd_pf0;
   return ctrl_regs[off];
  }
 }
 return 0;
}

static void rax_io_write(ADSP2181* cpu, uint32_t addr, uint16_t data)
{
 static uint32_t io_wr_count = 0;
 io_wr_count++;
 /* Always log non-BDMA writes and any write to host/prog-flag addresses */
 if(io_wr_count <= 64 || (addr != 0x0801 && addr != 0x0802 && addr != 0x0803 && addr != 0x0804))
  fprintf(stderr, "[RAX] io_write addr=%04X data=%04X PC=%04X cnt=%u\n", addr, data, (unsigned)cpu->pc, io_wr_count);
 if(addr < 0x800)
 {
  switch(addr)
  {
   case 0:  /* ram_bank_w */
    data_bank = data & 3;
    ADSP2181_UpdateDataBank(cpu, (int)cpu->dmovlay, data_bank);
    break;
   case 1:  /* rom_bank_w */
    rom_bank = (int)data;
    break;
   case 3:  /* host_w: ADSP writes result to output latch */
    data_out_latch = data;
    /* Batman Forever SH-2 never calls data_r() (no read handler in stv.cpp),
     * so do NOT clear pf0 here — ADSP must always see pf0=1 to continue */
    break;
  }
 }
 else
 {
  uint32_t off = addr - 0x800;
  if(off < 32)
   rax_ctrl_write(cpu, off, data);
 }
}

/* ── DMOVLAY callback ────────────────────────────────────────────────────── */

static void rax_dmovlay(ADSP2181* cpu, uint32_t val)
{
 ADSP2181_UpdateDataBank(cpu, (int)val, data_bank);
}

/* ── SPORT0 TX callback ──────────────────────────────────────────────────── */

static void rax_sport_tx(ADSP2181* cpu, int sport, uint32_t data)
{
 if(sport != 0)
  return;

 /* SPORT0 must be enabled in SYSCONTROL (bit 12) */
 if(!(ctrl_regs[SYSCONTROL_REG] & 0x1000))
  goto disable;

 /* Autobuffer must be enabled (bit 1 of S0_AUTOBUF_REG) */
 if(!(ctrl_regs[S0_AUTOBUF_REG] & 0x0002))
  goto disable;

 {
  /* Capture ring metadata for diagnostics (do NOT modify cpu->i[]) */
  audio_ireg = (ctrl_regs[S0_AUTOBUF_REG] >> 9) & 7;
  int mreg   = (ctrl_regs[S0_AUTOBUF_REG] >> 7) & 3;
  mreg      |= (audio_ireg & 4);
  audio_incs = cpu->m[mreg];
  audio_size = (int32_t)cpu->l[audio_ireg];
  audio_base = cpu->base[audio_ireg];
  audio_valid = true;

  sport_tx_call_count++;

  /* Push the transmitted PCM word into the output FIFO */
  int next = (audio_fifo_wr + 1) % AUDIO_FIFO_SIZE;
  if(next != audio_fifo_rd)
  {
   audio_fifo[audio_fifo_wr] = (int16_t)(uint16_t)data;
   audio_fifo_wr = next;
  }
  return;
 }

disable:
 audio_valid = false;
}

/* ── Control register write implementation ───────────────────────────────── */

static void rax_ctrl_write(ADSP2181* cpu, uint32_t offset, uint16_t data)
{
 static uint32_t ctrl_wr_count = 0;
 if(ctrl_wr_count++ < 256)
  fprintf(stderr, "[RAX] ctrl[%u]=%04X PC=%04X\n", offset, data, (unsigned)cpu->pc);
 ctrl_regs[offset] = data;

 switch(offset)
 {
  case BDMA_INT_ADDR_REG:
   ctrl_regs[offset] = data & 0x3FFF;
   break;

  case BDMA_EXT_ADDR_REG:
   ctrl_regs[offset] = data & 0x3FFF;
   break;

  case BDMA_CONTROL_REG:
   ctrl_regs[offset] = data & 0xFF0F;
   break;

  case BDMA_WORD_COUNT_REG:
  {
   ctrl_regs[offset] = data & 0x3FFF;

   if(!RAX_ROM)
    break;

   /* ADSP-2181 BDMA address = rom_bank*0x400000 + (page<<14) | ext[13:0].
    * rom_bank (set via IO[1]) selects a 4MB window in the 16MB ROM region:
    *   bank=0 → 0x000000: 350snda1.u52 (program ROM, 512KB)
    *   bank=1 → 0x400000: snd0+snd1 sample data
    *   bank=2 → 0x800000: snd2+snd3 sample data
    * BDMAC bits: [1:0]=BTYPE (00=PM 24-bit, 01=DM 16-bit, 10=DM hi-byte, 11=DM lo-byte)
    *             [2]=BDIR (0=ext→int, 1=int→ext), [3]=BRESET, [15:8]=BPAGE */
   uint8* rom   = RAX_ROM;
   uint32_t page  = (ctrl_regs[BDMA_CONTROL_REG] >> 8) & 0xFF;
   uint32_t type  = ctrl_regs[BDMA_CONTROL_REG] & 3;
   uint32_t dir   = (ctrl_regs[BDMA_CONTROL_REG] >> 2) & 1;
   /* Local offset within the current rom_bank window (used for register writeback) */
   uint32_t src_local = ((uint32_t)page << 14) | ctrl_regs[BDMA_EXT_ADDR_REG];
   /* Absolute byte address in RAX_ROM (matches MAME: adsp_rom = m_rom + rom_bank*0x400000) */
   uint32_t src   = (uint32_t)rom_bank * 0x400000 + src_local;
   uint32_t dst   = ctrl_regs[BDMA_INT_ADDR_REG];
   uint32_t count = ctrl_regs[BDMA_WORD_COUNT_REG];
   uint32_t orig_count = count;

   if(!dir)
   {
    if(type == 0)      /* ROM → PM (24-bit words) */
    {
     while(count--)
     {
      uint32_t rs = src % RAX_ROM_SIZE;
      uint32_t word = ((uint32_t)rom[rs] << 16) | ((uint32_t)rom[rs+1] << 8) | rom[rs+2];
      if(dst < ADSP2181_PM_SIZE)
       cpu->pm[dst] = word;
      src += 3; dst++;
     }
    }
    else if(type == 1) /* ROM → DM (16-bit words) */
    {
     /* log first 16 streaming BDMAs with bank/dmovlay state and first sample word */
     {
      static uint32_t sbd_cnt = 0;
      if(sbd_cnt < 16) {
       uint32_t rs0 = src % RAX_ROM_SIZE;
       uint16_t w0 = ((uint16_t)rom[rs0] << 8) | rom[rs0+1];
       fprintf(stderr, "[SBD] #%u dst=%04X cnt=%u bank=%d dmovlay=%u first_word=%04X\n",
               ++sbd_cnt, (unsigned)dst, (unsigned)count,
               rax_cpu.dm_active_bank_idx, (unsigned)rax_cpu.dmovlay, w0);
      }
     }
     while(count--)
     {
      uint32_t rs = src % RAX_ROM_SIZE;
      uint16_t word = ((uint16_t)rom[rs] << 8) | rom[rs+1];
      dm_write_addr(dst, word);
      src += 2; dst++;
     }
    }
    else               /* ROM → DM (byte, shifted) */
    {
     int shift = (type == 2) ? 8 : 0;
     while(count--)
     {
      uint32_t rs = src % RAX_ROM_SIZE;
      dm_write_addr(dst, (uint16_t)rom[rs] << shift);
      src++; dst++;
     }
    }
   }
   else /* dir=1: int→ext (DM → ROM) */
   {
    /* MAME notes "last stage in Batman Forever!? page=0, dir=1, type=1, src_addr=0xfd".
     * Transfer DM[BIAD..] → ROM[ext_addr..] so the in-memory ROM reflects any
     * DSP-computed sample data before a subsequent ext→int read uses it. */
    if(type == 1) /* DM → ROM (16-bit words) */
    {
     while(count--)
     {
      uint16_t word = dm_read_addr(dst);
      uint32_t rs = src % RAX_ROM_SIZE;
      rom[rs]   = (uint8_t)(word >> 8);
      rom[rs+1] = (uint8_t)(word & 0xFF);
      src += 2; dst++;
     }
    }
    else
    {
     fprintf(stderr, "[RAX] BDMA dir=1 type=%u unimplemented! rom_bank=%d page=%u ext=0x%07X int=%04X cnt=%u\n",
             (unsigned)type, rom_bank, (unsigned)page, (unsigned)src, (unsigned)dst, (unsigned)count);
    }
   }

   /* Update BDMA state registers after transfer (matches MAME rax.cpp behaviour).
    * BIAD auto-increments on every word written (MAME does ++m_control_regs[BDMA_INT_ADDR_REG]
    * inside the loop); net result is BIAD = initial_dst + count after transfer. */
   uint32_t src_local_end = src - (uint32_t)rom_bank * 0x400000;
   ctrl_regs[BDMA_INT_ADDR_REG]  = (uint16_t)(dst & 0x3FFF);
   ctrl_regs[BDMA_WORD_COUNT_REG] = 0;
   ctrl_regs[BDMA_EXT_ADDR_REG]   = (uint16_t)(src_local_end & 0x3FFF);
   ctrl_regs[BDMA_CONTROL_REG]    = (ctrl_regs[BDMA_CONTROL_REG] & ~0xFF00) |
                                     (uint16_t)(((src_local_end >> 14) & 0xFF) << 8);

   fprintf(stderr, "[RAX] BDMA: type=%u dir=%u rom_bank=%d page=%u ext=%04X dst_end=0x%04X cnt=%u abs_end=0x%07X\n",
           (unsigned)type, (unsigned)dir, rom_bank, (unsigned)page, (unsigned)(src_local_end & 0x3FFF),
           ctrl_regs[BDMA_INT_ADDR_REG], (unsigned)orig_count, (unsigned)src);

   /* Bit 3 of BDMA_CONTROL_REG: reset CPU after BDMA, else fire BDMA IRQ.
    * Pass nullptr so PM is NOT reloaded — only CPU registers are reset.
    * (Matches MAME: pulse_input_line(RESET) calls adsp device_reset which
    * only resets registers; PM is preserved so the BDMA-loaded code runs.) */
   if(ctrl_regs[BDMA_CONTROL_REG] & 0x0008)
    ADSP2181_Reset(cpu, nullptr);
   else {
    /* Mimic MAME's pulse_input_line(ADSP2181_BDMA, minimum_quantum_time):
     * assert sets the latch (rising edge), deassert resets m_irq_state so
     * the next BDMA also gets a fresh 0→1 rising edge. */
    ADSP2181_SetIRQ(cpu, ADSP2181_BDMA, 1);
    ADSP2181_SetIRQ(cpu, ADSP2181_BDMA, 0);
   }
   break;
  }

  case PROG_FLAG_DATA_REG:
   /* MAME ignores PFLAGS writes — pf0 is managed only via host_w/data_r.
    * Letting the firmware set pf0=0 here causes a deadlock: the firmware
    * later polls DM[0x3FE5] waiting for pf0=1, but it never recovers. */
   break;

  case S0_AUTOBUF_REG:
   if(!(data & 0x0002))
    audio_valid = false;
   break;

  default:
   break;
 }
}

/* ── Init / Kill ─────────────────────────────────────────────────────────── */

void RAX_Init(VirtualFS* vfs, const std::string& dir)
{
 /* Redirect stderr to a dedicated trace file so output is not swallowed by retroarch */
 freopen("/tmp/rax_trace.txt", "w", stderr);
 setvbuf(stderr, nullptr, _IOLBF, 0);

 RAX_ROM = new uint8[RAX_ROM_SIZE];
 memset(RAX_ROM, 0xFF, RAX_ROM_SIZE);

 bool any_loaded = false;

 for(const auto& rf : rax_rom_files)
 {
  const std::string fpath = vfs->eval_fip(dir, rf.fname);

  try
  {
   std::unique_ptr<Stream> s(vfs->open(fpath, VirtualFS::MODE_READ));

   const uint64 fsize = s->size();
   if(fsize != rf.size)
   {
    MDFN_printf(_("[RAX] Warning: %s is %llu bytes, expected %u — skipping.\n"),
                rf.fname, (unsigned long long)fsize, rf.size);
    continue;
   }

   s->read(RAX_ROM + rf.offset, rf.size);
   any_loaded = true;
   MDFN_printf(_("[RAX] Loaded %s at 0x%06X (%u KB)\n"),
               rf.fname, rf.offset, rf.size >> 10);
  }
  catch(...)
  {
   MDFN_printf(_("[RAX] Warning: could not open %s — audio will be silent.\n"),
               rf.fname);
  }
 }

 if(!any_loaded)
  MDFN_printf(_("[RAX] No RAX ROM files found — audio will be silent.\n"));

 /* ── Initialise the ADSP-2181 ── */
 ADSP2181_Init(&rax_cpu);

 rax_cpu.sport_tx_cb  = rax_sport_tx;
 rax_cpu.sport_rx_cb  = nullptr;
 rax_cpu.dmovlay_cb   = rax_dmovlay;
 rax_cpu.io_read_cb   = rax_io_read;
 rax_cpu.io_write_cb  = rax_io_write;

 RAX_Reset();
}

void RAX_Kill()
{
 if(RAX_ROM)
 {
  delete[] RAX_ROM;
  RAX_ROM = nullptr;
 }
}

/* ── Reset ───────────────────────────────────────────────────────────────── */

void RAX_Reset()
{
 memset(ctrl_regs,    0, sizeof(ctrl_regs));
 memset(cmd_fifo,     0, sizeof(cmd_fifo));
 cmd_fifo_rd    = 0;
 cmd_fifo_wr    = 0;
 data_out_latch = 0;
 adsp_snd_pf0   = 1;   /* idle: waiting for first command */
 rom_bank       = 0;
 data_bank      = 0;

 audio_valid    = false;
 audio_ireg     = 0;
 audio_incs     = 0;
 audio_size     = 0;
 audio_base     = 0;
 audio_fifo_wr  = 0;
 audio_fifo_rd  = 0;
 sport_tx_call_count  = 0;
 ring_write_count     = 0;
 ring_last_addr       = 0;
 ring_last_val        = 0;
 sport_tx_countdown   = 0;
 dmu_wr_count         = 0;
 dmu_wr_min           = 0xFFFF;
 dmu_wr_max           = 0;
 dmu_wr_last          = 0;
 dmu_wr_lval          = 0;

 if(RAX_ROM)
 {
  ADSP2181_Reset(&rax_cpu, RAX_ROM);
  /* DM[0x3467] holds I6, the command-queue write pointer saved across IRQL0 ISRs.
   * Firmware assumes it starts at 0x3400 (base of the 100-word circular buffer).
   * Real ADSP-2181 DM RAM is not zeroed on reset; we must initialize explicitly. */
  rax_cpu.dm_upper[0x3467 - 0x2000] = 0x3400;
 }
}

/* ── SH-2 read/write handlers ────────────────────────────────────────────── */

void RAX_Read16(uint32 A, uint16* DB)
{
 static uint32_t rd_count = 0;
 /* A&2==0: status word (bit 0 = adsp_snd_pf0: 1=idle/ready, 0=busy)  [0x04000000]
  * A&2==2: response data latched by ADSP via io_write addr=3          [0x04000002] */
 if(A & 2) {
  *DB = data_out_latch;
  adsp_snd_pf0 = 1;  /* MAME data_r: host consumed output, ADSP may write again */
 }
 else
  *DB = (uint16)adsp_snd_pf0;
 if(rd_count++ < 32)
  fprintf(stderr, "[STV] RAX_Read16 A=%08X DB=%04X pf0=%u cnt=%u\n",
          (unsigned)A, (unsigned)*DB, (unsigned)adsp_snd_pf0, rd_count);
}

void RAX_WriteCommand(uint16 data)
{
 static uint32_t cmd_count = 0;
 int next = (cmd_fifo_wr + 1) % CMD_FIFO_SIZE;
 if(next != cmd_fifo_rd)
 {
  cmd_fifo[cmd_fifo_wr] = data;
  cmd_fifo_wr = next;
 }
 /* Arm the mixing-loop DM-read trace on the first command */
 if(!g_mix_trace_armed) {
  g_mix_trace_armed = 1;
  g_mix_trace_budget = 512;
  fprintf(stderr, "[RAX] MIX_RD trace armed (first command=0x%04X)\n", (unsigned)data);
 }
 /* MAME data_w: does NOT change pf0 — host sending a command doesn't affect output-ready status */
 ADSP2181_SetIRQ(&rax_cpu, ADSP2181_IRQL0, 1);
 if(cmd_count < 64)
  fprintf(stderr, "[RAX] cmd[%u]=0x%04X PC=%04X irq_state=%u\n",
          cmd_count, (unsigned)data, (unsigned)rax_cpu.pc,
          (unsigned)rax_cpu.irq_state[ADSP2181_IRQL0]);
 cmd_count++;
}

/* ── Audio mixing ────────────────────────────────────────────────────────── */

void RAX_MixSample(int16* left, int16* right)
{
 /* Fire SPORT0_TX at the start of each sample period.
  * This mimics the MAME hardware timer that fires based on SCLKDIV:
  * when the serial port needs a new word, it asserts SPORT0_TX IRQ.
  * The firmware ISR at 0x0010 runs a DO UNTIL loop and (when synthesis is
  * active) jumps via PM[0x001C] to the synthesis routine at 0x26BA.
  * Without this pulse, SPORT0_TX never fires and synthesis is silent.
  * IRQL0 is NOT pulsed here — it is only asserted when the SH-2 writes a
  * command (in RAX_WriteCommand) and cleared when the ADSP reads it back. */
 /* Pulse SPORT0_TX low then high to produce a rising edge each sample.
  * SetIRQ only latches when transitioning 0→1; staying at 1 never re-fires. */
 ADSP2181_SetIRQ(&rax_cpu, ADSP2181_SPORT0_TX, 0);
 ADSP2181_SetIRQ(&rax_cpu, ADSP2181_SPORT0_TX, 1);
 ADSP2181_Run(&rax_cpu, ADSP_CYCLES_PER_SAMPLE);

 static bool pm_dumped = false;
 if(!pm_dumped && rax_cpu.pc > 0x100) {
  pm_dumped = true;
  fprintf(stderr, "[RAX] Phase1 BDMA boot complete (PC=0x%04X)\n", (unsigned)rax_cpu.pc);
  /* Synthesis only writes PM[0x31A3..0x33A2] (CNTR=256, I4 reset each call).
   * PM[0x3000..0x31A2] retains BDMA-loaded firmware code. Zero it so that
   * when I7 wraps into that range, we read 0 instead of noisy opcodes. */
  for(int j = 0x3000; j < 0x31A3; j++) rax_cpu.pm[j] = 0;
  /* Interrupt vector table PM[0x00..0x2F] */
  fprintf(stderr, "[RAX] Interrupt vector table:\n");
  for(int i = 0; i < 0x30; i += 4)
   fprintf(stderr, "  PM[%02X..%02X]: %06X %06X %06X %06X\n",
           i, i+3, rax_cpu.pm[i], rax_cpu.pm[i+1], rax_cpu.pm[i+2], rax_cpu.pm[i+3]);
  fprintf(stderr, "[RAX] IRQL0 vector(PM[0C])=%06X PM[0D]=%06X\n",
          rax_cpu.pm[0x0C], rax_cpu.pm[0x0D]);
  /* ISR dispatch area PM[0x2142..0x215A] */
  fprintf(stderr, "[RAX] ISR dispatch PM[2142..215A]:\n");
  for(int i = 0x2142; i <= 0x215A; i++)
   fprintf(stderr, "  PM[%04X]=%06X\n", i, rax_cpu.pm[i]);
  uint32_t pm_nz = 0;
  for(int i = 0; i < 0x4000; i++) if(rax_cpu.pm[i]) pm_nz++;
  fprintf(stderr, "[RAX] Total non-zero PM words: %u / 16384\n", pm_nz);
 }

 /* There are exactly 2 BDMA boot cycles (Phase1 → Phase2 init → Phase3 → Phase3 init).
  * Each init sets IMASK=0x00C0 at the end. ADSP2181_Reset clears IMASK back to 0 between cycles.
  * Phase3 BDMA includes the same entry that writes DM[0x3BD7]=0 (silence gate).
  * So we track IMASK rising transitions (non-0x00C0 → 0x00C0) and only force DM[0x3BD7]=1
  * on the 2nd transition (= end of Phase3 init, last boot cycle). */
 static uint16_t prev_imask = 0;
 static int imask_c0_transitions = 0;
 static bool phase_final_forced = false;
 if(pm_dumped && rax_cpu.imask == 0x00C0 && prev_imask != 0x00C0) {
  imask_c0_transitions++;
  fprintf(stderr, "[RAX] IMASK→0x00C0 transition #%d at PC=%04X\n",
          imask_c0_transitions, (unsigned)rax_cpu.pc);
  if(!phase_final_forced && imask_c0_transitions >= 2) {
   phase_final_forced = true;
   rax_cpu.dm_upper[0x3BD7 - 0x2000] = 1;
   fprintf(stderr, "[RAX] Final init done (transition #%d): DM[0x3BD7]=1 (silence gate open)\n",
           imask_c0_transitions);
  }
 }
 prev_imask = rax_cpu.imask;

 static uint32_t mix_count = 0;
 mix_count++;
 if(mix_count == 44100)  /* log once per second */
 {
  mix_count = 0;

  /* I/L/M dump every second to track register evolution */
  fprintf(stderr, "[RAX] --- 1s tick PC=%04X bank=%d IMASK=%04X DM[3BD7]=%04X imask_trans=%d final=%d ---\n",
          (unsigned)rax_cpu.pc, (int)rax_cpu.dm_active_bank_idx, (unsigned)rax_cpu.imask,
          (unsigned)rax_cpu.dm_upper[0x3BD7 - 0x2000], imask_c0_transitions, (int)phase_final_forced);
  for(int r = 0; r < 8; r++)
   fprintf(stderr, "  I[%d]=%04X L[%d]=%04X M[%d]=%04X base[%d]=%04X\n",
           r,(unsigned)rax_cpu.i[r], r,(unsigned)rax_cpu.l[r],
           r,(unsigned)rax_cpu.m[r], r,(unsigned)rax_cpu.base[r]);

  /* SPORT/control registers */
  int ireg = (ctrl_regs[S0_AUTOBUF_REG] >> 9) & 7;
  fprintf(stderr, "[RAX] SYSCONTROL=%04X S0AUTOBUF=%04X(ireg=%d) S0CTRL=%04X S1AUTOBUF=%04X\n",
          ctrl_regs[SYSCONTROL_REG], ctrl_regs[S0_AUTOBUF_REG], ireg,
          ctrl_regs[S0_CONTROL_REG], ctrl_regs[S1_AUTOBUF_REG]);

  /* dm_upper write range this second, and SPORT TX FIFO stats */
  int fifo_used = (audio_fifo_wr - audio_fifo_rd + AUDIO_FIFO_SIZE) % AUDIO_FIFO_SIZE;
  fprintf(stderr, "[RAX] dmu_writes=%u range=[%04X..%04X] sport_tx_calls=%u fifo_used=%d\n",
          dmu_wr_count,
          (dmu_wr_count ? dmu_wr_min : 0), (dmu_wr_count ? dmu_wr_max : 0),
          sport_tx_call_count, fifo_used);
  sport_tx_call_count = 0;
  dmu_wr_count = 0; dmu_wr_min = 0xFFFF; dmu_wr_max = 0;

  /* Ring buffer write diagnostics (from adsp2181.cpp per-second counters) */
  fprintf(stderr, "[RAX] ring[3000..33FF] writes=%u nz=%u",
          g_ring_total_per_sec, g_ring_nz_per_sec);
  if (g_ring_nz_pc_cnt > 0) {
   fprintf(stderr, " nz_PCs:");
   for (uint32_t k = 0; k < g_ring_nz_pc_cnt; k++)
    fprintf(stderr, " %04X", g_ring_nz_pc[k]);
  }
  fprintf(stderr, "\n");
  g_ring_total_per_sec = 0; g_ring_nz_per_sec = 0; g_ring_nz_pc_cnt = 0;

  fprintf(stderr, "[RAX] synth2E90/s=%u synth26BA/s=%u copy008C/s=%u mainloop/s=%u syngate_cmd/s=%u\n",
          g_synth2e90_per_sec, g_synth26ba_per_sec, g_copy008c_per_sec, g_mainloop_per_sec,
          g_syngate_cmd_per_sec);
  g_synth2e90_per_sec = 0; g_synth26ba_per_sec = 0; g_copy008c_per_sec = 0; g_mainloop_per_sec = 0;
  g_syngate_cmd_per_sec = 0;

  /* Autobuffer condition check */
  {
   int ab_ireg = (ctrl_regs[S0_AUTOBUF_REG] >> 9) & 7;
   int ab_mreg = (ctrl_regs[S0_AUTOBUF_REG] >> 7) & 3;
   ab_mreg |= (ab_ireg & 4);
   int32_t ab_incs = (int32_t)rax_cpu.m[ab_mreg];
   int32_t ab_lval = (int32_t)rax_cpu.l[ab_ireg];
   uint32_t ab_i   = rax_cpu.i[ab_ireg];
   fprintf(stderr, "[RAX] autobuf: ireg=%d mreg=%d I=%04X M=%d L=%d cond=%s ringval=%04X bank_idx=%d dmovlay=%d\n",
           ab_ireg, ab_mreg, ab_i, ab_incs, ab_lval,
           (ab_incs > 0 && ab_lval > 0) ? "PASS" : "FAIL",
           dm_read_addr(ab_i),
           rax_cpu.dm_active_bank_idx, (int)rax_cpu.dmovlay);
  }

  /* Scan dm_active_bank for non-zero (ring is at banked DM[0x0200..0x05FF]) */
  fprintf(stderr, "[RAX] dm_bank[%d] non-zero pages (0x0000-0x1FFF):\n", rax_cpu.dm_active_bank_idx);
  for(int page = 0; page < 0x2000; page += 0x100) {
   uint32_t nz = 0; uint32_t fa = 0xFFFF; uint16_t fv = 0;
   for(int off = 0; off < 0x100; off++) {
    uint16_t v = rax_cpu.dm_active_bank[page + off];
    if(v) { nz++; if(fa==0xFFFF){ fa=(uint32_t)(page+off); fv=v; } }
   }
   if(nz)
    fprintf(stderr, "  page %04X: %u nz, first=%04X:%04X\n",
            (unsigned)page, nz, fa, fv);
  }

  /* Dump DM[0x1FE0..0x1FEF] for ALL banks (voice state area) */
  for(int bk = 0; bk < ADSP2181_NUM_BANKS; bk++) {
   fprintf(stderr, "[RAX] DM[1FE0..1FEF] bank%d:", bk);
   for(int a = 0x1FE0; a < 0x1FF0; a++) fprintf(stderr, " %04X", rax_cpu.dm_bank[bk][a]);
   fprintf(stderr, "\n");
  }

  /* Dump first 16 words at 0x0000 for ALL banks (may hold voice count/flags) */
  for(int bk = 0; bk < ADSP2181_NUM_BANKS; bk++) {
   bool any = false;
   for(int a = 0; a < 0x10; a++) if(rax_cpu.dm_bank[bk][a]) { any = true; break; }
   if(any) {
    fprintf(stderr, "[RAX] DM[0000..000F] bank%d:", bk);
    for(int a = 0; a < 0x10; a++) fprintf(stderr, " %04X", rax_cpu.dm_bank[bk][a]);
    fprintf(stderr, "\n");
   }
  }

  /* Dump ring area dm_bank[active][0x0200..0x020F] */
  fprintf(stderr, "[RAX] dm_bank[%d][0x0200..0x020F]:", rax_cpu.dm_active_bank_idx);
  for(int a = 0x0200; a < 0x0210; a++) fprintf(stderr, " %04X", rax_cpu.dm_active_bank[a]);
  fprintf(stderr, "\n");

  /* Scan all dm_upper pages (256-word pages) for non-zero */
  fprintf(stderr, "[RAX] dm_upper non-zero pages (0x2000-0x3FDF):\n");
  for(int page = 0; page < 0x1FE0; page += 0x100) {
   uint32_t nz = 0; uint32_t fa = 0xFFFF; uint16_t fv = 0;
   for(int off = 0; off < 0x100 && (page+off) < 0x1FE0; off++) {
    uint16_t v = rax_cpu.dm_upper[page + off];
    if(v) { nz++; if(fa==0xFFFF){ fa=(uint32_t)(page+off+0x2000); fv=v; } }
   }
   if(nz)
    fprintf(stderr, "  page %04X: %u nz, first=%04X:%04X\n",
            (unsigned)(page+0x2000), nz, fa, fv);
  }

  /* Dump DM[3000..3007] (old ring location - should be zero) */
  fprintf(stderr, "[RAX] dm[3000..3007]:");
  for(int a = 0x3000; a < 0x3008; a++) fprintf(stderr, " %04X", rax_cpu.dm_upper[a-0x2000]);
  fprintf(stderr, "\n");
 }

/* ── TEST TONE: 440 Hz square wave ──────────────────────────────────────────
 * Uncomment RAX_TEST_TONE to bypass synthesis and output a direct test tone.
 * If you hear it → the audio output path (RAX→Saturn mixer→speakers) works.
 * If silent      → the problem is upstream of RAX_MixSample.
 * Comment it out once confirmed and re-run to diagnose synthesis silence.
 * #define RAX_TEST_TONE 1 */
//#define RAX_TEST_TONE 1
#ifdef RAX_TEST_TONE
 {
  static uint32_t tone_phase = 0;
  tone_phase++;
  /* 440 Hz square wave: half-period = 44100/440/2 ≈ 50 samples */
  int16_t v = (tone_phase % 100 < 50) ? 8000 : -8000;
  *left  = v;
  *right = v;
 }
#else
 /* Read L+R directly from the SPORT0 autobuffer ring in DM, like MAME's adsp_irq().
  * S0_AUTOBUF_REG decodes: ireg = bits[11:9], mreg = bits[9:7] | (ireg & 4).
  * The firmware sets I7/M7/L7 for the ring; we advance I7 circularly per sample. */
 {
  int ab_ireg = (ctrl_regs[S0_AUTOBUF_REG] >> 9) & 7;
  int ab_mreg = (ctrl_regs[S0_AUTOBUF_REG] >> 7) & 3;
  ab_mreg |= (ab_ireg & 4);
  int32_t stride = (int32_t)rax_cpu.m[ab_mreg];
  int32_t len    = (int32_t)rax_cpu.l[ab_ireg];
  if(stride > 0 && len > 0 && (ctrl_regs[SYSCONTROL_REG] & 0x1000))
  {
   uint32_t i    = rax_cpu.i[ab_ireg];
   uint32_t base = rax_cpu.base[ab_ireg];
   auto circ_adv = [&](uint32_t addr) -> uint32_t {
    addr = (addr + (uint32_t)stride) & 0x3fff;
    if(addr < base)                          addr += (uint32_t)len;
    else if(addr >= base + (uint32_t)len)    addr -= (uint32_t)len;
    return addr;
   };
   /* Read L+R from the SPORT0 autobuffer ring.
    * Also check I1 (dag1 synthesis write ptr) in case I7 and I1 are out of phase. */
   uint32_t il = i & 0x3FFF;
   *left  = (il >= 0x2000 && il < 0x3FE0)
            ? (int16_t)rax_cpu.dm_upper[il - 0x2000] : 0;
   i = circ_adv(i);
   uint32_t ir = i & 0x3FFF;
   *right = (ir >= 0x2000 && ir < 0x3FE0)
            ? (int16_t)rax_cpu.dm_upper[ir - 0x2000] : 0;
   i = circ_adv(i);
   rax_cpu.i[ab_ireg] = i;
   /* log first 64 non-zero reads to verify audio reaches RAX_MixSample */
   if(*left != 0 || *right != 0) {
    static uint32_t nz_read_cnt = 0;
    if(nz_read_cnt++ < 64)
     fprintf(stderr, "[MIX_RD] nz#%u L=%04X R=%04X I7=%04X I1=%04X\n",
             nz_read_cnt, (uint16_t)*left, (uint16_t)*right,
             (unsigned)(il & 0x3FFF), (unsigned)(rax_cpu.i[1] & 0x3FFF));
   }
  }
 }
#endif
}

/* ── Save state ──────────────────────────────────────────────────────────── */

void RAX_StateAction(StateMem* sm, const unsigned load, const bool data_only)
{
 /* Board-level state */
 SFORMAT BoardRegs[] =
 {
  SFPTR16N(ctrl_regs,    32,         "rax_ctrl"),
  SFPTR16N(cmd_fifo,     CMD_FIFO_SIZE, "rax_cmd_fifo"),
  SFVAR(cmd_fifo_rd),
  SFVAR(cmd_fifo_wr),
  SFVAR(data_out_latch),
  SFVAR(adsp_snd_pf0),
  SFVAR(rom_bank),
  SFVAR(data_bank),
  SFVAR(audio_valid),
  SFVAR(audio_ireg),
  SFVAR(audio_incs),
  SFVAR(audio_size),
  SFVAR(audio_base),
  SFEND
 };

 MDFNSS_StateAction(sm, load, data_only, BoardRegs, "RAX");

 /* ADSP-2181 CPU state — memory arrays */
 SFORMAT CpuMem[] =
 {
  SFPTR32N(rax_cpu.pm,               ADSP2181_PM_SIZE,
           "adsp_pm"),
  SFPTR16N(&rax_cpu.dm_bank[0][0],   ADSP2181_NUM_BANKS * ADSP2181_DM_INT_WORDS,
           "adsp_dm_bank"),
  SFPTR16N(rax_cpu.dm_upper,         ADSP2181_DM_UPPER_WORDS,
           "adsp_dm_upper"),
  SFEND
 };

 MDFNSS_StateAction(sm, load, data_only, CpuMem, "RAXMEM");

 /* ADSP-2181 CPU state — registers */
 SFORMAT CpuRegs[] =
 {
  /* Core register file (live + alternate bank) */
  SFPTR8N((uint8*)&rax_cpu.core,  sizeof(adsp_core), "adsp_core"),
  SFPTR8N((uint8*)&rax_cpu.alt,   sizeof(adsp_core), "adsp_alt"),

  /* Address generation unit */
  SFPTR32N(rax_cpu.i,    8, "adsp_i"),
  SFPTR32N((uint32*)rax_cpu.m,    8, "adsp_m"),
  SFPTR32N(rax_cpu.l,    8, "adsp_l"),
  SFPTR32N(rax_cpu.base, 8, "adsp_base"),
  SFVAR(rax_cpu.px),

  /* Overlay / bank */
  SFVAR(rax_cpu.pmovlay),
  SFVAR(rax_cpu.dmovlay),

  /* CPU control */
  SFVAR(rax_cpu.pc),
  SFVAR(rax_cpu.ppc),
  SFVAR(rax_cpu.loop),
  SFVAR(rax_cpu.loop_condition),
  SFVAR(rax_cpu.cntr),
  SFVAR(rax_cpu.astat),
  SFVAR(rax_cpu.sstat),
  SFVAR(rax_cpu.mstat),
  SFVAR(rax_cpu.mstat_prev),
  SFVAR(rax_cpu.astat_clear),
  SFVAR(rax_cpu.idle),

  /* Stacks */
  SFPTR32N(rax_cpu.loop_stack, 4,  "adsp_loop_stk"),
  SFPTR32N(rax_cpu.cntr_stack, 4,  "adsp_cntr_stk"),
  SFPTR32N(rax_cpu.pc_stack,   16, "adsp_pc_stk"),
  SFPTR8N((uint8*)rax_cpu.stat_stack, 4*3*2, "adsp_stat_stk"),
  SFVAR(rax_cpu.pc_sp),
  SFVAR(rax_cpu.cntr_sp),
  SFVAR(rax_cpu.stat_sp),
  SFVAR(rax_cpu.loop_sp),

  /* Flags / IO */
  SFVAR(rax_cpu.flagout),
  SFVAR(rax_cpu.flagin),
  SFVAR(rax_cpu.fl0),
  SFVAR(rax_cpu.fl1),
  SFVAR(rax_cpu.fl2),

  /* IDMA */
  SFVAR(rax_cpu.idma_addr),
  SFVAR(rax_cpu.idma_cache),
  SFVAR(rax_cpu.idma_offs),

  /* Interrupts */
  SFVAR(rax_cpu.imask),
  SFVAR(rax_cpu.icntl),
  SFVAR(rax_cpu.ifc),
  SFPTR8N(rax_cpu.irq_state, ADSP2181_NUM_IRQ, "adsp_irq_state"),
  SFPTR8N(rax_cpu.irq_latch, ADSP2181_NUM_IRQ, "adsp_irq_latch"),

  SFVAR(rax_cpu.dm_active_bank_idx),

  SFEND
 };

 MDFNSS_StateAction(sm, load, data_only, CpuRegs, "RAXCPU");

 if(load)
 {
  /* Re-point active bank pointer after load */
  rax_cpu.dm_active_bank = rax_cpu.dm_bank[rax_cpu.dm_active_bank_idx];
 }
}

} // namespace MDFN_IEN_SS
