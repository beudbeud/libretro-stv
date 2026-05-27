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

/* Command FIFO: SH-2 pushes commands faster than the ADSP can read them. */
#define CMD_FIFO_SIZE 8
static uint16_t cmd_fifo[CMD_FIFO_SIZE];
static int      cmd_fifo_rd;
static int      cmd_fifo_wr;

/* ── Audio state ─────────────────────────────────────────────────────────── */

static bool audio_valid;

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
 if(addr < 0x2000)  return rax_cpu.dm_active_bank[addr];
 if(addr < 0x3FE0)  return rax_cpu.dm_upper[addr - 0x2000];
 return 0;
}

static inline void dm_write_addr(uint32_t addr, uint16_t val)
{
 addr &= 0x3FFF;
 if(addr < 0x2000)       rax_cpu.dm_active_bank[addr] = val;
 else if(addr < 0x3FE0)  rax_cpu.dm_upper[addr - 0x2000] = val;
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
   * If more commands remain, re-assert IRQL0 so the ISR fires again after RTI. */
  if(addr == 3)
  {
   uint16_t val = 0;
   if(cmd_fifo_rd != cmd_fifo_wr)
   {
    val = cmd_fifo[cmd_fifo_rd];
    cmd_fifo_rd = (cmd_fifo_rd + 1) % CMD_FIFO_SIZE;
   }
   ADSP2181_SetIRQ(cpu, ADSP2181_IRQL0, 0);
   if(cmd_fifo_rd != cmd_fifo_wr)
    ADSP2181_SetIRQ(cpu, ADSP2181_IRQL0, 1);
   return val;
  }
 }
 else
 {
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
    /* Batman Forever SH-2 never calls data_r(), so do NOT clear pf0 here —
     * ADSP must always see pf0=1 to continue. */
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
 (void)data;
 if(sport != 0)
  return;

 if(!(ctrl_regs[SYSCONTROL_REG] & 0x1000) || !(ctrl_regs[S0_AUTOBUF_REG] & 0x0002))
 {
  audio_valid = false;
  return;
 }
 audio_valid = true;
}

/* ── Control register write implementation ───────────────────────────────── */

