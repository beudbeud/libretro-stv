/*
 * Standalone ADSP-2181 CPU emulator for the Acclaim RAX sound board.
 * Adapted from MAME's adsp2100.cpp / 2100ops.hxx (copyright Aaron Giles,
 * BSD-3-Clause).  Stripped of all MAME framework dependencies; targets
 * ADSP-2181 only.
 *
 * This file is structured as plain C++ functions taking an ADSP2181* first
 * parameter.  Macros near the top alias the MAME "m_xxx" names to
 * "cpu->xxx" so that the algorithm code from 2100ops.hxx can be ported
 * with minimal mechanical changes.
 */

#include "adsp2181.h"

#include <cassert>
#include <cstring>
#include <cstdio>

/* ── Per-second ring-write diagnostics (read/reset by acclaim_rax.cpp) ───── */
uint32_t g_ring_total_per_sec = 0;  /* all writes to DM[0x3000..0x33FF] */
uint32_t g_pm_ring_write_min  = 0xFFFF; /* min PM address written by synthesis in [0x3000..0x33FF] */
uint32_t g_pm_ring_write_max  = 0x0000; /* max PM address written by synthesis in [0x3000..0x33FF] */
uint32_t g_ring_nz_per_sec    = 0;  /* non-zero writes */
uint32_t g_ring_nz_pc[8];          /* PC of first 8 non-zero ring writes this second */
uint32_t g_ring_nz_pc_cnt     = 0; /* how many PCs captured so far */
uint32_t g_synth2e90_per_sec  = 0; /* entries into PM[0x2E90] synthesis subroutine */
uint32_t g_copy008c_per_sec   = 0; /* entries into PM[0x008C] copy subroutine */
uint32_t g_mainloop_per_sec   = 0; /* entries into PM[0x2176] main loop */
uint32_t g_synth26ba_per_sec  = 0; /* entries into PM[0x26BA] ISR synthesis */
uint32_t g_syngate_cmd_per_sec = 0; /* synthesis gate "commands present" path (PC=0x266C) */
int      g_mix_trace_armed    = 0;  /* set by acclaim_rax when first command queued */
uint32_t g_mix_trace_budget   = 0;  /* remaining MIX_RD events to log */

/* ── MAME compatibility helpers ────────────────────────────────────────── */

/* BIT(x,n)   – extract bit n */
template<typename T> static inline T BIT(T x, int n)         { return (x >> n) & T(1); }
/* BIT(x,n,w) – extract w-bit field starting at bit n */
template<typename T> static inline T BIT(T x, int n, int w)  { return (x >> n) & ((T(1) << w) - T(1)); }

/* Sign-extend a value of 'bits' width to int32_t */
static inline int32_t sext(int32_t val, int bits)
{
    return (val << (32 - bits)) >> (32 - bits);
}

/* Leading-zero / leading-one counts */
static inline int count_leading_zeros_32(uint32_t x)
{
    return x ? __builtin_clz(x) : 32;
}
static inline int count_leading_ones_32(uint32_t x)
{
    return count_leading_zeros_32(~x);
}

/* ── Stack-depth constants ──────────────────────────────────────────────── */

#define PC_STACK_DEPTH   16
#define CNTR_STACK_DEPTH  4
#define STAT_STACK_DEPTH  4
#define LOOP_STACK_DEPTH  4

/* ── ASTAT flag aliases (match adsp2181.h ADSP_xxx) ────────────────────── */

#define ZFLAG   ADSP_Z
#define NFLAG   ADSP_N
#define VFLAG   ADSP_V
#define CFLAG   ADSP_C
#define SFLAG   ADSP_S
#define QFLAG   ADSP_Q
#define MVFLAG  ADSP_MV
#define SSFLAG  ADSP_SS

/* ── MSTAT flag aliases (match adsp2181.h ADSP_MSTAT_xxx) ──────────────── */

#define MSTAT_BANK      ADSP_MSTAT_BANK
#define MSTAT_REVERSE   ADSP_MSTAT_REVERSE
#define MSTAT_STICKYV   ADSP_MSTAT_STICKYV
#define MSTAT_SATURATE  ADSP_MSTAT_SATURATE
#define MSTAT_INTEGER   ADSP_MSTAT_INTEGER
#define MSTAT_TIMER     ADSP_MSTAT_TIMER
#define MSTAT_GOMODE    ADSP_MSTAT_GOMODE

/* ── SSTAT bits ─────────────────────────────────────────────────────────── */

#define PC_EMPTY        0x01
#define PC_OVER         0x02
#define COUNT_EMPTY     0x04
#define COUNT_OVER      0x08
#define STATUS_EMPTY    0x10
#define STATUS_OVER     0x20
#define LOOP_EMPTY      0x40
#define LOOP_OVER       0x80

/* ── Macro aliases: "m_xxx" → "cpu->xxx" ───────────────────────────────── */
/*
 * All functions below receive 'ADSP2181 *cpu' as first argument.
 * These macros let the ported MAME code use m_xxx spelling unchanged.
 */

#define m_astat           (cpu->astat)
#define m_sstat           (cpu->sstat)
#define m_mstat           (cpu->mstat)
#define m_mstat_prev      (cpu->mstat_prev)
#define m_astat_clear     (cpu->astat_clear)
#define m_idle            (cpu->idle)
#define m_pc              (cpu->pc)
#define m_ppc             (cpu->ppc)
#define m_loop            (cpu->loop)
#define m_loop_condition  (cpu->loop_condition)
#define m_cntr            (cpu->cntr)
#define m_core            (cpu->core)
#define m_alt             (cpu->alt)
#define m_i               (cpu->i)
#define m_m               (cpu->m)
#define m_l               (cpu->l)
#define m_lmask           (cpu->lmask)
#define m_base            (cpu->base)
#define m_px              (cpu->px)
#define m_pmovlay         (cpu->pmovlay)
#define m_dmovlay         (cpu->dmovlay)
#define m_flagout         (cpu->flagout)
#define m_flagin          (cpu->flagin)
#define m_fl0             (cpu->fl0)
#define m_fl1             (cpu->fl1)
#define m_fl2             (cpu->fl2)
#define m_idma_addr       (cpu->idma_addr)
#define m_idma_cache      (cpu->idma_cache)
#define m_idma_offs       (cpu->idma_offs)
#define m_imask           (cpu->imask)
#define m_icntl           (cpu->icntl)
#define m_ifc             (cpu->ifc)
#define m_irq_state       (cpu->irq_state)
#define m_irq_latch       (cpu->irq_latch)
#define m_icount          (cpu->icount)
#define m_condition_table (cpu->condition_table)
#define m_mask_table      (cpu->mask_table)
#define m_reverse_table   (cpu->reverse_table)
#define m_read0_ptr       (cpu->read0_ptr)
#define m_read1_ptr       (cpu->read1_ptr)
#define m_read2_ptr       (cpu->read2_ptr)
#define m_alu_xregs       (cpu->alu_xregs)
#define m_alu_yregs       (cpu->alu_yregs)
#define m_mac_xregs       (cpu->mac_xregs)
#define m_mac_yregs       (cpu->mac_yregs)
#define m_shift_xregs     (cpu->shift_xregs)
#define m_loop_stack      (cpu->loop_stack)
#define m_cntr_stack      (cpu->cntr_stack)
#define m_pc_stack        (cpu->pc_stack)
#define m_stat_stack      (cpu->stat_stack)
#define m_pc_sp           (cpu->pc_sp)
#define m_cntr_sp         (cpu->cntr_sp)
#define m_stat_sp         (cpu->stat_sp)
#define m_loop_sp         (cpu->loop_sp)

/* chip-type is always ADSP-2181 here */
#define CHIP_TYPE_ADSP2101  0
#define CHIP_TYPE_ADSP2181  1
#define m_chip_type         CHIP_TYPE_ADSP2181
#define m_mstat_mask        0x7f
#define m_imask_mask        0x3ff

/* MAME callback shims */
#define m_timer_fired_cb(x)            /* no-op: timer driven by host */
#define m_dmovlay_cb(x)                do { if (cpu->dmovlay_cb)  cpu->dmovlay_cb(cpu, (x));         } while (0)
#define m_sport_tx_cb(port, val, mask) do { if (cpu->sport_tx_cb) cpu->sport_tx_cb(cpu, (port), (uint32_t)(val)); } while (0)
#define m_sport_rx_cb(port)            ((cpu->sport_rx_cb) ? cpu->sport_rx_cb(cpu, (port)) : (uint32_t)0)

/* MAME error shims */
#define logerror(...)   (void)0
#define fatalerror(...) (void)0

/* ── Register-file macros (from 2100ops.hxx) ────────────────────────────── */

#define ALU_GETXREG_UNSIGNED(x)   (*(uint16_t *)m_alu_xregs[x])
#define ALU_GETYREG_UNSIGNED(y)   (*(uint16_t *)m_alu_yregs[y])

#define MAC_GETXREG_UNSIGNED(x)   (*(uint16_t *)m_mac_xregs[x])
#define MAC_GETXREG_SIGNED(x)     (*( int16_t *)m_mac_xregs[x])
#define MAC_GETYREG_UNSIGNED(y)   (*(uint16_t *)m_mac_yregs[y])
#define MAC_GETYREG_SIGNED(y)     (*( int16_t *)m_mac_yregs[y])

#define SHIFT_GETXREG_UNSIGNED(x) (*(uint16_t *)m_shift_xregs[x])
#define SHIFT_GETXREG_SIGNED(x)   (*( int16_t *)m_shift_xregs[x])

/* ── ASTAT flag macros (from 2100ops.hxx) ───────────────────────────────── */

#define GET_SS  (m_astat & SSFLAG)
#define GET_MV  (m_astat & MVFLAG)
#define GET_Q   (m_astat &  QFLAG)
#define GET_S   (m_astat &  SFLAG)
#define GET_C   (m_astat &  CFLAG)
#define GET_V   (m_astat &  VFLAG)
#define GET_N   (m_astat &  NFLAG)
#define GET_Z   (m_astat &  ZFLAG)

#define CLR_SS  (m_astat &= ~SSFLAG)
#define CLR_MV  (m_astat &= ~MVFLAG)
#define CLR_Q   (m_astat &=  ~QFLAG)
#define CLR_S   (m_astat &=  ~SFLAG)
#define CLR_C   (m_astat &=  ~CFLAG)
#define CLR_V   (m_astat &=  ~VFLAG)
#define CLR_N   (m_astat &=  ~NFLAG)
#define CLR_Z   (m_astat &=  ~ZFLAG)

#define SET_SS  (m_astat |= SSFLAG)
#define SET_MV  (m_astat |= MVFLAG)
#define SET_Q   (m_astat |=  QFLAG)
#define SET_S   (m_astat |=  SFLAG)
#define SET_C   (m_astat |=  CFLAG)
#define SET_V   (m_astat |=  VFLAG)
#define SET_Z   (m_astat |=  ZFLAG)
#define SET_N   (m_astat |=  NFLAG)

#define CLR_FLAGS       (m_astat &= m_astat_clear)

#define CALC_Z(r)         (m_astat |= ((r & 0xffff) == 0))
#define CALC_N(r)         (m_astat |= (r >> 14) & 0x02)
#define CALC_V(s,d,r)     (m_astat |= ((s ^ d ^ r ^ (r >> 1)) >> 13) & 0x04)
#define CALC_C(r)         (m_astat |= (r >> 13) & 0x08)
#define CALC_C_SUB(r)     (m_astat |= (~r >> 13) & 0x08)
#define CALC_NZ(r)        CLR_FLAGS; CALC_N(r); CALC_Z(r)
#define CALC_NZV(s,d,r)   CLR_FLAGS; CALC_N(r); CALC_Z(r); CALC_V(s,d,r)
#define CALC_NZVC(s,d,r)  CLR_FLAGS; CALC_N(r); CALC_Z(r); CALC_V(s,d,r); CALC_C(r)
#define CALC_NZVC_SUB(s,d,r) CLR_FLAGS; CALC_N(r); CALC_Z(r); CALC_V(s,d,r); CALC_C_SUB(r)

/* ADSP-218x constant table (used by alu_op_ar_const / alu_op_af_const) */
static const int32_t constants[] =
{
    0x0001, 0xfffe, 0x0002, 0xfffd, 0x0004, 0xfffb, 0x0008, 0xfff7,
    0x0010, 0xffef, 0x0020, 0xffdf, 0x0040, 0xffbf, 0x0080, 0xff7f,
    0x0100, 0xfeff, 0x0200, 0xfdff, 0x0400, 0xfbff, 0x0800, 0xf7ff,
    0x1000, 0xefff, 0x2000, 0xdfff, 0x4000, 0xbfff, 0x8000, 0x7fff
};

/* ── Forward declarations ───────────────────────────────────────────────── */

static void check_irqs(ADSP2181 *cpu);
static void update_mstat(ADSP2181 *cpu);
static void update_i(ADSP2181 *cpu, int which);
static void update_l(ADSP2181 *cpu, int which);

/* How many instructions left to trace for the current IRQL0 ISR invocation.
 * Set to >0 by generate_irq when IRQL0 fires; decremented in execute_run. */
static int g_isr_trace_remaining = 0;
static int g_isr_trace_count     = 0;   /* which invocation we are tracing */

/* Set when synthesis first writes a zero to PM[0x31A3..0x33A2] (game active) */
static bool g_game_started = false;

/* ── Memory accessors ───────────────────────────────────────────────────── */

/*
 * Data memory map (14-bit address):
 *   0x0000-0x1FFF  internal banked RAM (DMOVLAY selects bank)
 *   0x2000-0x3FDF  internal upper RAM (always present)
 *   0x3FE0-0x3FFF  internal control registers → io callback, addr 0x800+offset
 */
