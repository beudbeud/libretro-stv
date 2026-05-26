/*
 * Standalone ADSP-2181 CPU emulator for the Acclaim RAX sound board.
 * Adapted from MAME's adsp2100.cpp/h (copyright Aaron Giles, BSD-3-Clause).
 * Stripped of all MAME framework dependencies; targets ADSP-2181 only.
 */

#ifndef ADSP2181_H
#define ADSP2181_H

#include <stdint.h>
#include <string.h>

/* IRQ indices for ADSP-2181 */
#define ADSP2181_IRQ0     0
#define ADSP2181_IRQ1     1
#define ADSP2181_IRQ2     2
#define ADSP2181_SPORT0_RX 3
#define ADSP2181_SPORT0_TX 4
#define ADSP2181_TIMER    5
#define ADSP2181_IRQE     6
#define ADSP2181_BDMA     7
#define ADSP2181_IRQL1    8
#define ADSP2181_IRQL0    9
#define ADSP2181_NUM_IRQ  10

/* ASTAT flag bits */
#define ADSP_SS  0x80
#define ADSP_MV  0x40
#define ADSP_Q   0x20
#define ADSP_S   0x10
#define ADSP_C   0x08
#define ADSP_V   0x04
#define ADSP_N   0x02
#define ADSP_Z   0x01

/* MSTAT bits */
#define ADSP_MSTAT_BANK     0x01
#define ADSP_MSTAT_REVERSE  0x02
#define ADSP_MSTAT_STICKYV  0x04
#define ADSP_MSTAT_SATURATE 0x08
#define ADSP_MSTAT_INTEGER  0x10
#define ADSP_MSTAT_TIMER    0x20
#define ADSP_MSTAT_GOMODE   0x40

/* Program memory size for ADSP-2181 */
#define ADSP2181_PM_SIZE   0x4000  /* 16K words × 24-bit (stored as uint32) */
/* Data memory size */
#define ADSP2181_DM_INT_WORDS 0x2000  /* Internal DM: 0x0000-0x1FFF (banked) */
#define ADSP2181_DM_UPPER_WORDS 0x1FE0 /* Internal DM: 0x2000-0x3FDF */
#define ADSP2181_DM_CTRL_WORDS 0x20    /* Control regs: 0x3FE0-0x3FFF */
#define ADSP2181_DM_TOTAL  0x4000      /* 16K words total address space */

/* Data banked RAM: 4 external banks + 1 internal bank (index 0) */
#define ADSP2181_NUM_BANKS 5

/* IO space (11-bit = 2K addresses) */
#define ADSP2181_IO_SIZE   0x800

/* Forward declaration */
struct ADSP2181;

/* Callback types */
typedef void     (*ADSP2181_SportTxCb)(ADSP2181 *cpu, int sport, uint32_t data);
typedef uint32_t (*ADSP2181_SportRxCb)(ADSP2181 *cpu, int sport);
typedef void     (*ADSP2181_DmovlayCb)(ADSP2181 *cpu, uint32_t val);
typedef uint16_t (*ADSP2181_IoReadCb) (ADSP2181 *cpu, uint32_t addr);
typedef void     (*ADSP2181_IoWriteCb)(ADSP2181 *cpu, uint32_t addr, uint16_t data);

/* 16-bit register union (signed/unsigned) */
union adsp_reg16 {
    uint16_t u;
    int16_t  s;
};

/* 32-bit shift result */
union adsp_shift {
    struct { adsp_reg16 sr0, sr1; } srx;
    uint32_t sr;
};

/* 40-bit MAC result (stored as 64-bit) */
union adsp_mac {
    struct { adsp_reg16 mr0, mr1, mr2, mrzero; } mrx;
    struct { uint32_t mr0lo, mr1lo; } mry;
    uint64_t mr;
};

/* Main register file (replicated for bank switching) */
struct adsp_core {
    adsp_reg16 ax0, ax1;
    adsp_reg16 ay0, ay1;
    adsp_reg16 ar;
    adsp_reg16 af;

    adsp_reg16 mx0, mx1;
    adsp_reg16 my0, my1;
    adsp_mac   mr;
    adsp_reg16 mf;

    adsp_reg16 si;
    adsp_reg16 se;
    adsp_reg16 sb;
    adsp_shift sr;

    adsp_reg16 zero;  /* always 0 */
};

struct ADSP2181 {
    /* ── Program memory ──────────────────────────────────────────────────── */
    uint32_t pm[ADSP2181_PM_SIZE];   /* 24-bit PM words in bits[23:0] */