static void rax_ctrl_write(ADSP2181* cpu, uint32_t offset, uint16_t data)
{
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
    * BDMAC bits: [1:0]=BTYPE (00=PM 24-bit, 01=DM 16-bit, 10=DM hi-byte, 11=DM lo-byte)
    *             [2]=BDIR (0=ext→int, 1=int→ext), [3]=BRESET, [15:8]=BPAGE */
   uint8*   rom   = RAX_ROM;
   uint32_t page  = (ctrl_regs[BDMA_CONTROL_REG] >> 8) & 0xFF;
   uint32_t type  = ctrl_regs[BDMA_CONTROL_REG] & 3;
   uint32_t dir   = (ctrl_regs[BDMA_CONTROL_REG] >> 2) & 1;
   uint32_t src   = (uint32_t)rom_bank * 0x400000
                  + ((uint32_t)page << 14)
                  + ctrl_regs[BDMA_EXT_ADDR_REG];
   uint32_t dst   = ctrl_regs[BDMA_INT_ADDR_REG];
   uint32_t count = ctrl_regs[BDMA_WORD_COUNT_REG];

   if(!dir)
   {
    if(type == 0)       /* ROM → PM (24-bit words) */
    {
     while(count--)
     {
      uint32_t rs   = src % RAX_ROM_SIZE;
      uint32_t word = ((uint32_t)rom[rs] << 16) | ((uint32_t)rom[rs+1] << 8) | rom[rs+2];
      if(dst < ADSP2181_PM_SIZE)
       cpu->pm[dst] = word;
      src += 3; dst++;
     }
    }
    else if(type == 1)  /* ROM → DM (16-bit words) */
    {
     while(count--)
     {
      uint32_t rs   = src % RAX_ROM_SIZE;
      uint16_t word = ((uint16_t)rom[rs] << 8) | rom[rs+1];
      dm_write_addr(dst, word);
      src += 2; dst++;
     }
    }
    else                /* ROM → DM (byte, shifted) */
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
   else  /* dir=1: int→ext (DM → ROM) */
   {
    /* MAME notes "last stage in Batman Forever!? page=0, dir=1, type=1, src_addr=0xfd".
     * Transfer DM[BIAD..] → ROM[ext_addr..] so the in-memory ROM reflects any
     * DSP-computed sample data before a subsequent ext→int read uses it. */
    if(type == 1)  /* DM → ROM (16-bit words) */
    {
     while(count--)
     {
      uint16_t word = dm_read_addr(dst);
      uint32_t rs   = src % RAX_ROM_SIZE;
      rom[rs]   = (uint8_t)(word >> 8);
      rom[rs+1] = (uint8_t)(word & 0xFF);
      src += 2; dst++;
     }
    }
   }

   /* Update BDMA state registers after transfer (matches MAME rax.cpp behaviour).
    * BIAD auto-increments on every word written; net result is BIAD = initial_dst + count. */
   uint32_t src_local_end = src - (uint32_t)rom_bank * 0x400000;
   ctrl_regs[BDMA_INT_ADDR_REG]   = (uint16_t)(dst & 0x3FFF);
   ctrl_regs[BDMA_WORD_COUNT_REG] = 0;
   ctrl_regs[BDMA_EXT_ADDR_REG]   = (uint16_t)(src_local_end & 0x3FFF);
   ctrl_regs[BDMA_CONTROL_REG]    = (ctrl_regs[BDMA_CONTROL_REG] & ~0xFF00) |
                                     (uint16_t)(((src_local_end >> 14) & 0xFF) << 8);

   /* Bit 3 of BDMA_CONTROL_REG: reset CPU after BDMA, else fire BDMA IRQ.
    * Pass nullptr so PM is NOT reloaded — only CPU registers are reset.
    * (Matches MAME: pulse_input_line(RESET) calls adsp device_reset.) */
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
 RAX_ROM = new uint8[RAX_ROM_SIZE];
 memset(RAX_ROM, 0xFF, RAX_ROM_SIZE);

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
   MDFN_printf(_("[RAX] Loaded %s at 0x%06X (%u KB)\n"),
               rf.fname, rf.offset, rf.size >> 10);
  }
  catch(...)
  {
   MDFN_printf(_("[RAX] Warning: could not open %s — audio will be silent.\n"),
               rf.fname);
  }
 }

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
 memset(ctrl_regs, 0, sizeof(ctrl_regs));
 memset(cmd_fifo,  0, sizeof(cmd_fifo));
 cmd_fifo_rd    = 0;
 cmd_fifo_wr    = 0;
 data_out_latch = 0;
 adsp_snd_pf0   = 1;   /* idle: waiting for first command */
 rom_bank       = 0;
 data_bank      = 0;
 audio_valid    = false;

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
 /* A&2==0: status word (bit 0 = adsp_snd_pf0: 1=idle/ready, 0=busy)  [0x04000000]
  * A&2==2: response data latched by ADSP via io_write addr=3          [0x04000002] */
 if(A & 2) {
  *DB = data_out_latch;
  adsp_snd_pf0 = 1;  /* MAME data_r: host consumed output, ADSP may write again */
 }
 else
  *DB = (uint16)adsp_snd_pf0;
}

void RAX_WriteCommand(uint16 data)
{
 int next = (cmd_fifo_wr + 1) % CMD_FIFO_SIZE;
 if(next != cmd_fifo_rd)
 {
  cmd_fifo[cmd_fifo_wr] = data;
  cmd_fifo_wr = next;
 }
 /* MAME data_w: does NOT change pf0 */
 ADSP2181_SetIRQ(&rax_cpu, ADSP2181_IRQL0, 1);
}

/* ── Audio mixing ────────────────────────────────────────────────────────── */