static inline uint16_t data_read(ADSP2181 *cpu, uint32_t addr)
{
    addr &= 0x3fff;
    uint16_t val;
    if (addr < 0x2000)
        val = cpu->dm_active_bank[addr];
    else if (addr < 0x3fe0)
        val = cpu->dm_upper[addr - 0x2000];
    else {
        val = cpu->io_read_cb ? cpu->io_read_cb(cpu, 0x800u + (addr & 0x1f)) : 0xffff;
    }

    /* Log ALL DM reads from the ISR dispatch area (PC 0x2100..0x2200) */
    if (cpu->pc >= 0x2100 && cpu->pc <= 0x2200) {
        static uint32_t isr_rd = 0;
        if (isr_rd++ < 2048)
            fprintf(stderr, "[ISR_RD] DM[%04X]=%04X PC=%04X\n",
                    addr, val, (unsigned)cpu->pc);
    }
    /* Trace DM reads from the mixing loop (PC 0x0000..0x0200), gated on g_mix_trace_armed */
    if (g_mix_trace_armed && cpu->pc <= 0x0200 && g_mix_trace_budget > 0) {
        g_mix_trace_budget--;
        fprintf(stderr, "[MIX_RD] DM[%04X]=%04X PC=%04X bank=%d dmov=%u\n",
                addr, val, (unsigned)cpu->pc,
                cpu->dm_active_bank_idx, (unsigned)cpu->dmovlay);
    }
    /* trace reads from command queue DM[0x3400..0x3490] */
    if (addr >= 0x3400 && addr < 0x3490) {
        static uint32_t cq_rd = 0;
        if (cq_rd++ < 256)
            fprintf(stderr, "[CQ_RD] DM[%04X]=%04X PC=%04X\n", addr, val, (unsigned)cpu->pc);
    }
    /* trace reads near synthesis pointer area DM[0x3100..0x31FF] */
    if (addr >= 0x3100 && addr < 0x3200) {
        static uint32_t syn_rd = 0;
        if (syn_rd++ < 128)
            fprintf(stderr, "[SYN_RD] DM[%04X]=%04X PC=%04X\n", addr, val, (unsigned)cpu->pc);
    }
    /* trace DM[0x3BDA] reads (CNTR for voice synthesis loop) */
    if (addr == 0x3BDA) {
        static uint32_t cntr_rd = 0;
        if (cntr_rd++ < 64)
            fprintf(stderr, "[CNTR_RD] DM[3BDA]=%04X PC=%04X (#%u)\n", val, (unsigned)cpu->pc, cntr_rd);
    }
    /* unconditional trace for DM[0x3BDC] reads — find the FFFF source */
    if (addr == 0x3BDC || addr == 0x3BDB || addr == 0x3BDE) {
        static uint32_t bdc_rd = 0;
        bdc_rd++;
        if (bdc_rd <= 200)
            fprintf(stderr, "[BDC_RD] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, val, (unsigned)cpu->pc, bdc_rd);
    }
    /* trace reads from voice table DM[0x3B00..0x3C00] */
    if (addr >= 0x3B00 && addr < 0x3C00 && addr != 0x3BDA) {
        static uint32_t vt_rd = 0;
        if (vt_rd++ < 256)
            fprintf(stderr, "[VT_RD] DM[%04X]=%04X PC=%04X\n", addr, val, (unsigned)cpu->pc);
    }
    /* trace ALL DM reads from synthesis outer loop PC=0x2D00..0x2F00 */
    if (cpu->pc >= 0x2D00 && cpu->pc < 0x2F00) {
        static uint32_t synth_rd = 0;
        if (synth_rd++ < 512)
            fprintf(stderr, "[SYNTH_RD] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, val, (unsigned)cpu->pc, synth_rd);
    }
    /* trace reads of the command queue pointers DM[0x3467] and DM[0x3469] (any PC) */
    if (addr == 0x3467 || addr == 0x3469) {
        static uint32_t ptr_rd = 0;
        if (ptr_rd++ < 128)
            fprintf(stderr, "[PTR_RD] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, val, (unsigned)cpu->pc, ptr_rd);
    }
    /* trace ALL DM reads from the voice synthesis subroutine PM[0x3C35..0x3DAB] */
    if (cpu->pc >= 0x3C35 && cpu->pc <= 0x3DAB) {
        static uint32_t sub3c35_rd = 0;
        if (sub3c35_rd++ < 1000)
            fprintf(stderr, "[SUB3C35_RD] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, val, (unsigned)cpu->pc, sub3c35_rd);
    }
    /* trace ALL DM reads from the extended synthesis area PM[0x3DAC..0x3E60] */
    if (cpu->pc >= 0x3DAC && cpu->pc <= 0x3E60) {
        static uint32_t sub3dac_rd = 0;
        if (sub3dac_rd++ < 500)
            fprintf(stderr, "[SUB3DAC_RD] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, val, (unsigned)cpu->pc, sub3dac_rd);
    }

    return val;
}

static inline void data_write(ADSP2181 *cpu, uint32_t addr, uint16_t data)
{
    addr &= 0x3fff;
    /* trace writes to the command queue pointers DM[0x3467] and DM[0x3469] (any PC) */
    if (addr == 0x3467 || addr == 0x3469) {
        static uint32_t ptr_wr = 0;
        if (ptr_wr++ < 128)
            fprintf(stderr, "[PTR_WR] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, data, (unsigned)cpu->pc, ptr_wr);
    }
    /* Log ALL DM writes from the ISR dispatch area (PC 0x2100..0x2200) */
    if (cpu->pc >= 0x2100 && cpu->pc <= 0x2200) {
        static uint32_t isr_wr = 0;
        if (isr_wr++ < 2048)
            fprintf(stderr, "[ISR_DM] DM[%04X]=%04X PC=%04X\n",
                    addr, data, (unsigned)cpu->pc);
    }
    if (addr < 0x2000)
    {
        /* trace writes to DM[0x1FE0..0x1FEF] (voice state area) */
        if (addr >= 0x1FE0 && addr < 0x1FF0) {
            static uint32_t fe_wr = 0;
            if (fe_wr++ < 64)
                fprintf(stderr, "[ADSP] DM[%04X]=%04X PC=%04X\n",
                        addr, data, (unsigned)cpu->pc);
        }
        /* trace ALL DM writes from mixing loop (PC 0x3A50..0x3B00) — first 256 */
        if (cpu->pc >= 0x3A50 && cpu->pc <= 0x3B00) {
            static uint32_t mix_wr = 0;
            if (mix_wr++ < 256)
                fprintf(stderr, "[MIX_DM] DM[%04X]=%04X PC=%04X (#%u) bank=%d\n",
                        addr, data, (unsigned)cpu->pc, mix_wr,
                        cpu->dm_active_bank_idx);
        }
        /* trace banked DM writes from synthesis/voice-table-update PC=0x3500..0x3800 */
        if (cpu->pc >= 0x3500 && cpu->pc < 0x3800) {
            static uint32_t bk_synout = 0;
            if (bk_synout++ < 512)
                fprintf(stderr, "[BK_SYNOUT] DM[%04X]=%04X PC=%04X bank=%d (#%u)\n",
                        addr, data, (unsigned)cpu->pc, cpu->dm_active_bank_idx, bk_synout);
        }
        /* trace first 32 non-zero writes to banked DM from synthesis engine (PC >= 0x3000) */
        if (data != 0 && cpu->pc >= 0x3000) {
            static uint32_t bank_syn_wr = 0;
            if (bank_syn_wr++ < 32)
                fprintf(stderr, "[BANK_SYN] DM[%04X]=%04X PC=%04X (#%u)\n",
                        addr, data, (unsigned)cpu->pc, bank_syn_wr);
        }
        cpu->dm_active_bank[addr] = data;
        return;
    }
    if (addr < 0x3fe0)
    {
        /* trace ring writes DM[0x3000..0x33FF] */
        if (addr >= 0x3000 && addr < 0x3400) {
            static uint32_t ring_zero_runs = 0;
            static uint32_t ring_write_total = 0;
            ring_write_total++;
            g_ring_total_per_sec++;
            /* log PC for first 32 ring writes to identify writer code */
            if (ring_write_total <= 32)
                fprintf(stderr, "[RING_PC] write #%u addr=%04X val=%04X PC=%04X\n",
                        ring_write_total, addr, data, (unsigned)cpu->pc);
            if (data != 0) {
                g_ring_nz_per_sec++;
                if (g_ring_nz_pc_cnt < 8)
                    g_ring_nz_pc[g_ring_nz_pc_cnt++] = cpu->pc;
                /* log first 32 non-zero ring writes with value and I1 position */
                {
                    static uint32_t nz_ring_log = 0;
                    if (nz_ring_log++ < 32)
                        fprintf(stderr, "[RING_NZ] #%u addr=%04X val=%04X PC=%04X I1=%04X I7=%04X\n",
                                nz_ring_log, addr, data, (unsigned)cpu->pc,
                                (unsigned)(cpu->i[1] & 0x3FFF),
                                (unsigned)(cpu->i[7] & 0x3FFF));
                }
            }
            /* detect zeroing sweeps (first 5 only) */
            if (data == 0 && addr == 0x3000) {
                ring_zero_runs++;
                if (ring_zero_runs <= 5)
                    fprintf(stderr, "[RING_ZERO] zeroing sweep #%u at PC=%04X total_ring_writes=%u\n",
                            ring_zero_runs, (unsigned)cpu->pc, ring_write_total);
            }
            /* log first ring write AFTER synthesis (first post-synthesis zero write in new second) */
            {
                static bool first_post_synth = false;
                if (!first_post_synth && ring_write_total > 15360 && data == 0) {
                    first_post_synth = true;
                    uint16_t i4v = (uint16_t)cpu->i[4];
                    uint32_t pm4v = cpu->pm[i4v & 0x3fff];
                    uint16_t i1v = (uint16_t)cpu->i[1];
                    fprintf(stderr, "[POST_SYNTH_RING] first zero write: addr=%04X data=%04X PC=%04X I4=%04X PM[I4]=%06X I1=%04X\n",
                            addr, data, (unsigned)cpu->pc, i4v, pm4v, i1v);
                }
            }
            /* log sweep end */
            if (addr == 0x33FF && data == 0 && ring_zero_runs > 0) {
                static uint32_t sweep_end = 0;
                if (sweep_end++ < 3)
                    fprintf(stderr, "[SWEEP_END] ring zeroing done at PC=%04X sweep=%u\n",
                            (unsigned)cpu->pc, ring_zero_runs);
            }
        }
        /* trace first 64 non-zero dm_upper writes from synthesis engine (PC >= 0x3000) */
        if (data != 0 && cpu->pc >= 0x3000) {
            static uint32_t upper_syn_wr = 0;
            if (upper_syn_wr++ < 64)
                fprintf(stderr, "[UPPER_SYN] DM[%04X]=%04X PC=%04X (#%u)\n",
                        addr, data, (unsigned)cpu->pc, upper_syn_wr);
        }
        /* trace writes to audio synthesis range [0x3900..0x3E00] with PC */
        if (addr >= 0x3900 && addr < 0x3E00 && data != 0) {
            static uint32_t dmu_wr = 0;
            if (dmu_wr++ < 256)
                fprintf(stderr, "[DMU] DM[%04X]=%04X PC=%04X\n",
                        addr, data, (unsigned)cpu->pc);
        }
        /* trace ALL writes to DM[0x3BDA] (voice count / CNTR source) — unlimited */
        if (addr == 0x3BDA) {
            static uint32_t bda_wr = 0;
            bda_wr++;
            fprintf(stderr, "[BDA_WR] DM[3BDA]=%04X PC=%04X (#%u)\n",
                    data, (unsigned)cpu->pc, bda_wr);
        }
        /* trace ALL writes to voice table area DM[0x3B00..0x3BFF] */
        if (addr >= 0x3B00 && addr < 0x3C00) {
            static uint32_t vtw = 0;
            if (vtw++ < 2048)
                fprintf(stderr, "[VT_WR] DM[%04X]=%04X PC=%04X (#%u)\n",
                        addr, data, (unsigned)cpu->pc, vtw);
        }
        /* unconditional trace for DM[0x3BDC] — must catch the FFFF write */
        if (addr == 0x3BDC || addr == 0x3BDB || addr == 0x3BDE || addr == 0x3BDA) {
            static uint32_t bdc_wr = 0;
            bdc_wr++;
            fprintf(stderr, "[BDC_WR] DM[%04X]=%04X PC=%04X (#%u)\n",
                    addr, data, (unsigned)cpu->pc, bdc_wr);
        }
        /* trace DM writes from synthesis output loop PC=0x3500..0x3800 (include zeros) */
        if (cpu->pc >= 0x3500 && cpu->pc < 0x3800) {
            static uint32_t synout_wr = 0;
            if (synout_wr++ < 1024)
                fprintf(stderr, "[SYNOUT] DM[%04X]=%04X PC=%04X (#%u)\n",
                        addr, data, (unsigned)cpu->pc, synout_wr);
        }
        /* trace ALL DM writes from voice synthesis subroutine PM[0x3C35..0x3DAB] */
        if (cpu->pc >= 0x3C35 && cpu->pc <= 0x3DAB) {
            static uint32_t sub3c35_wr = 0;
            if (sub3c35_wr++ < 500)
                fprintf(stderr, "[SUB3C35_WR] DM[%04X]=%04X PC=%04X (#%u)\n",
                        addr, data, (unsigned)cpu->pc, sub3c35_wr);
        }
        /* trace ALL DM writes from extended synthesis area PM[0x3DAC..0x3E60] */
        if (cpu->pc >= 0x3DAC && cpu->pc <= 0x3E60) {
            static uint32_t sub3dac_wr = 0;
            if (sub3dac_wr++ < 256)
                fprintf(stderr, "[SUB3DAC_WR] DM[%04X]=%04X PC=%04X (#%u)\n",
                        addr, data, (unsigned)cpu->pc, sub3dac_wr);
        }
        cpu->dm_upper[addr - 0x2000] = data;
        return;
    }
    /* control registers */
    {
        static uint32_t ctrl_trace = 0;
        if (ctrl_trace++ < 256)
            fprintf(stderr, "[ADSP] DM ctrl write: addr=%04X data=%04X PC=%04X\n",
                    addr, data, (unsigned)cpu->pc);
    }
    if (cpu->io_write_cb)
        cpu->io_write_cb(cpu, 0x800u + (addr & 0x1f), data);
}

/*
 * IO space (11-bit address, 0x000-0x7FF external).
 * Addresses 0x800-0x81F are the DM control registers routed here
 * by data_read/data_write above.
 */
static inline uint16_t io_read(ADSP2181 *cpu, uint32_t addr)
{
    if (cpu->io_read_cb)
        return cpu->io_read_cb(cpu, addr & 0xfff);
    return 0xffff;
}

static inline void io_write(ADSP2181 *cpu, uint32_t addr, uint16_t data)
{
    if (cpu->io_write_cb)
        cpu->io_write_cb(cpu, addr & 0xfff, data);
}

/* Program memory (14-bit address, 24-bit words stored in uint32) */
static inline uint32_t program_read(ADSP2181 *cpu, uint32_t addr)
{
    return cpu->pm[addr & 0x3fff];
}

static inline void program_write(ADSP2181 *cpu, uint32_t addr, uint32_t data)
{
    uint32_t masked = addr & 0x3fff;
    uint32_t nv = data & 0xffffff;
    uint32_t old = cpu->pm[masked];
    /* always trace writes to PM[0x001C] (synthesis hook) */
    if (masked == 0x001C)
        fprintf(stderr, "[PM_001C] PM[001C]: %06X → %06X (PC=%04X)\n",
                old, nv, (unsigned)cpu->pc);
    /* trace first 128 non-boot PM writes */
    if (cpu->pc < 0x3800 || cpu->pc > 0x38FF) {
        static uint32_t pm_wr_cnt = 0;
        pm_wr_cnt++;
        if (pm_wr_cnt <= 128)
            fprintf(stderr, "[PM_WR] PM[%04X]: %06X → %06X (PC=%04X cnt=%u)\n",
                    masked, old, nv, (unsigned)cpu->pc, pm_wr_cnt);
    }
    cpu->pm[masked] = nv;
}

/* Opcode fetch (same as program_read from current PC) */
static inline uint32_t opcode_read(ADSP2181 *cpu)
{
    return cpu->pm[cpu->pc & 0x3fff];
}

/* ── MSTAT update ───────────────────────────────────────────────────────── */

static void update_mstat(ADSP2181 *cpu)
{
    if ((m_mstat ^ m_mstat_prev) & MSTAT_BANK)
    {
        adsp_core tmp = m_core;
        m_core = m_alt;
        m_alt  = tmp;
    }
    m_timer_fired_cb((m_mstat & MSTAT_TIMER) != 0);
    if (m_mstat & MSTAT_STICKYV)
        m_astat_clear = ~(CFLAG | NFLAG | ZFLAG);
    else
        m_astat_clear = ~(CFLAG | VFLAG | NFLAG | ZFLAG);
    m_mstat_prev = m_mstat;
}

/* ── DAG helpers ────────────────────────────────────────────────────────── */

static inline void update_i(ADSP2181 *cpu, int which)
{
    m_base[which] = m_i[which] & m_lmask[which];
}

static inline void update_l(ADSP2181 *cpu, int which)
{
    m_lmask[which] = m_mask_table[m_l[which] & 0x3fff];
    m_base[which]  = m_i[which] & m_lmask[which];
}

static void update_dmovlay(ADSP2181 *cpu)
{
    {
        static uint32_t dmov_cnt = 0;
        if (dmov_cnt++ < 64)
            fprintf(stderr, "[DMOV] dmovlay=%u PC=%04X\n",
                    (unsigned)m_dmovlay, (unsigned)m_pc);
    }
    m_dmovlay_cb(m_dmovlay);
}

static inline void modify_address(ADSP2181 *cpu, uint32_t ireg, uint32_t mreg)
{
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
}

static inline void data_write_dag1(ADSP2181 *cpu, uint32_t op, int32_t val)
{
    uint32_t ireg = BIT(op, 2, 2);
    uint32_t mreg = BIT(op, 0, 2);
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    if (m_mstat & MSTAT_REVERSE)
        data_write(cpu, m_reverse_table[i & 0x3fff], val);
    else
        data_write(cpu, i, val);
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
}

static inline uint32_t data_read_dag1(ADSP2181 *cpu, uint32_t op)
{
    uint32_t ireg = BIT(op, 2, 2);
    uint32_t mreg = BIT(op, 0, 2);
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    uint32_t res;
    if (m_mstat & MSTAT_REVERSE)
        res = data_read(cpu, m_reverse_table[i & 0x3fff]);
    else
        res = data_read(cpu, i);
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
    return res;
}

static inline void data_write_dag2(ADSP2181 *cpu, uint32_t op, int32_t val)
{
    uint32_t ireg = 4 + BIT(op, 2, 2);
    uint32_t mreg = 4 + BIT(op, 0, 2);
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    data_write(cpu, i, val);
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
}

static inline uint32_t data_read_dag2(ADSP2181 *cpu, uint32_t op)
{
    uint32_t ireg = 4 + BIT(op, 2, 2);
    uint32_t mreg = 4 + BIT(op, 0, 2);
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    uint32_t res  = data_read(cpu, i);
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
    return res;
}

static inline void pgm_write_dag2(ADSP2181 *cpu, uint32_t op, int32_t val)
{
    uint32_t ireg = 4 + BIT(op, 2, 2);
    uint32_t mreg = 4 + BIT(op, 0, 2);
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    uint32_t pm_val = (val << 8) | m_px;
    if (i >= 0x31A3 && i <= 0x33A2) {
        static uint32_t synwr_cnt = 0;
        if (synwr_cnt++ < 20)
            fprintf(stderr, "[PM_WR] PM[%04X]=%06X val=%04X m_px=%02X PC=%04X\n",
                    i, pm_val, (unsigned)(uint16_t)val, (unsigned)(uint8_t)m_px, (unsigned)cpu->pc);
    }
    /* Trace writes to the synthesis hook at PM[0x001C] */
    if (i == 0x001C) {
        static uint32_t hook_cnt = 0;
        hook_cnt++;
        fprintf(stderr, "[PM001C_WR] #%u PM[001C]=%06X->%06X PC=%04X (val=%04X m_px=%02X)\n",
                hook_cnt, cpu->pm[0x001C], pm_val,
                (unsigned)cpu->pc, (unsigned)(uint16_t)val, (unsigned)(uint8_t)m_px);
    }
    program_write(cpu, i, pm_val);
    if (i >= 0x3000 && i <= 0x33FF) {
        if (i < g_pm_ring_write_min) g_pm_ring_write_min = i;
        if (i > g_pm_ring_write_max) g_pm_ring_write_max = i;
    }
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
}

static inline uint32_t pgm_read_dag2(ADSP2181 *cpu, uint32_t op)
{
    uint32_t ireg = 4 + BIT(op, 2, 2);
    uint32_t mreg = 4 + BIT(op, 0, 2);
    uint32_t base = m_base[ireg];
    uint32_t i    = m_i[ireg];
    uint32_t l    = m_l[ireg];
    uint32_t res  = program_read(cpu, i);
    /* log first 32 PM reads that produce a non-zero result */
    {
        static uint32_t pgrd_nz = 0;
        static uint32_t pgrd_total = 0;
        pgrd_total++;
        uint16_t val16 = (uint16_t)(res >> 8);
        if (val16 != 0 && pgrd_nz++ < 32)
            fprintf(stderr, "[PGRD] PM[%04X]=%06X val=%04X I%u base=%04X L=%04X M=%04X PC=%04X #%u\n",
                    i, res, val16, ireg, base, l, m_m[mreg], (unsigned)cpu->pc, pgrd_total);
    }
    m_px = res;
    res >>= 8;
    i = (i + m_m[mreg]) & 0x3fff;
    if      (i < base)     i += l;
    else if (i >= base + l) i -= l;
    m_i[ireg] = i;
    return res;
}

/* ── Stack handlers ─────────────────────────────────────────────────────── */

static inline uint32_t pc_stack_top(ADSP2181 *cpu)
{
    return (m_pc_sp > 0) ? m_pc_stack[m_pc_sp - 1] : m_pc_stack[0];
}

static inline void set_pc_stack_top(ADSP2181 *cpu, uint32_t top)
{
    if (m_pc_sp > 0) m_pc_stack[m_pc_sp - 1] = top;
    else             m_pc_stack[0] = top;
}

static inline void pc_stack_push(ADSP2181 *cpu)
{
    if (m_pc_sp < PC_STACK_DEPTH) { m_pc_stack[m_pc_sp++] = m_pc; m_sstat &= ~PC_EMPTY; }
    else                            m_sstat |= PC_OVER;
}

static inline void pc_stack_push_val(ADSP2181 *cpu, uint32_t val)
{
    if (m_pc_sp < PC_STACK_DEPTH) { m_pc_stack[m_pc_sp++] = val; m_sstat &= ~PC_EMPTY; }
    else                            m_sstat |= PC_OVER;
}

static inline void pc_stack_pop(ADSP2181 *cpu)
{
    if (m_pc_sp > 0) { m_pc_sp--; if (m_pc_sp == 0) m_sstat |= PC_EMPTY; }
    m_pc = m_pc_stack[m_pc_sp];
}

static inline uint32_t pc_stack_pop_val(ADSP2181 *cpu)
{
    if (m_pc_sp > 0) { m_pc_sp--; if (m_pc_sp == 0) m_sstat |= PC_EMPTY; }
    return m_pc_stack[m_pc_sp];
}

static inline uint32_t cntr_stack_top(ADSP2181 *cpu)
{
    return (m_cntr_sp > 0) ? m_cntr_stack[m_cntr_sp - 1] : m_cntr_stack[0];
}

static inline void cntr_stack_push(ADSP2181 *cpu)
{
    if (m_cntr_sp < CNTR_STACK_DEPTH) { m_cntr_stack[m_cntr_sp++] = m_cntr; m_sstat &= ~COUNT_EMPTY; }
    else                                m_sstat |= COUNT_OVER;
}

static inline void cntr_stack_pop(ADSP2181 *cpu)
{
    if (m_cntr_sp > 0) { m_cntr_sp--; if (m_cntr_sp == 0) m_sstat |= COUNT_EMPTY; }
    m_cntr = m_cntr_stack[m_cntr_sp];
}

static inline uint32_t loop_stack_top(ADSP2181 *cpu)
{
    return (m_loop_sp > 0) ? m_loop_stack[m_loop_sp - 1] : m_loop_stack[0];
}

static inline void loop_stack_push(ADSP2181 *cpu, uint32_t value)
{
    if (m_loop_sp < LOOP_STACK_DEPTH)
    {
        m_loop_stack[m_loop_sp++] = value;
        m_loop           = value >> 4;
        m_loop_condition = value & 15;
        m_sstat &= ~LOOP_EMPTY;
    }
    else m_sstat |= LOOP_OVER;
}

static inline void loop_stack_pop(ADSP2181 *cpu)
{
    if (m_loop_sp > 0)
    {
        m_loop_sp--;
        if (m_loop_sp == 0)
        {
            m_loop = 0xffff; m_loop_condition = 0; m_sstat |= LOOP_EMPTY;
        }
        else
        {
            m_loop           = m_loop_stack[m_loop_sp - 1] >> 4;
            m_loop_condition = m_loop_stack[m_loop_sp - 1] & 15;
        }
    }
}

static inline void stat_stack_push(ADSP2181 *cpu)
{
    if (m_stat_sp < STAT_STACK_DEPTH)
    {
        m_stat_stack[m_stat_sp][0] = m_mstat;
        m_stat_stack[m_stat_sp][1] = m_imask;
        m_stat_stack[m_stat_sp][2] = m_astat;
        m_stat_sp++;
        m_sstat &= ~STATUS_EMPTY;
    }
    else m_sstat |= STATUS_OVER;
}

static inline void stat_stack_pop(ADSP2181 *cpu)
{
    if (m_stat_sp > 0) { m_stat_sp--; if (m_stat_sp == 0) m_sstat |= STATUS_EMPTY; }
    m_mstat = m_stat_stack[m_stat_sp][0];
    update_mstat(cpu);
    m_imask = m_stat_stack[m_stat_sp][1];
    m_astat = m_stat_stack[m_stat_sp][2];
    check_irqs(cpu);
}

/* ── Condition code ─────────────────────────────────────────────────────── */

static int slow_condition(ADSP2181 *cpu)
{
    if ((int32_t)--m_cntr > 0)
        return 1;
    cntr_stack_pop(cpu);
    return 0;
}

/* condition(c): for use in execute loop — c==14 triggers slow_condition */
#define condition(c) (((c) != 14) ? (m_condition_table[((c) << 8) | m_astat]) : slow_condition(cpu))

/* ── Register read / write ──────────────────────────────────────────────── */

static void write_reg0(ADSP2181 *cpu, int regnum, int32_t val)
{
    switch (regnum)
    {
        case 0x00: m_core.ax0.s = val;                                                    break;
        case 0x01: m_core.ax1.s = val;                                                    break;
        case 0x02: m_core.mx0.s = val;                                                    break;
        case 0x03: m_core.mx1.s = val;                                                    break;
        case 0x04: m_core.ay0.s = val;                                                    break;
        case 0x05: m_core.ay1.s = val;                                                    break;
        case 0x06: m_core.my0.s = val;                                                    break;
        case 0x07: m_core.my1.s = val;                                                    break;
        case 0x08: m_core.si.s  = val;                                                    break;
        case 0x09: m_core.se.s  = (int8_t)val;                                            break;
        case 0x0a: m_core.ar.s  = val;                                                    break;
        case 0x0b: m_core.mr.mrx.mr0.s = val;                                             break;
        case 0x0c: m_core.mr.mrx.mr1.s = val; m_core.mr.mrx.mr2.s = (int16_t)val >> 15;  break;
        case 0x0d: m_core.mr.mrx.mr2.s = (int8_t)val;                                    break;
        case 0x0e: m_core.sr.srx.sr0.s = val;                                            break;
        case 0x0f: m_core.sr.srx.sr1.s = val;                                            break;
    }
}

static void write_reg1(ADSP2181 *cpu, int regnum, int32_t val)
{
    int index = regnum & 3;
    switch (regnum >> 2)
    {
        case 0:
            m_i[index] = val & 0x3fff;
            update_i(cpu, index);
            /* trace I1 writes */
            if (index == 1) {
                static uint32_t i1_cnt = 0;
                if (i1_cnt++ < 64)
                    fprintf(stderr, "[DAG] I1=0x%04X PC=%04X\n",
                            m_i[1], (unsigned)m_pc);
            }
            break;
        case 1:
            m_m[index] = sext(val, 14);
            break;
        case 2:
            m_l[index] = val & 0x3fff;
            update_l(cpu, index);
            /* trace L1 writes */
            if (index == 1) {
                static uint32_t l1_cnt = 0;
                if (l1_cnt++ < 64)
                    fprintf(stderr, "[DAG] L1=0x%04X PC=%04X\n",
                            m_l[1], (unsigned)m_pc);
            }
            break;
        case 3: /* ADSP-2181 overlay registers */
            switch (index)
            {
                case 2: m_pmovlay = val & 0x3fff;                        break;
                case 3: m_dmovlay = val & 0x3fff; update_dmovlay(cpu);   break;
                default: break;
            }
            break;
    }
}

static void write_reg2(ADSP2181 *cpu, int regnum, int32_t val)
{
    int index = 4 + (regnum & 3);
    switch (regnum >> 2)
    {
        case 0:
            m_i[index] = val & 0x3fff;
            update_i(cpu, index);
            /* trace I4..I7 writes */
            {
                static uint32_t i4_cnt = 0;
                if (i4_cnt++ < 128)
                    fprintf(stderr, "[DAG] I%d=0x%04X PC=%04X\n",
                            index, m_i[index], (unsigned)m_pc);
            }
            break;
        case 1:
            m_m[index] = sext(val, 14);
            {
                static uint32_t m4_cnt = 0;
                if (m4_cnt++ < 128)
                    fprintf(stderr, "[DAG] M%d=%d PC=%04X\n",
                            index, m_m[index], (unsigned)m_pc);
            }
            break;
        case 2:
            m_l[index] = val & 0x3fff;
            update_l(cpu, index);
            {
                static uint32_t l4_cnt = 0;
                if (l4_cnt++ < 128)
                    fprintf(stderr, "[DAG] L%d=0x%04X PC=%04X\n",
                            index, m_l[index], (unsigned)m_pc);
            }
            break;
        default: break;
    }
}

static void write_reg3(ADSP2181 *cpu, int regnum, int32_t val)
{
    switch (regnum)
    {
        case 0x00: m_astat = val & 0x00ff;                                   break;
        case 0x01: m_mstat = val & m_mstat_mask; update_mstat(cpu);          break;
        case 0x03: m_imask = val & m_imask_mask; check_irqs(cpu);            break;
        case 0x04: m_icntl = val & 0x001f;       check_irqs(cpu);            break;
        case 0x05: cntr_stack_push(cpu); m_cntr = val & 0x3fff;
            /* log CNTR sets outside of boot range 0x3800..0x38FF */
            if (m_pc < 0x3800 || m_pc > 0x38FF) {
                static uint32_t cntr_set = 0;
                fprintf(stderr, "[CNTR] CNTR=%04X PC=%04X (#%u)\n",
                        m_cntr, (unsigned)m_pc, ++cntr_set);
            }
            break;
        case 0x06: m_core.sb.s = sext(val, 5);                              break;
        case 0x07: m_px = val;                                               break;
        case 0x09: {
            static uint32_t tx0_cnt = 0;
            if (tx0_cnt++ < 16 || (tx0_cnt % 44100) == 0)
                fprintf(stderr, "[TX0] TX0=%04X PC=%04X (#%u)\n",
                        (unsigned)(uint16_t)val, (unsigned)m_pc, tx0_cnt);
            m_sport_tx_cb(0, val, 0xffff);
            break;
        }
        case 0x0b: m_sport_tx_cb(1, val, 0xffff);                           break;
        case 0x0c:
            m_ifc = val;
            { static uint32_t ifc_cnt=0; if(ifc_cnt++<32) fprintf(stderr,"[ADSP] IFC=%04X (BDMA_latch=%d) PC=%04X\n", val, (int)m_irq_latch[ADSP2181_BDMA], (unsigned)m_pc); }
            /* ADSP-2181 IFC bits: clear latches */
            if (BIT(val,  1)) m_irq_latch[ADSP2181_IRQ0]     = 0;
            if (BIT(val,  2)) m_irq_latch[ADSP2181_IRQ1]     = 0;
            if (BIT(val,  3)) m_irq_latch[ADSP2181_BDMA]     = 0;
            if (BIT(val,  4)) m_irq_latch[ADSP2181_IRQE]     = 0;
            if (BIT(val,  5)) m_irq_latch[ADSP2181_SPORT0_RX]= 0;
            if (BIT(val,  6)) m_irq_latch[ADSP2181_SPORT0_TX]= 0;
            if (BIT(val,  7)) m_irq_latch[ADSP2181_IRQ2]     = 0;
            /* force latches */
            if (BIT(val,  9)) m_irq_latch[ADSP2181_IRQ0]     = 1;
            if (BIT(val, 10)) m_irq_latch[ADSP2181_IRQ1]     = 1;
            if (BIT(val, 11)) m_irq_latch[ADSP2181_BDMA]     = 1;
            if (BIT(val, 12)) m_irq_latch[ADSP2181_IRQE]     = 1;
            if (BIT(val, 13)) m_irq_latch[ADSP2181_SPORT0_RX]= 1;
            if (BIT(val, 14)) m_irq_latch[ADSP2181_SPORT0_TX]= 1;
            if (BIT(val, 15)) m_irq_latch[ADSP2181_IRQ2]     = 1;
            check_irqs(cpu);
            break;
        case 0x0d: m_cntr = val & 0x3fff;              break;
        case 0x0f: pc_stack_push_val(cpu, val & 0x3fff); break;
        default: break;
    }
}

static int32_t read_reg0(ADSP2181 *cpu, int regnum)
{
    return *m_read0_ptr[regnum];
}

static int32_t read_reg1(ADSP2181 *cpu, int regnum)
{
    if (regnum == 0xe) return (int32_t)m_pmovlay;
    if (regnum == 0xf) return (int32_t)m_dmovlay;
    return (int32_t)*m_read1_ptr[regnum];
}

static int32_t read_reg2(ADSP2181 *cpu, int regnum)
{
    return (int32_t)*m_read2_ptr[regnum];
}

static int32_t read_reg3(ADSP2181 *cpu, int regnum)
{
    switch (regnum)
    {
        case 0x00: return m_astat;
        case 0x01: return m_mstat;
        case 0x02: return m_sstat;
        case 0x03: return m_imask;
        case 0x04: return m_icntl;
        case 0x05: return m_cntr;
        case 0x06: return m_core.sb.s;
        case 0x07: return m_px;
        case 0x08: return (int32_t)m_sport_rx_cb(0);
        case 0x0a: return (int32_t)m_sport_rx_cb(1);
        case 0x0f: return (int32_t)pc_stack_pop_val(cpu);
        default:   return 0;
    }
}

/* ── ALU operations ─────────────────────────────────────────────────────── */

/* Core ALU computation shared by ar/af/none variants.
   Returns the 32-bit result; also updates ASTAT flags. */
static int32_t alu_core(ADSP2181 *cpu, uint32_t op, bool use_const)
{
    int32_t xop = BIT(op, 8, 3);
    int32_t yop;
    int32_t res;

    if (use_const)
        yop = constants[BIT(op, 5, 3) | (BIT(op, 11, 2) << 3)];
    else
        yop = BIT(op, 11, 2);

    switch (BIT(op, 13, 4))
    {
        case 0x00: /* Y */
            res = use_const ? yop : (int32_t)ALU_GETYREG_UNSIGNED(yop);
            CALC_NZ(res);
            break;
        case 0x01: /* Y + 1 */
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = yop + 1; CALC_NZ(res);
            if (yop == 0x7fff) SET_V; else if ((uint32_t)yop == 0xffff) SET_C;
            break;
        case 0x02: /* X + Y + C */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            yop += GET_C >> 3; res = xop + yop; CALC_NZVC(xop, yop, res); break;
        case 0x03: /* X + Y */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = xop + yop; CALC_NZVC(xop, yop, res); break;
        case 0x04: /* NOT Y */
            res = (use_const ? yop : (int32_t)ALU_GETYREG_UNSIGNED(yop)) ^ 0xffff;
            CALC_NZ(res); break;
        case 0x05: /* -Y */
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = -yop; CALC_NZ(res);
            if (yop == 0x8000) SET_V; if (yop == 0x0000) SET_C; break;
        case 0x06: /* X - Y + C - 1 */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = xop - yop + (GET_C >> 3) - 1; CALC_NZVC_SUB(xop, yop, res); break;
        case 0x07: /* X - Y */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = xop - yop; CALC_NZVC_SUB(xop, yop, res); break;
        case 0x08: /* Y - 1 */
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = yop - 1; CALC_NZ(res);
            if (yop == 0x8000) SET_V; else if (yop == 0x0000) SET_C; break;
        case 0x09: /* Y - X */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = yop - xop; CALC_NZVC_SUB(yop, xop, res); break;
        case 0x0a: /* Y - X + C - 1 */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = yop - xop + (GET_C >> 3) - 1; CALC_NZVC_SUB(yop, xop, res); break;
        case 0x0b: /* NOT X */
            res = ALU_GETXREG_UNSIGNED(xop) ^ 0xffff; CALC_NZ(res); break;
        case 0x0c: /* X AND Y */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = xop & yop; CALC_NZ(res); break;
        case 0x0d: /* X OR Y */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = xop | yop; CALC_NZ(res); break;
        case 0x0e: /* X XOR Y */
            xop = ALU_GETXREG_UNSIGNED(xop);
            if (!use_const) yop = ALU_GETYREG_UNSIGNED(yop);
            res = xop ^ yop; CALC_NZ(res); break;
        case 0x0f: /* ABS X */
            xop = ALU_GETXREG_UNSIGNED(xop);
            res = (xop & 0x8000) ? -xop : xop;
            CLR_FLAGS; CLR_S;
            if (xop == 0) SET_Z;
            if (xop == 0x8000) { SET_N; SET_V; }
            if (xop & 0x8000) SET_S;
            break;
        default: res = 0; break;
    }
    return res;
}

static void alu_op_ar(ADSP2181 *cpu, uint32_t op)
{
    int32_t res = alu_core(cpu, op, false);
    if ((m_mstat & MSTAT_SATURATE) && GET_V) res = GET_C ? -32768 : 32767;
    m_core.ar.u = (uint16_t)res;
}

static void alu_op_ar_const(ADSP2181 *cpu, uint32_t op)
{
    int32_t res = alu_core(cpu, op, true);
    if ((m_mstat & MSTAT_SATURATE) && GET_V) res = GET_C ? -32768 : 32767;
    m_core.ar.u = (uint16_t)res;
}

static void alu_op_af(ADSP2181 *cpu, uint32_t op)
{
    m_core.af.u = (uint16_t)alu_core(cpu, op, false);
}

static void alu_op_af_const(ADSP2181 *cpu, uint32_t op)
{
    m_core.af.u = (uint16_t)alu_core(cpu, op, true);
}

static void alu_op_none(ADSP2181 *cpu, uint32_t op)
{
    alu_core(cpu, op, false); /* flags updated, result discarded */
}

/* ── MAC operations ─────────────────────────────────────────────────────── */

/* Returns 64-bit MAC result for opcode (xop=x reg index, yop=y reg index or same as xop).
   'xop_eq_yop' = true for the _xop variants (square). */
static int64_t mac_core(ADSP2181 *cpu, uint32_t op, bool xop_eq_yop)
{
    int8_t  shift = (m_mstat & MSTAT_INTEGER) ? 0 : 1;
    int32_t xi    = BIT(op, 8, 3);
    int32_t yi    = BIT(op, 11, 2);
    int32_t temp;
    int64_t res;

    switch (BIT(op, 13, 4))
    {
        case 0x00: return INT64_MIN; /* no-op sentinel */
        case 0x01: /* X*Y (RND) */
        {
            int32_t xv = MAC_GETXREG_SIGNED(xi);
            int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);
            temp = (xv * yv) << shift; res = (int64_t)temp;
            temp &= 0xffff; res += 0x8000;
            if (temp == 0x8000) res &= ~((uint64_t)0x10000);
            break;
        }
        case 0x02: /* MR + X*Y (RND) */
        {
            int32_t xv = MAC_GETXREG_SIGNED(xi);
            int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);
            temp = (xv * yv) << shift; res = m_core.mr.mr + (int64_t)temp;
            temp &= 0xffff; res += 0x8000;
            if (temp == 0x8000) res &= ~((uint64_t)0x10000);
            break;
        }
        case 0x03: /* MR - X*Y (RND) */
        {
            int32_t xv = MAC_GETXREG_SIGNED(xi);
            int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);
            temp = (xv * yv) << shift; res = m_core.mr.mr - (int64_t)temp;
            temp &= 0xffff; res += 0x8000;
            if (temp == 0x8000) res &= ~((uint64_t)0x10000);
            break;
        }
        case 0x04: { int32_t xv = MAC_GETXREG_SIGNED(xi);   int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);   temp=(xv*yv)<<shift; res=(int64_t)temp; break; }
        case 0x05: { int32_t xv = MAC_GETXREG_SIGNED(xi);   int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_UNSIGNED(yi); temp=(xv*yv)<<shift; res=(int64_t)temp; break; }
        case 0x06: { int32_t xv = MAC_GETXREG_UNSIGNED(xi); int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);   temp=(xv*yv)<<shift; res=(int64_t)temp; break; }
        case 0x07: { int32_t xv = MAC_GETXREG_UNSIGNED(xi); int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_UNSIGNED(yi); temp=(xv*yv)<<shift; res=(int64_t)temp; break; }
        case 0x08: { int32_t xv = MAC_GETXREG_SIGNED(xi);   int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);   temp=(xv*yv)<<shift; res=m_core.mr.mr+(int64_t)temp; break; }
        case 0x09: { int32_t xv = MAC_GETXREG_SIGNED(xi);   int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_UNSIGNED(yi); temp=(xv*yv)<<shift; res=m_core.mr.mr+(int64_t)temp; break; }
        case 0x0a: { int32_t xv = MAC_GETXREG_UNSIGNED(xi); int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);   temp=(xv*yv)<<shift; res=m_core.mr.mr+(int64_t)temp; break; }
        case 0x0b: { int32_t xv = MAC_GETXREG_UNSIGNED(xi); int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_UNSIGNED(yi); temp=(xv*yv)<<shift; res=m_core.mr.mr+(int64_t)temp; break; }
        case 0x0c: { int32_t xv = MAC_GETXREG_SIGNED(xi);   int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);   temp=(xv*yv)<<shift; res=m_core.mr.mr-(int64_t)temp; break; }
        case 0x0d: { int32_t xv = MAC_GETXREG_SIGNED(xi);   int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_UNSIGNED(yi); temp=(xv*yv)<<shift; res=m_core.mr.mr-(int64_t)temp; break; }
        case 0x0e: { int32_t xv = MAC_GETXREG_UNSIGNED(xi); int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_SIGNED(yi);   temp=(xv*yv)<<shift; res=m_core.mr.mr-(int64_t)temp; break; }
        case 0x0f: { int32_t xv = MAC_GETXREG_UNSIGNED(xi); int32_t yv = xop_eq_yop ? xv : MAC_GETYREG_UNSIGNED(yi); temp=(xv*yv)<<shift; res=m_core.mr.mr-(int64_t)temp; break; }
        default:   res = 0; break;
    }
    return res;
}

