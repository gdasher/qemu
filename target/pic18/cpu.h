/*
 * PIC18 (Q43 core, standard instruction set) CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_PIC18_CPU_H
#define QEMU_PIC18_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "system/memory.h"

#ifdef CONFIG_USER_ONLY
#error "PIC18 does not support user mode"
#endif

#define CPU_RESOLVING_TYPE TYPE_PIC18_CPU

/*
 * Harvard architecture: instruction fetch and data access use separate
 * address spaces that both start at zero.
 */
#define MMU_CODE_IDX 0
#define MMU_DATA_IDX 1

/*
 * Offsets of the two spaces within the host address space.
 *
 * Program space is byte addressed: 16-bit little-endian instruction words at
 * even addresses, a 21-bit PC. The flash occupies 0..0x1FFFF; the user IDs,
 * configuration bytes and EEPROM sit further up the same space, all readable
 * through TBLPTR, so the HEX file loads in one pass.
 *
 * Data space is 14 bits: 64 banks of 256 bytes. The SoC maps its RAM and
 * MMIO devices here, offset by OFFSET_DATA.
 */
#define OFFSET_CODE 0x00000000
#define OFFSET_DATA 0x00800000

#define PIC18_CODE_SPACE_SIZE 0x00400000
#define PIC18_DATA_SPACE_SIZE 0x4000
#define PIC18_FLASH_SIZE      0x20000
#define PIC18_CONFIG_BASE     0x300000
#define PIC18_NUM_CONFIG      10
#define PIC18_USERID_BASE     0x200000
#define PIC18_USERID_SIZE     0x40
#define PIC18_EEPROM_BASE     0x380000
#define PIC18_EEPROM_SIZE     0x400

#define PIC18_PC_MASK         0x1FFFFE  /* 21 bits, bit 0 always clear */
#define PIC18_TBLPTR_MASK     0x3FFFFF  /* 22 bits: reaches 0x300000 */
#define PIC18_FSR_MASK        0x3FFF    /* 14 bits */
#define PIC18_DATA_MASK       0x3FFF

/*
 * Data memory layout. SFRs fill banks 0-4 (0x000-0x4FF); the CPU itself
 * implements 0x4D6-0x4FF (INTCON0 .. TOSU) and everything below that is the
 * SoC's. General purpose RAM starts at 0x500.
 *
 * Access bank (a = 0): f < 0x60 addresses 0x500 + f (Access RAM, the first
 * 96 bytes of bank 5); f >= 0x60 addresses 0x400 + f (the last 160 bytes of
 * bank 4, where the fast SFRs live).
 */
#define PIC18_CORE_SFR_BASE   0x4D6
#define PIC18_SFR_END         0x500
#define PIC18_ACCESS_SPLIT    0x60
#define PIC18_ACCESS_RAM_BASE 0x500
#define PIC18_ACCESS_SFR_BASE 0x400
#define PIC18_BANK_SIZE       0x100
#define PIC18_NUM_BANKS       64