void RAX_MixSample(int16* left, int16* right)
{
 /* Pulse SPORT0_TX each sample period.  SPORT0_TX uses edge-triggered
  * latching: SetIRQ(1) always sets the latch, so no prior deassert needed.
  * The ISR at 0x0010 jumps via PM[0x001C] to the synthesis mixer (0x26BA). */
 ADSP2181_SetIRQ(&rax_cpu, ADSP2181_SPORT0_TX, 1);
 ADSP2181_Run(&rax_cpu, ADSP_CYCLES_PER_SAMPLE);

 /* Read L+R directly from the SPORT0 autobuffer ring in DM.
  * S0_AUTOBUF_REG decodes: ireg = bits[11:9], mreg = bits[9:7] | (ireg & 4).
  * The firmware sets I7/M7/L7 for the ring; we advance I7 circularly per sample. */
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
   if(addr < base)                       addr += (uint32_t)len;
   else if(addr >= base + (uint32_t)len) addr -= (uint32_t)len;
   return addr;
  };
  *left  = (int16_t)dm_read_addr(i);
  i = circ_adv(i);
  *right = (int16_t)dm_read_addr(i);
  i = circ_adv(i);
  rax_cpu.i[ab_ireg] = i;
 }
}

/* ── Save state ──────────────────────────────────────────────────────────── */

void RAX_StateAction(StateMem* sm, const unsigned load, const bool data_only)
{
 SFORMAT BoardRegs[] =
 {
  SFPTR16N(ctrl_regs, 32,            "rax_ctrl"),
  SFPTR16N(cmd_fifo,  CMD_FIFO_SIZE, "rax_cmd_fifo"),
  SFVAR(cmd_fifo_rd),
  SFVAR(cmd_fifo_wr),
  SFVAR(data_out_latch),
  SFVAR(adsp_snd_pf0),
  SFVAR(rom_bank),
  SFVAR(data_bank),
  SFVAR(audio_valid),
  SFEND
 };

 MDFNSS_StateAction(sm, load, data_only, BoardRegs, "RAX");

 SFORMAT CpuMem[] =
 {
  SFPTR32N(rax_cpu.pm,             ADSP2181_PM_SIZE,
           "adsp_pm"),
  SFPTR16N(&rax_cpu.dm_bank[0][0], ADSP2181_NUM_BANKS * ADSP2181_DM_INT_WORDS,
           "adsp_dm_bank"),
  SFPTR16N(rax_cpu.dm_upper,       ADSP2181_DM_UPPER_WORDS,
           "adsp_dm_upper"),
  SFEND
 };

 MDFNSS_StateAction(sm, load, data_only, CpuMem, "RAXMEM");

 SFORMAT CpuRegs[] =
 {
  SFPTR8N((uint8*)&rax_cpu.core,  sizeof(adsp_core), "adsp_core"),
  SFPTR8N((uint8*)&rax_cpu.alt,   sizeof(adsp_core), "adsp_alt"),

  SFPTR32N(rax_cpu.i,            8, "adsp_i"),
  SFPTR32N((uint32*)rax_cpu.m,   8, "adsp_m"),
  SFPTR32N(rax_cpu.l,            8, "adsp_l"),
  SFPTR32N(rax_cpu.base,         8, "adsp_base"),
  SFVAR(rax_cpu.px),

  SFVAR(rax_cpu.pmovlay),
  SFVAR(rax_cpu.dmovlay),

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

  SFPTR32N(rax_cpu.loop_stack, 4,  "adsp_loop_stk"),
  SFPTR32N(rax_cpu.cntr_stack, 4,  "adsp_cntr_stk"),
  SFPTR32N(rax_cpu.pc_stack,  16,  "adsp_pc_stk"),
  SFPTR8N((uint8*)rax_cpu.stat_stack, 4*3*2, "adsp_stat_stk"),
  SFVAR(rax_cpu.pc_sp),
  SFVAR(rax_cpu.cntr_sp),
  SFVAR(rax_cpu.stat_sp),
  SFVAR(rax_cpu.loop_sp),

  SFVAR(rax_cpu.flagout),
  SFVAR(rax_cpu.flagin),
  SFVAR(rax_cpu.fl0),
  SFVAR(rax_cpu.fl1),
  SFVAR(rax_cpu.fl2),

  SFVAR(rax_cpu.idma_addr),
  SFVAR(rax_cpu.idma_cache),
  SFVAR(rax_cpu.idma_offs),

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
  rax_cpu.dm_active_bank = rax_cpu.dm_bank[rax_cpu.dm_active_bank_idx];
}

} // namespace MDFN_IEN_SS