static void mac_op_mr(ADSP2181 *cpu, uint32_t op)
{
    int64_t res = mac_core(cpu, op, false);
    if (res == INT64_MIN) return; /* no-op */
    int32_t t = (int32_t)BIT(res, 31, 9);
    CLR_MV;
    if (t != 0x000 && t != 0x1ff) SET_MV;
    m_core.mr.mr = res;
}

static void mac_op_mr_xop(ADSP2181 *cpu, uint32_t op)
{
    int64_t res = mac_core(cpu, op, true);
    if (res == INT64_MIN) return;
    int32_t t = (int32_t)BIT(res, 31, 9);
    CLR_MV;
    if (t != 0x000 && t != 0x1ff) SET_MV;
    m_core.mr.mr = res;
}

static void mac_op_mf(ADSP2181 *cpu, uint32_t op)
{
    int64_t res = mac_core(cpu, op, false);
    if (res == INT64_MIN) return;
    m_core.mf.u = (uint16_t)((uint32_t)res >> 16);
}

static void mac_op_mf_xop(ADSP2181 *cpu, uint32_t op)
{
    int64_t res = mac_core(cpu, op, true);
    if (res == INT64_MIN) return;
    m_core.mf.u = (uint16_t)((uint32_t)res >> 16);
}

/* ── Shift operations ───────────────────────────────────────────────────── */