    /* ── Data memory ─────────────────────────────────────────────────────── */
    /* bank[0] = always-internal bank (when DMOVLAY=0, this maps 0x0000-0x1FFF)
     * bank[1..4] = external banks (selected when DMOVLAY=1 or 2, plus dm_bank) */
    uint16_t dm_bank[ADSP2181_NUM_BANKS][ADSP2181_DM_INT_WORDS];
    uint16_t dm_upper[ADSP2181_DM_UPPER_WORDS]; /* addresses 0x2000-0x3FDF */

    /* ── Register files ──────────────────────────────────────────────────── */
    adsp_core  core;   /* live registers */
    adsp_core  alt;    /* alternate bank */

    /* Address generation registers */
    uint32_t i[8];
    int32_t  m[8];
    uint32_t l[8];
    uint32_t lmask[8];
    uint32_t base[8];
    uint8_t  px;

    /* Overlay registers */
    uint32_t pmovlay;
    uint32_t dmovlay;

    /* ── CPU state ───────────────────────────────────────────────────────── */
    uint32_t pc;
    uint32_t ppc;   /* previous PC */
    uint32_t loop;
    uint32_t loop_condition;
    uint32_t cntr;

    uint32_t astat;
    uint32_t sstat;
    uint32_t mstat;
    uint32_t mstat_prev;
    uint32_t astat_clear;
    uint32_t idle;

    /* Stacks */
    static const int PC_STACK_DEPTH   = 16;
    static const int CNTR_STACK_DEPTH = 4;
    static const int STAT_STACK_DEPTH = 4;
    static const int LOOP_STACK_DEPTH = 4;

    uint32_t loop_stack[4];
    uint32_t cntr_stack[4];
    uint32_t pc_stack[16];
    uint16_t stat_stack[4][3];
    int32_t  pc_sp;
    int32_t  cntr_sp;
    int32_t  stat_sp;
    int32_t  loop_sp;

    /* External I/O */
    uint8_t flagout;
    uint8_t flagin;
    uint8_t fl0, fl1, fl2;

    /* IDMA */
    uint16_t idma_addr;
    uint16_t idma_cache;
    uint8_t  idma_offs;

    /* Interrupts */
    uint16_t imask;
    uint8_t  icntl;
    uint16_t ifc;
    uint8_t  irq_state[ADSP2181_NUM_IRQ];
    uint8_t  irq_latch[ADSP2181_NUM_IRQ];

    /* Cycle counter */
    int icount;

    /* ── Lookup tables ───────────────────────────────────────────────────── */
    uint8_t  condition_table[0x1000];
    uint16_t mask_table[0x4000];
    uint16_t reverse_table[0x4000];

    /* ── Register pointer tables (for fast dispatch) ─────────────────────── */
    int16_t  *read0_ptr[16];
    uint32_t *read1_ptr[16];
    uint32_t *read2_ptr[16];
    void     *alu_xregs[8];
    void     *alu_yregs[4];
    void     *mac_xregs[8];
    void     *mac_yregs[4];
    void     *shift_xregs[8];

    /* ── Callbacks ───────────────────────────────────────────────────────── */
    ADSP2181_SportTxCb sport_tx_cb;
    ADSP2181_SportRxCb sport_rx_cb;
    ADSP2181_DmovlayCb dmovlay_cb;
    ADSP2181_IoReadCb  io_read_cb;
    ADSP2181_IoWriteCb io_write_cb;
    void *cb_userdata;

    /* current data bank pointer (points into dm_bank[]) */
    uint16_t *dm_active_bank;  /* = dm_bank[active_bank_index] */
    int       dm_active_bank_idx;
};

/* ── API ────────────────────────────────────────────────────────────────── */

/* Initialise the CPU struct; zero-fill and build lookup tables */
void ADSP2181_Init(ADSP2181 *cpu);

/* Hard reset: loads first 32 PM words from the 96 bytes of ROM at rom_boot */
void ADSP2181_Reset(ADSP2181 *cpu, const uint8_t *rom_boot);

/* Run for `cycles` machine cycles; returns cycles actually consumed */
int  ADSP2181_Run(ADSP2181 *cpu, int cycles);

/* Assert/deassert an IRQ line (level-sensitive for IRQL0/IRQL1, edge for others) */
void ADSP2181_SetIRQ(ADSP2181 *cpu, int which, int state);

/* IDMA port (used by the RAX host init) */
void     ADSP2181_IdmaAddrW(ADSP2181 *cpu, uint16_t data);
uint16_t ADSP2181_IdmaAddrR(ADSP2181 *cpu);
void     ADSP2181_IdmaDataW(ADSP2181 *cpu, uint16_t data);
uint16_t ADSP2181_IdmaDataR(ADSP2181 *cpu);

/* Select the active data bank (called when DMOVLAY changes or dm_bank register changes) */
void ADSP2181_UpdateDataBank(ADSP2181 *cpu, int dmovlay_val, int ext_bank_idx);

#endif /* ADSP2181_H */