/* Core register addresses. */
#define PIC18_REG_INTCON0  0x4D6
#define PIC18_REG_INTCON1  0x4D7
#define PIC18_REG_STATUS   0x4D8
#define PIC18_REG_FSR2L    0x4D9
#define PIC18_REG_FSR2H    0x4DA
#define PIC18_REG_PLUSW2   0x4DB
#define PIC18_REG_PREINC2  0x4DC
#define PIC18_REG_POSTDEC2 0x4DD
#define PIC18_REG_POSTINC2 0x4DE
#define PIC18_REG_INDF2    0x4DF
#define PIC18_REG_BSR      0x4E0
#define PIC18_REG_FSR1L    0x4E1
#define PIC18_REG_FSR1H    0x4E2
#define PIC18_REG_PLUSW1   0x4E3
#define PIC18_REG_PREINC1  0x4E4
#define PIC18_REG_POSTDEC1 0x4E5
#define PIC18_REG_POSTINC1 0x4E6
#define PIC18_REG_INDF1    0x4E7
#define PIC18_REG_WREG     0x4E8
#define PIC18_REG_FSR0L    0x4E9
#define PIC18_REG_FSR0H    0x4EA
#define PIC18_REG_PLUSW0   0x4EB
#define PIC18_REG_PREINC0  0x4EC
#define PIC18_REG_POSTDEC0 0x4ED
#define PIC18_REG_POSTINC0 0x4EE
#define PIC18_REG_INDF0    0x4EF
#define PIC18_REG_PCON0    0x4F0
#define PIC18_REG_PCON1    0x4F1
#define PIC18_REG_CPUDOZE  0x4F2
#define PIC18_REG_PRODL    0x4F3
#define PIC18_REG_PRODH    0x4F4
#define PIC18_REG_TABLAT   0x4F5
#define PIC18_REG_TBLPTRL  0x4F6
#define PIC18_REG_TBLPTRH  0x4F7
#define PIC18_REG_TBLPTRU  0x4F8
#define PIC18_REG_PCL      0x4F9
#define PIC18_REG_PCLATH   0x4FA
#define PIC18_REG_PCLATU   0x4FB
#define PIC18_REG_STKPTR   0x4FC
#define PIC18_REG_TOSL     0x4FD
#define PIC18_REG_TOSH     0x4FE
#define PIC18_REG_TOSU     0x4FF

/*
 * Indirect operands: each FSR has five virtual registers, laid out
 * identically (PLUSW, PREINC, POSTDEC, POSTINC, INDF) at 0x4DB (FSR2),
 * 0x4E3 (FSR1) and 0x4EB (FSR0).
 */
#define PIC18_IND_PLUSW   0
#define PIC18_IND_PREINC  1
#define PIC18_IND_POSTDEC 2
#define PIC18_IND_POSTINC 3
#define PIC18_IND_INDF    4

/*
 * Hardware return stack: 127 entries of 21 bits (DS40002147F 9.1.3). STKPTR
 * is the index of the top entry, zero when empty; a push increments it first.
 * Entry 0 is never written and reads as zero through TOS.
 */
#define PIC18_STACK_DEPTH 127
#define PIC18_STKPTR_MASK 0x7F

/* Reset and the legacy (MVECEN = 0) interrupt vectors, as byte addresses. */
#define PIC18_RESET_VECTOR    0x000000
#define PIC18_HIGH_INT_VECTOR 0x000008
#define PIC18_LOW_INT_VECTOR  0x000018

/* The two interrupt request lines, unnamed GPIO inputs of the CPU. */
#define PIC18_IRQ_HIGH 0
#define PIC18_IRQ_LOW  1

#define EXCP_RESET 1
#define EXCP_INT   2

/* STATUS bit positions (DS40002147F 7.7.7). TO and PD are read-only. */
#define PIC18_STATUS_C  0
#define PIC18_STATUS_DC 1
#define PIC18_STATUS_Z  2
#define PIC18_STATUS_OV 3
#define PIC18_STATUS_N  4
#define PIC18_STATUS_PD 5
#define PIC18_STATUS_TO 6

/* INTCON0 bit positions. Bits 2:0 are the INTx edge selects, reset to 1. */
#define PIC18_INTCON0_GIE  7
#define PIC18_INTCON0_GIEL 6
#define PIC18_INTCON0_IPEN 5
#define PIC18_INTCON0_RESET 0x07

/*
 * INTCON1 STAT[1:0], the interrupt controller's execution state
 * (DS40002147F 11.13.2). The hardware keeps this rather than clearing GIE
 * on entry, and RETFIE returns to the previous state.
 */
#define PIC18_INTSTAT_MAIN      0  /* main routine */
#define PIC18_INTSTAT_LOW       1  /* low-priority ISR, from main */
#define PIC18_INTSTAT_HIGH      2  /* high-priority ISR, from main */
#define PIC18_INTSTAT_HIGH_LOW  3  /* high-priority ISR, from a low ISR */

