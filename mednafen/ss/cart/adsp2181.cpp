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
    if (__builtin_expect(addr < 0x2000, 1))  return cpu->dm_active_bank[addr];
    if (__builtin_expect(addr < 0x3fe0, 1))  return cpu->dm_upper[addr - 0x2000];
    return cpu->io_read_cb ? cpu->io_read_cb(cpu, 0x800u + (addr & 0x1f)) : 0xffff;
}

static inline void data_write(ADSP2181 *cpu, uint32_t addr, uint16_t data)
{
    addr &= 0x3fff;
    if (__builtin_expect(addr < 0x2000, 1))  { cpu->dm_active_bank[addr] = data; return; }
    if (__builtin_expect(addr < 0x3fe0, 1))  { cpu->dm_upper[addr - 0x2000] = data; return; }
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
    cpu->pm[addr & 0x3fff] = data & 0xffffff;
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
    uint32_t ireg   = 4 + BIT(op, 2, 2);
    uint32_t mreg   = 4 + BIT(op, 0, 2);
    uint32_t base   = m_base[ireg];
    uint32_t i      = m_i[ireg];
    uint32_t l      = m_l[ireg];
    uint32_t pm_val = (val << 8) | m_px;
    program_write(cpu, i, pm_val);
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
            break;
        case 1:
            m_m[index] = sext(val, 14);
            break;
        case 2:
            m_l[index] = val & 0x3fff;
            update_l(cpu, index);
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
            break;
        case 1:
            m_m[index] = sext(val, 14);
            break;
        case 2:
            m_l[index] = val & 0x3fff;
            update_l(cpu, index);
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
        case 0x05: cntr_stack_push(cpu); m_cntr = val & 0x3fff; break;
        case 0x06: m_core.sb.s = sext(val, 5);                              break;
        case 0x07: m_px = val;                                               break;
        case 0x09: m_sport_tx_cb(0, val, 0xffff); break;
        case 0x0b: m_sport_tx_cb(1, val, 0xffff);                           break;
        case 0x0c:
            m_ifc = val;
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
                    if (BIT(op, 4))
                        stat_stack_pop(cpu);
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
