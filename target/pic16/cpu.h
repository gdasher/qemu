/*
 * PIC16 (enhanced mid-range) CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_PIC16_CPU_H
#define QEMU_PIC16_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "system/memory.h"

#ifdef CONFIG_USER_ONLY
#error "PIC16 does not support user mode"
#endif

#define CPU_RESOLVING_TYPE TYPE_PIC16_CPU

/*
 * Harvard architecture: instruction fetch and data access use separate
 * address spaces that both start at zero.
 */
#define MMU_CODE_IDX 0
#define MMU_DATA_IDX 1

/*
 * Offsets of the two spaces within the host address space.
 *
 * Program memory holds 14-bit instructions one per 16-bit little-endian word,
 * so a word address maps to byte address * 2. The 15-bit PC spans 32K words,
 * hence 64 KB of code space.
 *
 * The data window is laid out to match the FSR view exactly (see
 * PORTING-PLAN.md D2): banked data at 0x0000, the linear GPR window at
 * 0x2000, and program memory as data at 0x8000.
 */
#define OFFSET_CODE 0x00000000
/*
 * Word addresses 0x8000 and up hold the configuration words, Device
 * Information Area and Device Configuration Information. They are not
 * executable, but they appear in the same HEX file as the program, so they get
 * a region immediately above the program flash and are loaded in one pass.
 */
#define OFFSET_CONFIG 0x00010000
#define OFFSET_DATA 0x00800000

#define PIC16_CODE_SIZE 0x10000   /* 32K words, word address * 2 */
#define PIC16_CONFIG_SIZE 0x10000 /* word 0x8000-0xFFFF */
#define PIC16_DATA_SIZE 0x10000

/* Data address space subdivisions, as seen through an FSR. */
#define PIC16_BANKED_BASE   0x0000  /* 64 banks of 128 bytes */
#define PIC16_BANKED_SIZE   0x2000
#define PIC16_LINEAR_BASE   0x2000  /* GPR blocks of banks 0..50, packed */
#define PIC16_LINEAR_SIZE   0x0FF0
#define PIC16_PFM_BASE      0x8000  /* program memory, low byte of each word */

#define PIC16_BANK_SIZE     0x80
#define PIC16_NUM_BANKS     64
#define PIC16_CORE_REGS     0x0C    /* per-bank core registers, 0x00-0x0B */
#define PIC16_GPR_BASE      0x20    /* per-bank GPR, 0x20-0x6F */
#define PIC16_GPR_SIZE      0x50
#define PIC16_COMMON_BASE   0x70    /* aliased into every bank */
#define PIC16_COMMON_SIZE   0x10

/* 16-level hardware stack, 15 bits per entry, not part of data memory. */
#define PIC16_STACK_DEPTH 16

/* Reset and the single interrupt vector, as word addresses. */
#define PIC16_RESET_VECTOR 0x0000
#define PIC16_INT_VECTOR   0x0004

/* Configuration words live at word address 0x8007 upwards. */
#define PIC16_CONFIG_BASE  0x8007
#define PIC16_NUM_CONFIG   5

#define EXCP_RESET 1
#define EXCP_INT   2

/* STATUS bit positions (DS40002637A 9.7.4). TO and PD are read-only. */
#define PIC16_STATUS_C  0
#define PIC16_STATUS_DC 1
#define PIC16_STATUS_Z  2
#define PIC16_STATUS_PD 3
#define PIC16_STATUS_TO 4

/* INTCON bit positions. */
#define PIC16_INTCON_GIE  7
#define PIC16_INTCON_PEIE 6

typedef enum PIC16Feature {
    PIC16_FEATURE_EEPROM,
} PIC16Feature;

typedef struct CPUArchState {
    uint32_t pc_w;      /* program counter, word address, 15 bits */

    uint32_t wreg;      /* working register, 8 bits */
    uint32_t bsr;       /* bank select register, 6 bits */
    uint32_t pclath;    /* write latch for the high PC bits, 7 bits */
    uint32_t intcon;    /* GIE, PEIE, INTEDG */
    uint32_t fsr[2];    /* file select registers, 16 bits each */

    /*
     * STATUS is kept as separate words rather than packed, so that translated
     * code can update one flag without a read-modify-write. TO and PD hold
     * the architectural bit value: 1 is the normal (not-timed-out,
     * not-powered-down) state.
     */
    uint32_t sregC;
    uint32_t sregDC;
    uint32_t sregZ;
    uint32_t sregPD;
    uint32_t sregTO;

    /* Hardware stack. */
    uint32_t stack[PIC16_STACK_DEPTH];
    uint32_t stkptr;

    /* Shadow registers, saved and restored automatically around interrupts. */
    uint32_t shadow_wreg;
    uint32_t shadow_status;
    uint32_t shadow_bsr;
    uint32_t shadow_fsr[2];
    uint32_t shadow_pclath;

    uint64_t intsrc;    /* pending interrupt sources */

    uint32_t config[PIC16_NUM_CONFIG];
    uint64_t features;
} CPUPIC16State;

/**
 * PIC16CPU:
 * @env: #CPUPIC16State
 */
struct ArchCPU {
    CPUState parent_obj;

    CPUPIC16State env;
};

/**
 * PIC16CPUClass:
 * @parent_realize: the parent class' realize handler.
 * @parent_phases: the parent class' reset phase handlers.
 */
struct PIC16CPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

extern const struct VMStateDescription vms_pic16_cpu;

void pic16_cpu_do_interrupt(CPUState *cpu);
bool pic16_cpu_exec_interrupt(CPUState *cpu, int int_req);
hwaddr pic16_cpu_get_phys_addr_debug(CPUState *cpu, vaddr addr);
bool pic16_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr);

int pic16_print_insn(bfd_vma addr, disassemble_info *info);

void pic16_cpu_tcg_init(void);
void pic16_cpu_translate_code(CPUState *cs, TranslationBlock *tb,
                              int *max_insns, vaddr pc, void *host_pc);

static inline int pic16_feature(CPUPIC16State *env, PIC16Feature feature)
{
    return (env->features & (1U << feature)) != 0;
}

static inline void set_pic16_feature(CPUPIC16State *env, int feature)
{
    env->features |= (1U << feature);
}

static inline int cpu_interrupts_enabled(CPUPIC16State *env)
{
    return (env->intcon >> PIC16_INTCON_GIE) & 1;
}

static inline uint8_t cpu_get_status(CPUPIC16State *env)
{
    return env->sregC  << PIC16_STATUS_C
         | env->sregDC << PIC16_STATUS_DC
         | env->sregZ  << PIC16_STATUS_Z
         | env->sregPD << PIC16_STATUS_PD
         | env->sregTO << PIC16_STATUS_TO;
}

static inline void cpu_set_status(CPUPIC16State *env, uint8_t status)
{
    env->sregC  = (status >> PIC16_STATUS_C)  & 1;
    env->sregDC = (status >> PIC16_STATUS_DC) & 1;
    env->sregZ  = (status >> PIC16_STATUS_Z)  & 1;
    env->sregPD = (status >> PIC16_STATUS_PD) & 1;
    env->sregTO = (status >> PIC16_STATUS_TO) & 1;
}

#endif /* QEMU_PIC16_CPU_H */