/* PCON0 bit positions. The "n" bits are active low: 0 means "this happened". */
#define PIC18_PCON0_STKOVF 7
#define PIC18_PCON0_STKUNF 6
#define PIC18_PCON0_WDTWV  5
#define PIC18_PCON0_nRWDT  4
#define PIC18_PCON0_nRMCLR 3
#define PIC18_PCON0_nRI    2
#define PIC18_PCON0_nPOR   1
#define PIC18_PCON0_nBOR   0
/* Power-on value: POR and BOR flagged, everything else negated. */
#define PIC18_PCON0_POWERON 0x3C

/* PCON1: only nMEMV, which reads 1 (no memory violation). */
#define PIC18_PCON1_VALUE 0x02

/* CONFIG3 bit 3: multi-vector interrupts (unimplemented; must be 0). */
#define PIC18_CONFIG3_MVECEN (1u << 3)
/* CONFIG4 bit 4: a stack overflow or underflow forces a reset. */
#define PIC18_CONFIG4_STVREN (1u << 4)
/* CONFIG5 bits 6:5 Watchdog Timer Enable, bits 4:0 the period select. */
#define PIC18_CONFIG5_WDTE_SHIFT  5
#define PIC18_CONFIG5_WDTE_MASK   (3u << PIC18_CONFIG5_WDTE_SHIFT)
#define PIC18_CONFIG5_WDTCPS_MASK 0x1Fu

typedef struct CPUArchState {
    uint32_t pc;        /* program counter, byte address, 21 bits */

    uint32_t wreg;      /* working register, 8 bits */
    uint32_t bsr;       /* bank select register, 6 bits */
    uint32_t pclath;    /* PC high latch, 8 bits */
    uint32_t pclatu;    /* PC upper latch, 5 bits */
    uint32_t intcon0;   /* GIE/GIEH, GIEL, IPEN, INTxEDG */
    uint32_t intstat;   /* INTCON1 STAT[1:0]: the ISR nesting state */
    uint32_t cpudoze;   /* stored, not acted on */
    uint32_t fsr[3];    /* file select registers, 14 bits each */
    uint32_t prodl;
    uint32_t prodh;
    uint32_t tablat;
    uint32_t tblptr;    /* table pointer, 22 bits */

    /*
     * STATUS is kept as separate words rather than packed, so that translated
     * code can update one flag without a read-modify-write. TO and PD hold
     * the architectural bit value: 1 is the normal (not-timed-out,
     * not-powered-down) state.
     */
    uint32_t sregC;
    uint32_t sregDC;
    uint32_t sregZ;
    uint32_t sregOV;
    uint32_t sregN;
    uint32_t sregPD;
    uint32_t sregTO;

    /* Hardware stack; entry 0 is the empty slot. */
    uint32_t stack[PIC18_STACK_DEPTH + 1];
    uint32_t stkptr;
    /*
     * Reset-cause flags. These deliberately survive a reset -- that is what
     * they are for -- and are cleared only by software writing PCON0.
     */
    uint32_t stkovf;
    uint32_t stkunf;
    uint32_t reset_ri;  /* the last reset came from a RESET instruction */
    uint32_t reset_wdt; /* the SoC may set this before a watchdog reset */
    uint32_t pcon0;     /* the software-writable PCON0 bits (WDTWV..nBOR) */

    /*
     * Interrupt shadow registers, main-routine context: saved when an
     * interrupt is taken from the main routine, restored by RETFIE 1 from a
     * high-priority ISR entered from main or from a low-priority ISR.
     */
    uint32_t shadow_status;
    uint32_t shadow_wreg;
    uint32_t shadow_bsr;
    uint32_t shadow_fsr[3];
    uint32_t shadow_prodl;
    uint32_t shadow_prodh;
    uint32_t shadow_pclath;
    uint32_t shadow_pclatu;

    /* Low-ISR context: saved when a high-priority interrupt preempts a low one. */
    uint32_t shadow_lo_status;
    uint32_t shadow_lo_wreg;
    uint32_t shadow_lo_bsr;
    uint32_t shadow_lo_fsr[3];
    uint32_t shadow_lo_prodl;
    uint32_t shadow_lo_prodh;
    uint32_t shadow_lo_pclath;
    uint32_t shadow_lo_pclatu;

    /* Call shadow registers: CALL k, 1 saves, RETURN 1 restores. */
    uint32_t cshad_status;
    uint32_t cshad_wreg;
    uint32_t cshad_bsr;

    uint32_t intreq;    /* asserted request lines, bit PIC18_IRQ_HIGH/LOW */

    uint32_t config[PIC18_NUM_CONFIG];
} CPUPIC18State;