static void shift_op(ADSP2181 *cpu, uint32_t op)
{
    int8_t   sc  = m_core.se.s;
    int32_t  xi  = BIT(op, 8, 3);
    uint32_t res;

    switch (BIT(op, 11, 4))
    {
        case 0x00: xi=SHIFT_GETXREG_UNSIGNED(xi)<<16; res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr =res; break;
        case 0x01: xi=SHIFT_GETXREG_UNSIGNED(xi)<<16; res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr|=res; break;
        case 0x02: xi=SHIFT_GETXREG_UNSIGNED(xi);     res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr =res; break;
        case 0x03: xi=SHIFT_GETXREG_UNSIGNED(xi);     res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr|=res; break;
        case 0x04: xi=SHIFT_GETXREG_SIGNED(xi)<<16;   res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr =res; break;
        case 0x05: xi=SHIFT_GETXREG_SIGNED(xi)<<16;   res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr|=res; break;
        case 0x06: xi=SHIFT_GETXREG_SIGNED(xi);       res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr =res; break;
        case 0x07: xi=SHIFT_GETXREG_SIGNED(xi);       res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr|=res; break;
        case 0x08: /* NORM HI */
            xi=SHIFT_GETXREG_SIGNED(xi)<<16;
            if(sc>0){xi=((uint32_t)xi>>1)|((m_astat&CFLAG)<<28);res=(uint32_t)xi>>(sc-1);}
            else res=(sc>-32)?(xi<<-sc):0;
            m_core.sr.sr=res; break;
        case 0x09: /* NORM HI OR */
            xi=SHIFT_GETXREG_SIGNED(xi)<<16;
            if(sc>0){xi=((uint32_t)xi>>1)|((m_astat&CFLAG)<<28);res=(uint32_t)xi>>(sc-1);}
            else res=(sc>-32)?(xi<<-sc):0;
            m_core.sr.sr|=res; break;
        case 0x0a: xi=SHIFT_GETXREG_UNSIGNED(xi); res=(sc>0)?((sc<32)?(xi>>sc):0):((sc>-32)?(xi<<-sc):0); m_core.sr.sr =res; break;
        case 0x0b: xi=SHIFT_GETXREG_UNSIGNED(xi); res=(sc>0)?((sc<32)?(xi>>sc):0):((sc>-32)?(xi<<-sc):0); m_core.sr.sr|=res; break;
        case 0x0c: /* EXP HI */
        {
            int32_t xv=SHIFT_GETXREG_SIGNED(xi);
            if(xv<0){SET_SS;res=(uint32_t)count_leading_ones_32((uint32_t)xv)-16-1;}
            else    {CLR_SS;res=(uint32_t)count_leading_zeros_32((uint32_t)xv)-16-1;}
            m_core.se.s=-(int8_t)res; break;
        }
        case 0x0d: /* EXP HIX */
        {
            int32_t xv=SHIFT_GETXREG_SIGNED(xi);
            if(GET_V){m_core.se.s=1;if(xv<0)CLR_SS;else SET_SS;}
            else{if(xv<0){SET_SS;res=(uint32_t)count_leading_ones_32((uint32_t)xv)-16-1;}
                 else    {CLR_SS;res=(uint32_t)count_leading_zeros_32((uint32_t)xv)-16-1;}
                 m_core.se.s=-(int8_t)res;}
            break;
        }
        case 0x0e: /* EXP LO */
            if(m_core.se.s==-15){
                int32_t xv=SHIFT_GETXREG_SIGNED(xi);
                res=count_leading_zeros_32((uint32_t)((GET_SS?~xv:xv)&0xffff))-1;
                m_core.se.s=-(int8_t)res;
            }
            break;
        case 0x0f: /* EXPADJ */
        {
            int32_t xv=SHIFT_GETXREG_SIGNED(xi);
            res=count_leading_zeros_32((uint32_t)((xv<0)?~xv:xv))-16-1;
            if((int32_t)res < -(int32_t)m_core.sb.s) m_core.sb.s=-(int8_t)res;
            break;
        }
    }
}

static void shift_op_imm(ADSP2181 *cpu, uint32_t op)
{
    int8_t   sc  = (int8_t)op;
    int32_t  xi  = BIT(op, 8, 3);
    uint32_t res;

    switch (BIT(op, 11, 4))
    {
        case 0x00: xi=SHIFT_GETXREG_UNSIGNED(xi)<<16; res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr =res; break;
        case 0x01: xi=SHIFT_GETXREG_UNSIGNED(xi)<<16; res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr|=res; break;
        case 0x02: xi=SHIFT_GETXREG_UNSIGNED(xi);     res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr =res; break;
        case 0x03: xi=SHIFT_GETXREG_UNSIGNED(xi);     res=(sc>0)?((sc<32)?(uint32_t(xi)<<sc):0):((sc>-32)?(uint32_t(xi)>>-sc):0); m_core.sr.sr|=res; break;
        case 0x04: xi=SHIFT_GETXREG_SIGNED(xi)<<16;   res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr =res; break;
        case 0x05: xi=SHIFT_GETXREG_SIGNED(xi)<<16;   res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr|=res; break;
        case 0x06: xi=SHIFT_GETXREG_SIGNED(xi);       res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr =res; break;
        case 0x07: xi=SHIFT_GETXREG_SIGNED(xi);       res=(sc>0)?((sc<32)?(xi<<sc):0):((sc>-32)?(xi>>-sc):(xi>>31));              m_core.sr.sr|=res; break;
        case 0x08: /* NORM HI */
            xi=SHIFT_GETXREG_SIGNED(xi)<<16;
            if(sc>0){xi=((uint32_t)xi>>1)|((m_astat&CFLAG)<<28);res=(uint32_t)xi>>(sc-1);}
            else res=(sc>-32)?(xi<<-sc):0;
            m_core.sr.sr=res; break;
        case 0x09:
            xi=SHIFT_GETXREG_SIGNED(xi)<<16;
            if(sc>0){xi=((uint32_t)xi>>1)|((m_astat&CFLAG)<<28);res=(uint32_t)xi>>(sc-1);}
            else res=(sc>-32)?(xi<<-sc):0;
            m_core.sr.sr|=res; break;
        case 0x0a: xi=SHIFT_GETXREG_UNSIGNED(xi); res=(sc>0)?((sc<32)?(xi>>sc):0):((sc>-32)?(xi<<-sc):0); m_core.sr.sr =res; break;
        case 0x0b: xi=SHIFT_GETXREG_UNSIGNED(xi); res=(sc>0)?((sc<32)?(xi>>sc):0):((sc>-32)?(xi<<-sc):0); m_core.sr.sr|=res; break;
    }
}

/* ── IRQ handling ───────────────────────────────────────────────────────── */

static bool generate_irq(ADSP2181 *cpu, int which, int indx)
{
    if (!(m_imask & (0x200 >> indx)))
        return false;

    m_irq_latch[which] = 0;
    pc_stack_push(cpu);
    stat_stack_push(cpu);

    m_pc   = 0x04 + indx * 4;
    m_idle = 0;
    if (which == ADSP2181_IRQL0) {
        static uint32_t irql0_count = 0;
        irql0_count++;
        if (irql0_count <= 10) {
            /* Arm per-ISR execution trace for the first 10 invocations */
            g_isr_trace_remaining = 300;
            g_isr_trace_count     = (int)irql0_count;
            fprintf(stderr, "[ADSP] IRQL0 ISR fired #%u → entry PC=%04X imask=%04X\n",
                    irql0_count, (unsigned)m_pc, (unsigned)m_imask);
        }
    }
    if (which == ADSP2181_SPORT0_TX) {
        static uint32_t sport_tx_count = 0;
        sport_tx_count++;
        /* Log first 5 fires, then once per second (~44100 fires) */
        if (sport_tx_count <= 5 || (sport_tx_count % 44100) == 0) {
            fprintf(stderr, "[ADSP] SPORT0_TX ISR #%u → PC=%04X CNTR=%u loop=%04X loop_cond=%u PM[001C]=%06X imask=%04X\n",
                    sport_tx_count, (unsigned)m_pc,
                    (unsigned)cpu->cntr,
                    (unsigned)cpu->loop,
                    (unsigned)cpu->loop_condition,
                    (unsigned)cpu->pm[0x001C],
                    (unsigned)m_imask);
        }
    }
    if (which == ADSP2181_BDMA) {
        static uint32_t bdma_count = 0;
        if (bdma_count++ < 16)
            fprintf(stderr, "[ADSP] BDMA ISR fired #%u → PC=%04X imask=%04X\n",
                    bdma_count, (unsigned)m_pc, (unsigned)m_imask);
    }

    if (m_icntl & 0x10) m_imask &= ~(0x3ff >> indx);
    else                m_imask &= ~0x3ff;

    return true;
}

static void check_irqs(ADSP2181 *cpu)
{
    uint8_t check;

    /* Priority order from MAME adsp2181_device::check_irqs */
    check = (m_icntl & 4) ? m_irq_latch[ADSP2181_IRQ2]     : m_irq_state[ADSP2181_IRQ2];
    if (check && generate_irq(cpu, ADSP2181_IRQ2, 0))     return;

    check = m_irq_state[ADSP2181_IRQL1];
    if (check && generate_irq(cpu, ADSP2181_IRQL1, 1))    return;

    check = m_irq_state[ADSP2181_IRQL0];
    if (check && generate_irq(cpu, ADSP2181_IRQL0, 2))    return;

    check = m_irq_latch[ADSP2181_SPORT0_TX];
    if (check && generate_irq(cpu, ADSP2181_SPORT0_TX, 3)) return;

    check = m_irq_latch[ADSP2181_SPORT0_RX];
    if (check && generate_irq(cpu, ADSP2181_SPORT0_RX, 4)) return;

    check = m_irq_latch[ADSP2181_IRQE];
    if (check && generate_irq(cpu, ADSP2181_IRQE, 5))     return;

    check = m_irq_latch[ADSP2181_BDMA];
    if (check && generate_irq(cpu, ADSP2181_BDMA, 6))     return;

    check = (m_icntl & 2) ? m_irq_latch[ADSP2181_IRQ1]   : m_irq_state[ADSP2181_IRQ1];
    if (check && generate_irq(cpu, ADSP2181_IRQ1, 7))     return;

    check = (m_icntl & 1) ? m_irq_latch[ADSP2181_IRQ0]   : m_irq_state[ADSP2181_IRQ0];
    if (check && generate_irq(cpu, ADSP2181_IRQ0, 8))     return;

    check = m_irq_latch[ADSP2181_TIMER];
    if (check && generate_irq(cpu, ADSP2181_TIMER, 9))    return;
}

/* ── Lookup table creation ──────────────────────────────────────────────── */

static void create_tables(ADSP2181 *cpu)
{
    /* bit-reverse table */
    for (int i = 0; i < 0x4000; i++)
    {
        uint16_t d = 0;
        d |= (i >> 13) & 0x0001; d |= (i >> 11) & 0x0002;
        d |= (i >>  9) & 0x0004; d |= (i >>  7) & 0x0008;
        d |= (i >>  5) & 0x0010; d |= (i >>  3) & 0x0020;
        d |= (i >>  1) & 0x0040; d |= (i <<  1) & 0x0080;
        d |= (i <<  3) & 0x0100; d |= (i <<  5) & 0x0200;
        d |= (i <<  7) & 0x0400; d |= (i <<  9) & 0x0800;
        d |= (i << 11) & 0x1000; d |= (i << 13) & 0x2000;
        m_reverse_table[i] = d;
    }

    /* modulo-address mask table */
    for (int i = 0; i < 0x4000; i++)
    {
        if      (i > 0x2000) m_mask_table[i] = 0x0000;
        else if (i > 0x1000) m_mask_table[i] = 0x2000;
        else if (i > 0x0800) m_mask_table[i] = 0x3000;
        else if (i > 0x0400) m_mask_table[i] = 0x3800;
        else if (i > 0x0200) m_mask_table[i] = 0x3c00;
        else if (i > 0x0100) m_mask_table[i] = 0x3e00;
        else if (i > 0x0080) m_mask_table[i] = 0x3f00;
        else if (i > 0x0040) m_mask_table[i] = 0x3f80;
        else if (i > 0x0020) m_mask_table[i] = 0x3fc0;
        else if (i > 0x0010) m_mask_table[i] = 0x3fe0;
        else if (i > 0x0008) m_mask_table[i] = 0x3ff0;
        else if (i > 0x0004) m_mask_table[i] = 0x3ff8;
        else if (i > 0x0002) m_mask_table[i] = 0x3ffc;
        else if (i > 0x0001) m_mask_table[i] = 0x3ffe;
        else                 m_mask_table[i] = 0x3fff;
    }

    /* condition table */
    for (int i = 0; i < 0x100; i++)
    {
        int az = ((i & ZFLAG)  != 0);
        int an = ((i & NFLAG)  != 0);
        int av = ((i & VFLAG)  != 0);
        int ac = ((i & CFLAG)  != 0);
        int mv = ((i & MVFLAG) != 0);
        int as = ((i & SFLAG)  != 0);

        m_condition_table[i | 0x000] = az;
        m_condition_table[i | 0x100] = !az;
        m_condition_table[i | 0x200] = !((an ^ av) | az);
        m_condition_table[i | 0x300] = (an ^ av) | az;
        m_condition_table[i | 0x400] = an ^ av;
        m_condition_table[i | 0x500] = !(an ^ av);
        m_condition_table[i | 0x600] = av;
        m_condition_table[i | 0x700] = !av;
        m_condition_table[i | 0x800] = ac;
        m_condition_table[i | 0x900] = !ac;
        m_condition_table[i | 0xa00] = as;
        m_condition_table[i | 0xb00] = !as;
        m_condition_table[i | 0xc00] = mv;
        m_condition_table[i | 0xd00] = !mv;
        m_condition_table[i | 0xf00] = 1;
    }
}

/* ── Execute loop ───────────────────────────────────────────────────────── */