/**
 * PIC18CPU:
 * @env: #CPUPIC18State
 */
struct ArchCPU {
    CPUState parent_obj;

    CPUPIC18State env;

    /* Pulsed by CLRWDT, so the watchdog can see it without polling. */
    qemu_irq clrwdt;
};

/**
 * PIC18CPUClass:
 * @parent_realize: the parent class' realize handler.
 * @parent_phases: the parent class' reset phase handlers.
 */
struct PIC18CPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

extern const struct VMStateDescription vms_pic18_cpu;

void pic18_cpu_do_interrupt(CPUState *cpu);
bool pic18_cpu_exec_interrupt(CPUState *cpu, int int_req);
hwaddr pic18_cpu_get_phys_addr_debug(CPUState *cpu, vaddr addr);
bool pic18_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr);
int pic18_pending_interrupt(const CPUPIC18State *env);

int pic18_print_insn(bfd_vma addr, disassemble_info *info);

void pic18_cpu_tcg_init(void);
void pic18_cpu_translate_code(CPUState *cs, TranslationBlock *tb,
                              int *max_insns, vaddr pc, void *host_pc);

/* Configuration bytes: CONFIGn is at 0x300000 + n - 1, so config[n - 1]. */
static inline unsigned pic18_wdte(const CPUPIC18State *env)
{
    return (env->config[4] & PIC18_CONFIG5_WDTE_MASK) >> PIC18_CONFIG5_WDTE_SHIFT;
}

static inline unsigned pic18_wdtcps(const CPUPIC18State *env)
{
    return env->config[4] & PIC18_CONFIG5_WDTCPS_MASK;
}

static inline bool pic18_stvren(const CPUPIC18State *env)
{
    return (env->config[3] & PIC18_CONFIG4_STVREN) != 0;
}

static inline bool pic18_mvecen(const CPUPIC18State *env)
{
    return (env->config[2] & PIC18_CONFIG3_MVECEN) != 0;
}

static inline uint8_t cpu_get_status(const CPUPIC18State *env)
{
    return env->sregC  << PIC18_STATUS_C
         | env->sregDC << PIC18_STATUS_DC
         | env->sregZ  << PIC18_STATUS_Z
         | env->sregOV << PIC18_STATUS_OV
         | env->sregN  << PIC18_STATUS_N
         | env->sregPD << PIC18_STATUS_PD
         | env->sregTO << PIC18_STATUS_TO;
}

/* TO and PD are read-only. */
static inline void cpu_set_status(CPUPIC18State *env, uint8_t status)
{
    env->sregC  = (status >> PIC18_STATUS_C)  & 1;
    env->sregDC = (status >> PIC18_STATUS_DC) & 1;
    env->sregZ  = (status >> PIC18_STATUS_Z)  & 1;
    env->sregOV = (status >> PIC18_STATUS_OV) & 1;
    env->sregN  = (status >> PIC18_STATUS_N)  & 1;
}

/*
 * The shadow registers as the SoC exposes them at STATUS_SHAD .. PRODH_SHAD
 * (0x377-0x383): index 0 STATUS, 1 WREG, 2 BSR, 3 PCLATH, 4 PCLATU,
 * 5 FSR0L, 6 FSR0H, 7 FSR1L, 8 FSR1H, 9 FSR2L, 10 FSR2H, 11 PRODL, 12 PRODH.
 * @low selects the low-ISR context (SHADCON.SHADLO = 1).
 */
#define PIC18_NUM_SHADOW 13

uint8_t pic18_shadow_read(const CPUPIC18State *env, unsigned index, bool low);
void pic18_shadow_write(CPUPIC18State *env, unsigned index, bool low,
                        uint8_t value);

#endif /* QEMU_PIC18_CPU_H */