static void execute_run(ADSP2181 *cpu)
{
    check_irqs(cpu);

    do {
        m_ppc = m_pc;

        /* Track unique PCs visited in main-loop range 0x0050..0x37FF (not boot) */
        {
            static uint8_t pc_seen[0x3800] = {};
            if (m_pc >= 0x0050 && m_pc < 0x3800 && !pc_seen[m_pc]) {
                pc_seen[m_pc] = 1;
                fprintf(stderr, "[PC_NEW] %04X op=%06X\n", (unsigned)m_pc, cpu->pm[m_pc & 0x3fff]);
            }
        }

        if (g_isr_trace_remaining > 0) {
            fprintf(stderr, "[ISR%d] PC=%04X op=%06X AR=%04X AX0=%04X AX1=%04X imask=%03X\n",
                    g_isr_trace_count,
                    m_pc, cpu->pm[m_pc & 0x3fff],
                    (unsigned)(uint16_t)m_core.ar.s,
                    (unsigned)(uint16_t)m_core.ax0.s,
                    (unsigned)(uint16_t)m_core.ax1.s,
                    (unsigned)m_imask);
            g_isr_trace_remaining--;
        }

        /* Dump PM[0x2E90..0x2EBF] and I/L/M regs on first entry to load/zero area */
        {
            static bool pm2e9_dumped = false;
            if (!pm2e9_dumped && m_pc == 0x2E9C) {
                pm2e9_dumped = true;
                fprintf(stderr, "[PM2E9] PM[2E90..2EBF]:\n");
                for (int _p = 0x2E90; _p <= 0x2EBF; _p++)
                    fprintf(stderr, "[PM2E9]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
                fprintf(stderr, "[PM2E9] I0..I7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                        m_i[0], m_i[1], m_i[2], m_i[3], m_i[4], m_i[5], m_i[6], m_i[7]);
                fprintf(stderr, "[PM2E9] L0..L7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                        m_l[0], m_l[1], m_l[2], m_l[3], m_l[4], m_l[5], m_l[6], m_l[7]);
                fprintf(stderr, "[PM2E9] M0..M7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                        m_m[0], m_m[1], m_m[2], m_m[3], m_m[4], m_m[5], m_m[6], m_m[7]);
            }
        }

        /* One-time PM dump of synthesis DO loop area at first entry */
        {
            static bool syn_pm_dumped = false;
            if (!syn_pm_dumped && m_pc == 0x00AC) {
                syn_pm_dumped = true;
                fprintf(stderr, "[SYN_PM] PM[00A0..00CF]:\n");
                for (int _p = 0x00A0; _p <= 0x00CF; _p++)
                    fprintf(stderr, "[SYN_PM]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
                fprintf(stderr, "[SYN_PM] I0..I7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                        m_i[0], m_i[1], m_i[2], m_i[3], m_i[4], m_i[5], m_i[6], m_i[7]);
                fprintf(stderr, "[SYN_PM] L0..L7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                        m_l[0], m_l[1], m_l[2], m_l[3], m_l[4], m_l[5], m_l[6], m_l[7]);
                fprintf(stderr, "[SYN_PM] M0..M7: %04X %04X %04X %04X %04X %04X %04X %04X\n",
                        m_m[0], m_m[1], m_m[2], m_m[3], m_m[4], m_m[5], m_m[6], m_m[7]);
            }
        }

        /* trace I4 at DO loop start — every invocation, compact */
        {
            static uint32_t do_loop_cnt = 0;
            if (m_pc == 0x00AC) {
                do_loop_cnt++;
                uint16_t i4 = m_i[4];
                /* Print first 4, then every 5th around loop 30 */
                if (do_loop_cnt <= 4 || (do_loop_cnt >= 28 && do_loop_cnt <= 38))
                    fprintf(stderr, "[I4] DO #%u: I4=0x%04X I1=0x%04X pc_sp=%u\n",
                            do_loop_cnt, i4, m_i[1], (unsigned)m_pc_sp);
                /* Dump DAG2 context for calls #21-34 to compare synthesis vs post-synthesis */
                if (do_loop_cnt >= 21 && do_loop_cnt <= 34) {
                    uint16_t caller = (m_pc_sp > 0) ? m_pc_stack[m_pc_sp - 1] : 0xFFFF;
                    fprintf(stderr, "[COPY_DAG2] call #%u caller=%04X I4=%04X L4=%04X base4=%04X M5=%04X MSTAT=%02X\n",
                            do_loop_cnt, caller, i4,
                            (unsigned)m_l[4], (unsigned)m_base[4], (unsigned)m_m[5],
                            (unsigned)m_mstat);
                    /* Also dump PM around I4 */
                    fprintf(stderr, "[COPY_PM] call #%u I4=%04X:", do_loop_cnt, i4);
                    for (int _p = (int)i4; _p < (int)i4 + 8; _p++)
                        fprintf(stderr, " %04X=%06X", (unsigned)(_p & 0x3fff), cpu->pm[_p & 0x3fff]);
                    fprintf(stderr, "\n");
                }
            }
        }

        /* Trace copy subroutine: PM[0x008C] entry and I1 after DM[0x3465] load */
        {
            static uint32_t copy008c_cnt = 0;
            if (m_pc == 0x008C) { copy008c_cnt++; g_copy008c_per_sec++; }
            /* Trace I1 value just after DM[3465] is loaded: calls #21..#42 */
            if (m_pc == 0x008E && copy008c_cnt >= 21 && copy008c_cnt <= 42) {
                uint16_t dm3465 = (uint16_t)data_read(cpu, 0x3465);
                uint16_t caller = (m_pc_sp > 0) ? m_pc_stack[m_pc_sp - 1] : 0xFFFF;
                fprintf(stderr, "[COPY I1] call #%u: I1=%04X DM[3465]=%04X caller=%04X\n",
                        copy008c_cnt, m_i[1], dm3465, caller);
            }
        }

        /* trace main loop entry at PC=0x2176 */
        {
            static uint32_t main_loop_cnt = 0;
            if (m_pc == 0x2176) {
                main_loop_cnt++;
                g_mainloop_per_sec++;
                if (main_loop_cnt <= 8)
                    fprintf(stderr, "[MAIN] entry #%u I0=%04X I4=%04X I6=%04X AX0=%04X\n",
                            main_loop_cnt, m_i[0], m_i[4], m_i[6],
                            (unsigned)(uint16_t)m_core.ax0.s);
            }
        }

        /* trace ISR synthesis at PM[0x26BA] */
        {
            if (m_pc == 0x26BA) {
                g_synth26ba_per_sec++;
                static uint32_t cnt26ba = 0;
                cnt26ba++;
                if (cnt26ba <= 5) {
                    uint16_t caller = (m_pc_sp > 0) ? m_pc_stack[m_pc_sp - 1] : 0xFFFF;
                    fprintf(stderr, "[ISR_SYN] entry #%u caller=%04X I4=%04X IMASK=%04X AR=%04X\n",
                            cnt26ba, caller, (unsigned)m_i[4],
                            (unsigned)m_imask, (unsigned)(uint16_t)m_core.ar.u);
                }
            }
        }

        /* trace PM[0x001C] patch: any pgm_write to address 0x001C */
        /* (handled inside pgm_write_dag2 separately) */

        /* trace PM[0x001C] value when DO UNTIL reaches it (PC=0x001C) */
        {
            static uint32_t pm001c_cnt = 0;
            if (m_pc == 0x001C) {
                pm001c_cnt++;
                uint32_t op = cpu->pm[0x001C];
                if (pm001c_cnt <= 20 || (pm001c_cnt % 1000) == 0)
                    fprintf(stderr, "[PM001C] visit #%u PC=001C op=%06X CNTR=%u\n",
                            pm001c_cnt, op, (unsigned)m_cntr);
            }
        }

        /* Trace every instruction in PM[0x2E60..0x2EBF] outside inner DO loops, for first 100 + last 50 of DO */
        {
            static uint32_t synblk_cnt = 0;
            static bool in_do_loop = false;
            if (m_pc >= 0x2E60 && m_pc <= 0x2EBF) {
                bool is_do_inner = (m_pc >= 0x2EA9 && m_pc <= 0x2EAB);
                /* always trace non-inner instructions; for inner: first 12 and every 3rd when CNTR<=3 */
                bool do_trace = !is_do_inner
                    || synblk_cnt < 12
                    || m_cntr <= 3;
                synblk_cnt++;
                if (do_trace) {
                    uint32_t _op = cpu->pm[m_pc & 0x3fff];
                    fprintf(stderr, "[SYN_BLK] #%u PC=%04X op=%06X I4=%04X CNTR=%u loop=%04X\n",
                            synblk_cnt, (unsigned)m_pc, _op,
                            (unsigned)m_i[4], (unsigned)m_cntr, (unsigned)m_loop);
                }
            }
        }

        /* Once game is active, dump PM[0x008C..0x00CF] once to see runtime code */
        {
            static bool runtime_pm_dumped = false;
            if (!runtime_pm_dumped && g_game_started && m_pc == 0x2EA0) {
                runtime_pm_dumped = true;
                fprintf(stderr, "[RT_PM] PM[008C..00CF] after game start:\n");
                for (int _p = 0x008C; _p <= 0x00CF; _p++)
                    fprintf(stderr, "[RT_PM]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
                fprintf(stderr, "[RT_PM] MSTAT=%02X (BANK=%d)\n",
                        (unsigned)m_mstat, (m_mstat & MSTAT_BANK) ? 1 : 0);
            }
        }

        /* Per-instruction trace in synthesis inner loop PM[2E94..2E9A] for first 3 iters */
        {
            static uint32_t syn_insn_cnt = 0;
            if (m_pc >= 0x2E94 && m_pc <= 0x2E9B) {
                syn_insn_cnt++;
                if (syn_insn_cnt <= 24) {
                    uint32_t _op = cpu->pm[m_pc & 0x3fff];
                    fprintf(stderr, "[SYN_INSN] #%02u PC=%04X op=%06X AR=%04X AY0=%04X CNTR=%04X ASTAT=%02X\n",
                            syn_insn_cnt, (unsigned)m_pc, _op,
                            (unsigned)(uint16_t)m_core.ar.u,
                            (unsigned)(uint16_t)m_core.ay0.u,
                            (unsigned)m_cntr,
                            (unsigned)m_astat);
                }
            }
        }

        /* Trace AR at start of synthesis DO loop PM[0x2EA8] */
        {
            static uint32_t syn_do_cnt = 0;
            if (m_pc == 0x2EA8 && syn_do_cnt < 5) {
                syn_do_cnt++;
                fprintf(stderr, "[SYN_DO] #%u PC=2EA8 AR=%04X MR0=%04X MR1=%04X I4=%04X CNTR=%u\n",
                        syn_do_cnt,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr1.u,
                        (unsigned)m_i[4], (unsigned)m_cntr);
            }
        }

        /* Count synthesis subroutine entries per second, and log caller */
        if (m_pc == 0x2E90) {
            g_synth2e90_per_sec++;
            /* log caller PC (top of call stack) and CNTR, DM[3BDA] */
            uint16_t caller = (m_pc_sp > 0) ? m_pc_stack[m_pc_sp - 1] : 0xFFFF;
            uint16_t dm3bda = (uint16_t)data_read(cpu, 0x3BDA);
            fprintf(stderr, "[SYNTH] call #%u caller_pc=%04X CNTR=%u DM[3BDA]=%04X I4=%04X\n",
                    g_synth2e90_per_sec, caller, (unsigned)m_cntr, dm3bda, (unsigned)m_i[4]);
            /* dump PM around caller so we understand the outer loop */
            if (g_synth2e90_per_sec == 1) {
                uint16_t base_pc = (caller > 0x20) ? (caller - 0x20) : 0;
                fprintf(stderr, "[SYNTH_CTX] PM[%04X..%04X]:", base_pc, base_pc + 0x3F);
                for (int _p = base_pc; _p <= base_pc + 0x3F; _p++)
                    fprintf(stderr, " %04X=%06X", _p, cpu->pm[_p & 0x3fff]);
                fprintf(stderr, "\n");
                /* also dump the full synthesis subroutine from 0x2E3A so we can trace the flow */
                fprintf(stderr, "[SYNTH_CTX2] PM[2E3A..2E90]:\n");
                for (int _p = 0x2E3A; _p <= 0x2E90; _p++)
                    fprintf(stderr, "[SYNTH_CTX2]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
            }
        }

        /* Trace synthesis subroutine entry PM[0x2E3A]: log CNTR + key regs, first 20 hits */
        {
            static uint32_t syn_entry_cnt = 0;
            if (m_pc == 0x2E3A && syn_entry_cnt < 20) {
                syn_entry_cnt++;
                uint16_t dm3bda = (uint16_t)data_read(cpu, 0x3BDA);
                fprintf(stderr, "[SYN2E3A] #%u CNTR=%u AR=%04X AX0=%04X MR0=%04X DM[3BDA]=%04X I4=%04X I6=%04X\n",
                        syn_entry_cnt, (unsigned)m_cntr,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.ax0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        dm3bda,
                        (unsigned)m_i[4], (unsigned)m_i[6]);
            }
        }

        /* Trace voice-activator entry (PM[0x2E62] = CALL 0x215B always) and gate points */
        {
            static uint32_t vact_cnt = 0;
            /* PM[0x2E62]: voice activator entered */
            if (m_pc == 0x2E62 && vact_cnt < 30) {
                vact_cnt++;
                uint16_t dm3bd7 = (uint16_t)data_read(cpu, 0x3BD7);
                uint16_t dm3bd8 = (uint16_t)data_read(cpu, 0x3BD8);
                uint16_t dm3bda = (uint16_t)data_read(cpu, 0x3BDA);
                fprintf(stderr, "[VACT_IN] #%u AR=%04X MR0=%04X MR1=%04X DM[3BD7]=%04X DM[3BD8]=%04X DM[3BDA]=%04X\n",
                        vact_cnt,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr1.u,
                        dm3bd7, dm3bd8, dm3bda);
            }
        }
        /* PM[0x2E66]: first gate — AR = DM[0x3BD7]+MR0, IF LT RTS */
        {
            static uint32_t gate1_cnt = 0;
            if (m_pc == 0x2E66 && gate1_cnt < 20) {
                gate1_cnt++;
                fprintf(stderr, "[GATE1] #%u AR=%04X MR0=%04X AX0=%04X ASTAT=%02X (LT=%d → %s)\n",
                        gate1_cnt,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        (unsigned)(uint16_t)m_core.ax0.u,
                        (unsigned)m_astat,
                        (m_astat >> 1) & 1,   /* LT bit in ASTAT */
                        ((m_astat >> 1) & 1) ? "RTS" : "continue");
            }
        }
        /* PM[0x2E6F]: second gate — alu_op_none(SR0+MR0), IF LE RTS */
        {
            static uint32_t gate2_cnt = 0;
            if (m_pc == 0x2E6F && gate2_cnt < 20) {
                gate2_cnt++;
                uint16_t dm3bd8 = (uint16_t)data_read(cpu, 0x3BD8);
                fprintf(stderr, "[GATE2] #%u BEFORE: AR=%04X SR0=%04X MR0=%04X MR1=%04X DM[3BD8]=%04X ASTAT=%02X\n",
                        gate2_cnt,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.sr.srx.sr0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr1.u,
                        dm3bd8,
                        (unsigned)m_astat);
            }
        }
        /* PM[0x2E70]: gate 2 decision (IF LE RTS) */
        {
            static uint32_t gate2b_cnt = 0;
            if (m_pc == 0x2E70 && gate2b_cnt < 20) {
                gate2b_cnt++;
                /* ASTAT now reflects result of alu_op_none(SR0+MR0) at 0x2E6F */
                fprintf(stderr, "[GATE2B] #%u AFTER_ALUOP: ASTAT=%02X (AZ=%u AN=%u AV=%u) LE=%u → %s\n",
                        gate2b_cnt,
                        (unsigned)m_astat,
                        (m_astat & 0x01) ? 1 : 0,   /* AZ */
                        (m_astat & 0x02) ? 1 : 0,   /* AN */
                        (m_astat & 0x04) ? 1 : 0,   /* AV */
                        (((m_astat>>1)&1 ^ (m_astat>>2)&1) | (m_astat&1)) ? 1 : 0,
                        (((m_astat>>1)&1 ^ (m_astat>>2)&1) | (m_astat&1)) ? "RTS (skip synth)" : "continue to synth2E90");
            }
        }
        /* PM[0x2E03]: voice activator call site — check SR0+MR0 BEFORE alu_op_none */
        {
            static uint32_t e03_cnt = 0;
            if (m_pc == 0x2E03 && e03_cnt < 10) {
                e03_cnt++;
                fprintf(stderr, "[E03] #%u SR0=%04X MR0=%04X ASTAT=%02X (NE=%u → %s)\n",
                        e03_cnt,
                        (unsigned)(uint16_t)m_core.sr.srx.sr0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        (unsigned)m_astat,
                        ((m_astat & 0x01) == 0) ? 1 : 0,   /* NE = !AZ */
                        ((m_astat & 0x01) == 0) ? "CALL activator" : "skip");
            }
        }
        /* PM[0x2E10]: second voice activator call site */
        {
            static uint32_t e10_cnt = 0;
            if (m_pc == 0x2E10 && e10_cnt < 10) {
                e10_cnt++;
                uint16_t dm3bd4 = (uint16_t)data_read(cpu, 0x3BD4);
                fprintf(stderr, "[E10] #%u AR=%04X AY0=%04X SR0=%04X MR0=%04X ASTAT=%02X DM[3BD4]=%04X (NE=%u → %s)\n",
                        e10_cnt,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.ay0.u,
                        (unsigned)(uint16_t)m_core.sr.srx.sr0.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u,
                        (unsigned)m_astat,
                        dm3bd4,
                        ((m_astat & 0x01) == 0) ? 1 : 0,
                        ((m_astat & 0x01) == 0) ? "CALL activator" : "skip");
            }
        }
        /* PM[0x2DDE]: routing decision — DM[3BD4] check — and PM[0x2DE2]: DM[3BD5] check */
        {
            static uint32_t dde_cnt = 0;
            if (m_pc == 0x2DDE && dde_cnt < 6) {
                dde_cnt++;
                uint16_t dm3bd4 = (uint16_t)data_read(cpu, 0x3BD4);
                uint16_t dm3bd5 = (uint16_t)data_read(cpu, 0x3BD5);
                fprintf(stderr, "[DDE] #%u AR=%04X ASTAT=%02X DM[3BD4]=%04X DM[3BD5]=%04X\n",
                        dde_cnt, (unsigned)(uint16_t)m_core.ar.u, (unsigned)m_astat, dm3bd4, dm3bd5);
            }
        }

        /* Trace DM[0x3BDA] and CNTR at PM[2E9E] (loop count for CALL 008C) */
        {
            static uint32_t pm2e9e_cnt = 0;
            if (m_pc == 0x2E9E && pm2e9e_cnt < 8) {
                pm2e9e_cnt++;
                uint16_t dm3bda = (uint16_t)data_read(cpu, 0x3BDA);
                uint16_t dm3bd7 = (uint16_t)data_read(cpu, 0x3BD7);
                fprintf(stderr, "[2E9E] #%u DM[3BDA]=%04X DM[3BD7]=%04X I4=%04X\n",
                        pm2e9e_cnt, dm3bda, dm3bd7, (unsigned)m_i[4]);
            }
        }

        /* One-time PM dump of pf0 polling area on first entry to 0x2160..0x2170 */
        {
            static bool pf0_pm_dumped = false;
            if (!pf0_pm_dumped && m_pc >= 0x2160 && m_pc <= 0x2170) {
                pf0_pm_dumped = true;
                fprintf(stderr, "[PF0_PM] PM[2155..2195] (pf0 polling area):\n");
                for (int _p = 0x2155; _p <= 0x2195; _p++)
                    fprintf(stderr, "[PF0_PM]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
                fprintf(stderr, "[PF0_PM] AX0=%04X AR=%04X ASTAT=%02X MSTAT=%02X\n",
                        (unsigned)(uint16_t)m_core.ax0.u,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)m_astat, (unsigned)m_mstat);
            }
        }

        /* Per-instruction trace for pf0 polling area 0x2160..0x2175 (first 48 hits) */
        {
            static uint32_t pf0_insn_cnt = 0;
            if (m_pc >= 0x2160 && m_pc <= 0x2175 && pf0_insn_cnt < 48) {
                pf0_insn_cnt++;
                uint32_t _op = cpu->pm[m_pc & 0x3fff];
                fprintf(stderr, "[PF0_INSN] #%u PC=%04X op=%06X AX0=%04X AR=%04X ASTAT=%02X fl0=%u pf0=%u\n",
                        pf0_insn_cnt, (unsigned)m_pc, _op,
                        (unsigned)(uint16_t)m_core.ax0.u,
                        (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)m_astat,
                        (unsigned)cpu->fl0,
                        (unsigned)cpu->fl0);  /* placeholder until we know real pf0 addr */
            }
        }

        /* One-time dump of runtime PM[0x2655..0x26C0] (voice-count gate + synthesis dispatcher) */
        {
            static bool synth_pm_dumped = false;
            if (!synth_pm_dumped && m_pc == 0x266A) {
                synth_pm_dumped = true;
                fprintf(stderr, "[SYNGATE_PM] PM[2655..26C0] runtime dump:\n");
                for (int _p = 0x2655; _p <= 0x26C0; _p++)
                    fprintf(stderr, "[SYNGATE_PM]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
            }
        }

        /* Trace AR/AY0 at PM[0x266A] (voice-count gate: AR-AY0, skip if AZ) */
        {
            static uint32_t syngate_cnt = 0;
            if (m_pc == 0x266A) {
                uint16_t dm3467 = (uint16_t)data_read(cpu, 0x3467);
                uint16_t i1val  = (uint16_t)m_i[1];
                bool cmd_present = (dm3467 != i1val);
                /* Log first 20 always; after that log every time commands are present */
                bool do_log = (syngate_cnt < 20) || (cmd_present && syngate_cnt < 100);
                if (do_log) {
                    syngate_cnt++;
                    uint16_t dm3464 = (uint16_t)data_read(cpu, 0x3464);
                    fprintf(stderr, "[SYNGATE] #%u AR=%04X AY0(I1)=%04X I1=%04X DM[3467]=%04X %s DM[3464]=%04X ASTAT=%02X\n",
                            syngate_cnt,
                            (unsigned)(uint16_t)m_core.ar.u,
                            (unsigned)(uint16_t)m_core.ay0.u,
                            i1val, dm3467,
                            cmd_present ? "CMD_PRESENT" : "no_cmd",
                            dm3464, (unsigned)m_astat);
                } else if (!do_log) {
                    syngate_cnt++;
                }
            }
        }

        /* Trace synthesis gate commands-present path (PM[0x266C] entered) */
        {
            static uint32_t cmd266c_cnt = 0;
            if (m_pc == 0x266C) {
                cmd266c_cnt++;
                g_syngate_cmd_per_sec++;
                if (cmd266c_cnt <= 30) {
                    uint16_t dm3467 = (uint16_t)data_read(cpu, 0x3467);
                    uint16_t dm3400 = (uint16_t)data_read(cpu, 0x3400);
                    uint16_t dm3401 = (uint16_t)data_read(cpu, 0x3401);
                    fprintf(stderr, "[SYNGATE_CMDPATH] #%u I0=%04X I1=%04X I2=%04X I6=%04X DM[3467]=%04X DM[3400]=%04X DM[3401]=%04X AR=%04X\n",
                            cmd266c_cnt,
                            (unsigned)m_i[0], (unsigned)m_i[1],
                            (unsigned)m_i[2], (unsigned)m_i[6],
                            dm3467, dm3400, dm3401,
                            (unsigned)(uint16_t)m_core.ar.u);
                }
            }
        }

        /* Trace synthesis gate no-commands path (PM[0x26A8] entered) */
        {
            static uint32_t nocmd26a8_cnt = 0;
            if (m_pc == 0x26A8) {
                if (nocmd26a8_cnt++ < 5)
                    fprintf(stderr, "[SYNGATE_NOCMD] #%u I1=%04X DM[3467]=%04X\n",
                            nocmd26a8_cnt, (unsigned)m_i[1],
                            (uint16_t)data_read(cpu, 0x3467));
            }
        }

        /* One-time dump of PM[0x2DA3..0x2E3A] — full synthesis main + voice scheduler */
        {
            static bool synmain_dumped = false;
            if (!synmain_dumped && m_pc == 0x2DA3) {
                synmain_dumped = true;
                fprintf(stderr, "[SYNMAIN_PM] PM[2DA3..2E3A] complete runtime dump:\n");
                for (int _p = 0x2DA3; _p <= 0x2E3A; _p++)
                    fprintf(stderr, "[SYNMAIN_PM]  PM[%04X] = %06X\n", _p, cpu->pm[_p & 0x3fff]);
            }
        }
        /* Trace synthesis main entry (PM[0x2DA3]) — log DM[0x3467] on each call */
        {
            static uint32_t synmain_call_cnt = 0;
            if (m_pc == 0x2DA3 && synmain_call_cnt < 16) {
                synmain_call_cnt++;
                uint16_t dm3467 = (uint16_t)data_read(cpu, 0x3467);
                uint16_t dm3bd4 = (uint16_t)data_read(cpu, 0x3BD4);
                fprintf(stderr, "[SYNMAIN] #%u entry DM[3467]=%04X DM[3BD4]=%04X AR=%04X\n",
                        synmain_call_cnt, dm3467, dm3bd4,
                        (unsigned)(uint16_t)m_core.ar.u);
            }
            /* PM[0x2DB0]: early-exit check — DM[3467] vs 0x3400 */
            if (m_pc == 0x2DB0 && synmain_call_cnt <= 16) {
                static uint32_t db0_cnt = 0;
                if (db0_cnt++ < 32) {
                    uint16_t dm3467 = (uint16_t)data_read(cpu, 0x3467);
                    fprintf(stderr, "[SYNMAIN_DB0] #%u DM[3467]=%04X (== 0x3400? %s)\n",
                            db0_cnt, dm3467, dm3467 == 0x3400 ? "YES→proceed" : "NO→early_exit");
                }
            }
        }
        /* Trace key PCs inside synthesis main to understand control flow */
        {
            static uint32_t pc_db5 = 0, pc_dc0 = 0, pc_dd0 = 0, pc_e12 = 0, pc_e13 = 0;
            if (m_pc == 0x2DB5 && pc_db5 < 5) {
                pc_db5++;
                fprintf(stderr, "[FLOW] PC=2DB5 (RTS?) ASTAT=%02X AR=%04X MR0=%04X #%u\n",
                        (unsigned)m_astat, (unsigned)(uint16_t)m_core.ar.u,
                        (unsigned)(uint16_t)m_core.mr.mrx.mr0.u, pc_db5);
            }
            if (m_pc == 0x2DC0 && pc_dc0 < 5) {
                pc_dc0++;
                fprintf(stderr, "[FLOW] PC=2DC0 #%u AR=%04X CNTR=%u\n",
                        pc_dc0, (unsigned)(uint16_t)m_core.ar.u, (unsigned)m_cntr);
            }
            if (m_pc == 0x2DD0 && pc_dd0 < 5) {
                pc_dd0++;
                fprintf(stderr, "[FLOW] PC=2DD0 (CALL 2E3A) #%u CNTR=%u loop=%04X\n",
                        pc_dd0, (unsigned)m_cntr, (unsigned)m_loop);
            }
            if (m_pc == 0x2E12 && pc_e12 < 5) {
                pc_e12++;
                fprintf(stderr, "[FLOW] PC=2E12 (DO end) #%u CNTR=%u cond=%u\n",
                        pc_e12, (unsigned)m_cntr, (unsigned)m_loop_condition);
            }
            if (m_pc == 0x2E13 && pc_e13 < 5) {
                pc_e13++;
                fprintf(stderr, "[FLOW] PC=2E13 (post DO) #%u\n", pc_e13);
            }
        }

        uint32_t op = opcode_read(cpu);

        /* advance PC / handle loop */
        if (m_pc != m_loop)
            m_pc++;
        else
        {
            if (condition(m_loop_condition))
                m_pc = pc_stack_top(cpu);
            else
            {
                loop_stack_pop(cpu);
                pc_stack_pop_val(cpu);
                m_pc++;
            }
        }

        uint32_t temp;
        switch (BIT(op, 16, 8))
        {
            case 0x00: /* NOP */ break;

            case 0x01: /* IO read/write (ADSP-218x) */
                if (!BIT(op, 15))
                    write_reg0(cpu, BIT(op, 0, 4), io_read(cpu, BIT(op, 4, 11)));
                else
                    io_write(cpu, BIT(op, 4, 11), read_reg0(cpu, BIT(op, 0, 4)));
                break;

            case 0x02: /* modify flag out / idle */
                if (BIT(op, 15)) { m_idle = 1; m_icount = 0; }
                else if (condition(BIT(op, 0, 4)))
                {
                    if (BIT(op,  5)) m_flagout = 0;
                    if (BIT(op,  4)) m_flagout ^= 1;
                    if (BIT(op,  7)) m_fl0 = 0; if (BIT(op,  6)) m_fl0 ^= 1;
                    if (BIT(op,  9)) m_fl1 = 0; if (BIT(op,  8)) m_fl1 ^= 1;
                    if (BIT(op, 11)) m_fl2 = 0; if (BIT(op, 10)) m_fl2 ^= 1;
                }
                break;

            case 0x03: /* call/jump on FLAG IN */
                if (BIT(op, 1) ? m_flagin : !m_flagin)
                {
                    if (BIT(op, 0)) pc_stack_push(cpu);
                    m_pc = BIT(op, 4, 12) | (BIT(op, 2, 2) << 12);
                }
                break;

            case 0x04: /* stack control */
                if (BIT(op, 4)) pc_stack_pop_val(cpu);
                if (BIT(op, 3)) loop_stack_pop(cpu);
                if (BIT(op, 2)) cntr_stack_pop(cpu);
                if (BIT(op, 1)) { if (BIT(op, 0)) stat_stack_pop(cpu); else stat_stack_push(cpu); }
                break;

            case 0x05: /* saturate MR */
                if (GET_MV)
                {
                    if (m_core.mr.mrx.mr2.u & 0x80)
                    { m_core.mr.mrx.mr2.u=0xffff; m_core.mr.mrx.mr1.u=0x8000; m_core.mr.mrx.mr0.u=0x0000; }
                    else
                    { m_core.mr.mrx.mr2.u=0x0000; m_core.mr.mrx.mr1.u=0x7fff; m_core.mr.mrx.mr0.u=0xffff; }
                }
                break;

            case 0x06: /* DIVS */
            {
                int xop2 = ALU_GETXREG_UNSIGNED(BIT(op, 8, 3));
                int yop2 = ALU_GETYREG_UNSIGNED(BIT(op, 11, 2));
                temp = xop2 ^ yop2;
                m_astat = (m_astat & ~QFLAG) | ((temp >> 10) & QFLAG);
                m_core.af.u = (uint16_t)((yop2 << 1) | (m_core.ay0.u >> 15));
                m_core.ay0.u = (uint16_t)((m_core.ay0.u << 1) | (temp >> 15));
                break;
            }

            case 0x07: /* DIVQ */
            {
                int xop2 = ALU_GETXREG_UNSIGNED(BIT(op, 8, 3));
                int res2;
                if (GET_Q) res2 = m_core.af.u + xop2;
                else       res2 = m_core.af.u - xop2;
                temp = res2 ^ xop2;
                m_astat = (m_astat & ~QFLAG) | ((temp >> 10) & QFLAG);
                m_core.af.u  = (uint16_t)((res2 << 1) | (m_core.ay0.u >> 15));
                m_core.ay0.u = (uint16_t)((m_core.ay0.u << 1) | ((~temp >> 15) & 1));
                break;
            }

            case 0x08: break; /* reserved */

            case 0x09: /* modify address register */
                temp = BIT(op, 2, 3);
                modify_address(cpu, temp, (temp & 4) | (op & 3));
                break;

            case 0x0a: /* conditional return */
                if (condition(BIT(op, 0, 4)))
                {
                    pc_stack_pop(cpu);
                    if (BIT(op, 4)) {
                        stat_stack_pop(cpu);
                        /* RTI: stop ISR trace and log return destination */
                        if (g_isr_trace_remaining > 0) {
                            fprintf(stderr, "[ISR%d] RTI → PC=%04X (had %d insns left)\n",
                                    g_isr_trace_count, m_pc, g_isr_trace_remaining);
                            g_isr_trace_remaining = 0;
                        }
                    }
                }
                break;

            case 0x0b: /* conditional jump indirect */
                if (condition(BIT(op, 0, 4)))
                {
                    if (BIT(op, 4)) pc_stack_push(cpu);
                    m_pc = m_i[4 + BIT(op, 6, 2)] & 0x3fff;
                }
                break;

            case 0x0c: /* mode control */
                if (BIT(op,  3)) m_mstat = BIT(op,  2) ? (m_mstat|MSTAT_GOMODE)  : (m_mstat&~MSTAT_GOMODE);
                if (BIT(op, 13)) m_mstat = BIT(op, 12) ? (m_mstat|MSTAT_INTEGER) : (m_mstat&~MSTAT_INTEGER);
                if (BIT(op, 15)) m_mstat = BIT(op, 14) ? (m_mstat|MSTAT_TIMER)   : (m_mstat&~MSTAT_TIMER);
                if (BIT(op,  5)) m_mstat = BIT(op,  4) ? (m_mstat|MSTAT_BANK)    : (m_mstat&~MSTAT_BANK);
                if (BIT(op,  7)) m_mstat = BIT(op,  6) ? (m_mstat|MSTAT_REVERSE) : (m_mstat&~MSTAT_REVERSE);
                if (BIT(op,  9)) m_mstat = BIT(op,  8) ? (m_mstat|MSTAT_STICKYV) : (m_mstat&~MSTAT_STICKYV);
                if (BIT(op, 11)) m_mstat = BIT(op, 10) ? (m_mstat|MSTAT_SATURATE): (m_mstat&~MSTAT_SATURATE);
                update_mstat(cpu);
                break;

            case 0x0d: /* internal data move */
                switch (BIT(op, 8, 4))
                {
                    case 0x0: write_reg0(cpu,BIT(op,4,4),read_reg0(cpu,BIT(op,0,4))); break;
                    case 0x1: write_reg0(cpu,BIT(op,4,4),read_reg1(cpu,BIT(op,0,4))); break;
                    case 0x2: write_reg0(cpu,BIT(op,4,4),read_reg2(cpu,BIT(op,0,4))); break;
                    case 0x3: write_reg0(cpu,BIT(op,4,4),read_reg3(cpu,BIT(op,0,4))); break;
                    case 0x4: write_reg1(cpu,BIT(op,4,4),read_reg0(cpu,BIT(op,0,4))); break;
                    case 0x5: write_reg1(cpu,BIT(op,4,4),read_reg1(cpu,BIT(op,0,4))); break;
                    case 0x6: write_reg1(cpu,BIT(op,4,4),read_reg2(cpu,BIT(op,0,4))); break;
                    case 0x7: write_reg1(cpu,BIT(op,4,4),read_reg3(cpu,BIT(op,0,4))); break;
                    case 0x8: write_reg2(cpu,BIT(op,4,4),read_reg0(cpu,BIT(op,0,4))); break;
                    case 0x9: write_reg2(cpu,BIT(op,4,4),read_reg1(cpu,BIT(op,0,4))); break;
                    case 0xa: write_reg2(cpu,BIT(op,4,4),read_reg2(cpu,BIT(op,0,4))); break;
                    case 0xb: write_reg2(cpu,BIT(op,4,4),read_reg3(cpu,BIT(op,0,4))); break;
                    case 0xc: write_reg3(cpu,BIT(op,4,4),read_reg0(cpu,BIT(op,0,4))); break;
                    case 0xd: write_reg3(cpu,BIT(op,4,4),read_reg1(cpu,BIT(op,0,4))); break;
                    case 0xe: write_reg3(cpu,BIT(op,4,4),read_reg2(cpu,BIT(op,0,4))); break;
                    case 0xf: write_reg3(cpu,BIT(op,4,4),read_reg3(cpu,BIT(op,0,4))); break;
                }
                break;

            case 0x0e: if (condition(BIT(op,0,4))) shift_op(cpu,op);     break;
            case 0x0f: shift_op_imm(cpu, op);                            break;
            case 0x10: shift_op(cpu,op); temp=read_reg0(cpu,BIT(op,0,4)); write_reg0(cpu,BIT(op,4,4),temp); break;

            case 0x11:
                if (BIT(op,15)) { pgm_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); shift_op(cpu,op); }
                else            { shift_op(cpu,op); write_reg0(cpu,BIT(op,4,4),pgm_read_dag2(cpu,op)); }
                break;
            case 0x12:
                if (BIT(op,15)) { data_write_dag1(cpu,op,read_reg0(cpu,BIT(op,4,4))); shift_op(cpu,op); }
                else            { shift_op(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag1(cpu,op)); }
                break;
            case 0x13:
                if (BIT(op,15)) { data_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); shift_op(cpu,op); }
                else            { shift_op(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag2(cpu,op)); }
                break;

            case 0x14: case 0x15: case 0x16: case 0x17: /* DO UNTIL */
                loop_stack_push(cpu, op & 0x3ffff);
                pc_stack_push(cpu);
                break;

            case 0x18: case 0x19: case 0x1a: case 0x1b: /* conditional jump */
                if (condition(BIT(op,0,4)))
                {
                    m_pc = BIT(op, 4, 14);
                    if (m_pc == m_ppc) m_icount = 0; /* busy loop */
                }
                break;

            case 0x1c: case 0x1d: case 0x1e: case 0x1f: /* conditional call */
                if (condition(BIT(op,0,4))) {
                    uint32_t call_target = BIT(op,4,14);
                    /* log all CALLs from main loop area (not boot 0x38xx) */
                    if (m_ppc < 0x3800 || m_ppc > 0x38FF) {
                        static uint32_t call_cnt = 0;
                        if (call_cnt++ < 256)
                            fprintf(stderr, "[CALL] %04X → %04X (#%u)\n",
                                    (unsigned)m_ppc, call_target, call_cnt);
                    }
                    pc_stack_push(cpu); m_pc = call_target;
                }
                break;

            case 0x20: case 0x21: /* conditional MAC→MR */
                if (condition(BIT(op,0,4)))
                { if ((op & 0x0018f0) == 0x000010) mac_op_mr_xop(cpu,op); else mac_op_mr(cpu,op); }
                break;
            case 0x22: case 0x23: /* conditional ALU→AR */
                if (condition(BIT(op,0,4)))
                { if (BIT(op,4)) alu_op_ar_const(cpu,op); else alu_op_ar(cpu,op); }
                break;
            case 0x24: case 0x25: /* conditional MAC→MF */
                if (condition(BIT(op,0,4)))
                { if ((op & 0x0018f0) == 0x000010) mac_op_mf_xop(cpu,op); else mac_op_mf(cpu,op); }
                break;
            case 0x26: case 0x27: /* conditional ALU→AF */
                if (condition(BIT(op,0,4)))
                { if (BIT(op,4)) alu_op_af_const(cpu,op); else alu_op_af(cpu,op); }
                break;

            case 0x28: case 0x29: temp=read_reg0(cpu,BIT(op,0,4)); mac_op_mr(cpu,op); write_reg0(cpu,BIT(op,4,4),temp); break;
            case 0x2a: case 0x2b:
                if (BIT(op,0,8)==0xaa) alu_op_none(cpu,op);
                else { temp=read_reg0(cpu,BIT(op,0,4)); alu_op_ar(cpu,op); write_reg0(cpu,BIT(op,4,4),temp); }
                break;
            case 0x2c: case 0x2d: temp=read_reg0(cpu,BIT(op,0,4)); mac_op_mf(cpu,op); write_reg0(cpu,BIT(op,4,4),temp); break;
            case 0x2e: case 0x2f: temp=read_reg0(cpu,BIT(op,0,4)); alu_op_af(cpu,op); write_reg0(cpu,BIT(op,4,4),temp); break;

            case 0x30: case 0x31: case 0x32: case 0x33: write_reg0(cpu,BIT(op,0,4),sext(op>>4,14)); break;
            case 0x34: case 0x35: case 0x36: case 0x37: write_reg1(cpu,BIT(op,0,4),sext(op>>4,14)); break;
            case 0x38: case 0x39: case 0x3a: case 0x3b: write_reg2(cpu,BIT(op,0,4),sext(op>>4,14)); break;
            case 0x3c: case 0x3d: case 0x3e: case 0x3f: write_reg3(cpu,BIT(op,0,4),sext(op>>4,14)); break;

            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
                write_reg0(cpu, BIT(op,0,4), BIT(op,4,16)); break;

            case 0x50: case 0x51: mac_op_mr(cpu,op); write_reg0(cpu,BIT(op,4,4),pgm_read_dag2(cpu,op)); break;
            case 0x52: case 0x53: alu_op_ar(cpu,op); write_reg0(cpu,BIT(op,4,4),pgm_read_dag2(cpu,op)); break;
            case 0x54: case 0x55: mac_op_mf(cpu,op); write_reg0(cpu,BIT(op,4,4),pgm_read_dag2(cpu,op)); break;
            case 0x56: case 0x57: alu_op_af(cpu,op); write_reg0(cpu,BIT(op,4,4),pgm_read_dag2(cpu,op)); break;
            case 0x58: case 0x59: pgm_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); mac_op_mr(cpu,op); break;
            case 0x5a: case 0x5b: pgm_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); alu_op_ar(cpu,op); break;
            case 0x5c: case 0x5d: pgm_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); mac_op_mf(cpu,op); break;
            case 0x5e: case 0x5f: pgm_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); alu_op_af(cpu,op); break;

            case 0x60: case 0x61: mac_op_mr(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag1(cpu,op)); break;
            case 0x62: case 0x63: alu_op_ar(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag1(cpu,op)); break;
            case 0x64: case 0x65: mac_op_mf(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag1(cpu,op)); break;
            case 0x66: case 0x67: alu_op_af(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag1(cpu,op)); break;
            case 0x68: case 0x69: data_write_dag1(cpu,op,read_reg0(cpu,BIT(op,4,4))); mac_op_mr(cpu,op); break;
            case 0x6a: case 0x6b: data_write_dag1(cpu,op,read_reg0(cpu,BIT(op,4,4))); alu_op_ar(cpu,op); break;
            case 0x6c: case 0x6d: data_write_dag1(cpu,op,read_reg0(cpu,BIT(op,4,4))); mac_op_mf(cpu,op); break;
            case 0x6e: case 0x6f: data_write_dag1(cpu,op,read_reg0(cpu,BIT(op,4,4))); alu_op_af(cpu,op); break;

            case 0x70: case 0x71: mac_op_mr(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag2(cpu,op)); break;
            case 0x72: case 0x73: alu_op_ar(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag2(cpu,op)); break;
            case 0x74: case 0x75: mac_op_mf(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag2(cpu,op)); break;
            case 0x76: case 0x77: alu_op_af(cpu,op); write_reg0(cpu,BIT(op,4,4),data_read_dag2(cpu,op)); break;
            case 0x78: case 0x79: data_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); mac_op_mr(cpu,op); break;
            case 0x7a: case 0x7b: data_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); alu_op_ar(cpu,op); break;
            case 0x7c: case 0x7d: data_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); mac_op_mf(cpu,op); break;
            case 0x7e: case 0x7f: data_write_dag2(cpu,op,read_reg0(cpu,BIT(op,4,4))); alu_op_af(cpu,op); break;

            case 0x80: case 0x81: case 0x82: case 0x83: write_reg0(cpu,BIT(op,0,4),data_read(cpu,BIT(op,4,14))); break;
            case 0x84: case 0x85: case 0x86: case 0x87: write_reg1(cpu,BIT(op,0,4),data_read(cpu,BIT(op,4,14))); break;
            case 0x88: case 0x89: case 0x8a: case 0x8b: write_reg2(cpu,BIT(op,0,4),data_read(cpu,BIT(op,4,14))); break;
            case 0x8c: case 0x8d: case 0x8e: case 0x8f: write_reg3(cpu,BIT(op,0,4),data_read(cpu,BIT(op,4,14))); break;

            case 0x90: case 0x91: case 0x92: case 0x93: data_write(cpu,BIT(op,4,14),read_reg0(cpu,BIT(op,0,4))); break;
            case 0x94: case 0x95: case 0x96: case 0x97: data_write(cpu,BIT(op,4,14),read_reg1(cpu,BIT(op,0,4))); break;
            case 0x98: case 0x99: case 0x9a: case 0x9b: data_write(cpu,BIT(op,4,14),read_reg2(cpu,BIT(op,0,4))); break;
            case 0x9c: case 0x9d: case 0x9e: case 0x9f: data_write(cpu,BIT(op,4,14),read_reg3(cpu,BIT(op,0,4))); break;

            case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
            case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
                data_write_dag1(cpu, op, BIT(op, 4, 16)); break;

            case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
            case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
                data_write_dag2(cpu, op, BIT(op, 4, 16)); break;

            /* dual-fetch (MAC/ALU + data DAG1 + pgm DAG2) */
            case 0xc0: case 0xc1: mac_op_mr(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xc2: case 0xc3: alu_op_ar(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xc4: case 0xc5: mac_op_mr(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xc6: case 0xc7: alu_op_ar(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xc8: case 0xc9: mac_op_mr(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xca: case 0xcb: alu_op_ar(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xcc: case 0xcd: mac_op_mr(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xce: case 0xcf: alu_op_ar(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.ay0.u=pgm_read_dag2(cpu,op>>4); break;

            case 0xd0: case 0xd1: mac_op_mr(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xd2: case 0xd3: alu_op_ar(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xd4: case 0xd5: mac_op_mr(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xd6: case 0xd7: alu_op_ar(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xd8: case 0xd9: mac_op_mr(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xda: case 0xdb: alu_op_ar(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xdc: case 0xdd: mac_op_mr(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xde: case 0xdf: alu_op_ar(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.ay1.u=pgm_read_dag2(cpu,op>>4); break;

            case 0xe0: case 0xe1: mac_op_mr(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xe2: case 0xe3: alu_op_ar(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xe4: case 0xe5: mac_op_mr(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xe6: case 0xe7: alu_op_ar(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xe8: case 0xe9: mac_op_mr(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xea: case 0xeb: alu_op_ar(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xec: case 0xed: mac_op_mr(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xee: case 0xef: alu_op_ar(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.my0.u=pgm_read_dag2(cpu,op>>4); break;

            case 0xf0: case 0xf1: mac_op_mr(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xf2: case 0xf3: alu_op_ar(cpu,op); m_core.ax0.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xf4: case 0xf5: mac_op_mr(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xf6: case 0xf7: alu_op_ar(cpu,op); m_core.ax1.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xf8: case 0xf9: mac_op_mr(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xfa: case 0xfb: alu_op_ar(cpu,op); m_core.mx0.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xfc: case 0xfd: mac_op_mr(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
            case 0xfe: case 0xff: alu_op_ar(cpu,op); m_core.mx1.u=data_read_dag1(cpu,op); m_core.my1.u=pgm_read_dag2(cpu,op>>4); break;
        }

        m_icount--;
    } while (m_icount > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

void ADSP2181_Init(ADSP2181 *cpu)
{
    memset(cpu, 0, sizeof(*cpu));

    create_tables(cpu);

    /* register group 0 read pointers */
    m_read0_ptr[0x0] = &m_core.ax0.s;
    m_read0_ptr[0x1] = &m_core.ax1.s;
    m_read0_ptr[0x2] = &m_core.mx0.s;
    m_read0_ptr[0x3] = &m_core.mx1.s;
    m_read0_ptr[0x4] = &m_core.ay0.s;
    m_read0_ptr[0x5] = &m_core.ay1.s;
    m_read0_ptr[0x6] = &m_core.my0.s;
    m_read0_ptr[0x7] = &m_core.my1.s;
    m_read0_ptr[0x8] = &m_core.si.s;
    m_read0_ptr[0x9] = &m_core.se.s;
    m_read0_ptr[0xa] = &m_core.ar.s;
    m_read0_ptr[0xb] = &m_core.mr.mrx.mr0.s;
    m_read0_ptr[0xc] = &m_core.mr.mrx.mr1.s;
    m_read0_ptr[0xd] = &m_core.mr.mrx.mr2.s;
    m_read0_ptr[0xe] = &m_core.sr.srx.sr0.s;
    m_read0_ptr[0xf] = &m_core.sr.srx.sr1.s;

    /* register group 1 + 2 read pointers */
    for (int k = 0; k < 4; k++)
    {
        m_read1_ptr[0x0 + k] = &m_i[0 + k];
        m_read1_ptr[0x4 + k] = (uint32_t *)&m_m[0 + k];
        m_read1_ptr[0x8 + k] = &m_l[0 + k];
        m_read1_ptr[0xc + k] = &m_l[0 + k];
        m_read2_ptr[0x0 + k] = &m_i[4 + k];
        m_read2_ptr[0x4 + k] = (uint32_t *)&m_m[4 + k];
        m_read2_ptr[0x8 + k] = &m_l[4 + k];
        m_read2_ptr[0xc + k] = &m_l[4 + k];
    }

    /* ALU register pointers */
    m_alu_xregs[0]=&m_core.ax0; m_alu_xregs[1]=&m_core.ax1;
    m_alu_xregs[2]=&m_core.ar;  m_alu_xregs[3]=&m_core.mr.mrx.mr0;
    m_alu_xregs[4]=&m_core.mr.mrx.mr1; m_alu_xregs[5]=&m_core.mr.mrx.mr2;
    m_alu_xregs[6]=&m_core.sr.srx.sr0; m_alu_xregs[7]=&m_core.sr.srx.sr1;
    m_alu_yregs[0]=&m_core.ay0; m_alu_yregs[1]=&m_core.ay1;
    m_alu_yregs[2]=&m_core.af;  m_alu_yregs[3]=&m_core.zero;

    /* MAC register pointers */
    m_mac_xregs[0]=&m_core.mx0; m_mac_xregs[1]=&m_core.mx1;
    m_mac_xregs[2]=&m_core.ar;  m_mac_xregs[3]=&m_core.mr.mrx.mr0;
    m_mac_xregs[4]=&m_core.mr.mrx.mr1; m_mac_xregs[5]=&m_core.mr.mrx.mr2;
    m_mac_xregs[6]=&m_core.sr.srx.sr0; m_mac_xregs[7]=&m_core.sr.srx.sr1;
    m_mac_yregs[0]=&m_core.my0; m_mac_yregs[1]=&m_core.my1;
    m_mac_yregs[2]=&m_core.mf;  m_mac_yregs[3]=&m_core.zero;

    /* shift register pointers */
    m_shift_xregs[0]=&m_core.si; m_shift_xregs[1]=&m_core.si;
    m_shift_xregs[2]=&m_core.ar; m_shift_xregs[3]=&m_core.mr.mrx.mr0;
    m_shift_xregs[4]=&m_core.mr.mrx.mr1; m_shift_xregs[5]=&m_core.mr.mrx.mr2;
    m_shift_xregs[6]=&m_core.sr.srx.sr0; m_shift_xregs[7]=&m_core.sr.srx.sr1;

    /* initial data bank = internal bank 0 */
    cpu->dm_active_bank     = cpu->dm_bank[0];
    cpu->dm_active_bank_idx = 0;
}

void ADSP2181_Reset(ADSP2181 *cpu, const uint8_t *rom_boot)
{
    /* ensure zero register stays zero */
    m_core.zero.u = m_alt.zero.u = 0;

    /* Load first 32 PM words (96 bytes) from ROM.
     * MAME acclaim_rax device_reset: 32 words × 3 bytes big-endian.
     * (The load_boot_data 4-byte/pagelen format is for other ADSP boards,
     * not the RAX where the BDMA boot ROM is stored packed 3 bytes/word.) */
    if (rom_boot)
    {
        for (int i = 0; i < 32; i++)
        {
            cpu->pm[i] = ((uint32_t)rom_boot[i*3 + 0] << 16)
                       | ((uint32_t)rom_boot[i*3 + 1] <<  8)
                       |  (uint32_t)rom_boot[i*3 + 2];
        }
    }

    /* recompute address-generator bases */
    for (int k = 0; k < 4; k++)
    {
        write_reg1(cpu, 0x08 + k, m_l[k]);   write_reg1(cpu, 0x00 + k, m_i[k]);
        write_reg2(cpu, 0x08 + k, m_l[4+k]); write_reg2(cpu, 0x00 + k, m_i[4+k]);
    }

    /* reset overlays */
    m_pmovlay = m_dmovlay = 0;
    update_dmovlay(cpu);

    /* reset PC */
    m_pc = m_ppc = 0;
    m_loop = 0xffff;
    m_loop_condition = 0;

    /* reset status */
    m_astat_clear = ~(CFLAG | VFLAG | NFLAG | ZFLAG);
    m_mstat = 0;
    m_sstat = 0x55; /* all stacks empty */
    m_idle  = 0;
    update_mstat(cpu);

    /* reset stacks */
    m_pc_sp = m_cntr_sp = m_stat_sp = m_loop_sp = 0;

    /* reset external I/O */
    m_flagout = m_flagin = m_fl0 = m_fl1 = m_fl2 = 0;

    /* reset interrupts */
    m_imask = 0;
    for (int i = 0; i < ADSP2181_NUM_IRQ; i++)
        m_irq_state[i] = m_irq_latch[i] = 0;

    /* reset data bank to internal */
    cpu->dm_active_bank     = cpu->dm_bank[0];
    cpu->dm_active_bank_idx = 0;
}

int ADSP2181_Run(ADSP2181 *cpu, int cycles)
{
    if (cycles <= 0) return 0;
    m_icount = cycles;
    execute_run(cpu);
    return cycles - m_icount;
}

void ADSP2181_SetIRQ(ADSP2181 *cpu, int which, int state)
{
    if (which < 0 || which >= ADSP2181_NUM_IRQ) return;
    /* rising edge latches the interrupt */
    if (state != 0 && m_irq_state[which] == 0)
        m_irq_latch[which] = 1;
    m_irq_state[which] = state ? 1 : 0;
    check_irqs(cpu);
}

void ADSP2181_IdmaAddrW(ADSP2181 *cpu, uint16_t data)
{
    m_idma_addr = data;
    m_idma_offs = 0;
}

uint16_t ADSP2181_IdmaAddrR(ADSP2181 *cpu)
{
    return m_idma_addr;
}

void ADSP2181_IdmaDataW(ADSP2181 *cpu, uint16_t data)
{
    if (!(m_idma_addr & 0x4000))
    {
        /* program memory: two 16-bit writes per 24-bit word */
        if (m_idma_offs == 0)
        {
            m_idma_cache = data;
            m_idma_offs  = 1;
        }
        else
        {
            program_write(cpu, m_idma_addr++ & 0x3fff,
                          ((uint32_t)m_idma_cache << 8) | (data & 0xff));
            m_idma_offs = 0;
        }
    }
    else
    {
        /* data memory */
        data_write(cpu, m_idma_addr++ & 0x3fff, data);
    }
}

uint16_t ADSP2181_IdmaDataR(ADSP2181 *cpu)
{
    uint16_t result;
    if (!(m_idma_addr & 0x4000))
    {
        if (m_idma_offs == 0)
        {
            result       = (uint16_t)(program_read(cpu, m_idma_addr & 0x3fff) >> 8);
            m_idma_offs  = 1;
        }
        else
        {
            result       = (uint16_t)(program_read(cpu, m_idma_addr++ & 0x3fff) & 0xff);
            m_idma_offs  = 0;
        }
    }
    else
    {
        result = data_read(cpu, m_idma_addr++ & 0x3fff);
    }
    return result;
}

void ADSP2181_UpdateDataBank(ADSP2181 *cpu, int dmovlay_val, int ext_bank_idx)
{
    if (dmovlay_val == 0)
    {
        cpu->dm_active_bank     = cpu->dm_bank[0];
        cpu->dm_active_bank_idx = 0;
    }
    else
    {
        int idx = 1 + (ext_bank_idx & 3);
        cpu->dm_active_bank     = cpu->dm_bank[idx];
        cpu->dm_active_bank_idx = idx;
    }
}
