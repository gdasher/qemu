/*
 * PIC18 CPU helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/helper-proto.h"
#include "qemu/plugin.h"
#include "hw/core/irq.h"
#include "system/runstate.h"
#include "qemu/main-loop.h"

/*
 * Interrupts
 *
 * The SoC drives two level lines: a high-priority and a low-priority request.
 * Which one is taken depends on GIE/GIEH, GIEL, IPEN and the controller's
 * execution state (INTCON1.STAT). On this core the hardware does not clear
 * GIE on entry; the state machine gates re-entry and RETFIE steps it back
 * (DS40002147F 11.5, 11.7). Returns the vector to take, or -1.
 */
int pic18_pending_interrupt(const CPUPIC18State *env)
{
    uint32_t req = env->intreq;
    bool gie = env->intcon0 & (1u << PIC18_INTCON0_GIE);
    bool giel = env->intcon0 & (1u << PIC18_INTCON0_GIEL);
    bool ipen = env->intcon0 & (1u << PIC18_INTCON0_IPEN);

    if (req == 0 || !gie) {
        return -1;
    }

    if (!ipen) {
        /* No priorities: every request is high priority, no nesting. */
        return env->intstat == PIC18_INTSTAT_MAIN ? PIC18_HIGH_INT_VECTOR : -1;
    }

    switch (env->intstat) {
    case PIC18_INTSTAT_MAIN:
        if (req & (1u << PIC18_IRQ_HIGH)) {
            return PIC18_HIGH_INT_VECTOR;
        }
        if (giel && (req & (1u << PIC18_IRQ_LOW))) {
            return PIC18_LOW_INT_VECTOR;
        }
        return -1;
    case PIC18_INTSTAT_LOW:
        /* A high-priority request preempts a low-priority handler. */
        if (req & (1u << PIC18_IRQ_HIGH)) {
            return PIC18_HIGH_INT_VECTOR;
        }
        return -1;
    default:
        return -1;
    }
}

bool pic18_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUPIC18State *env = cpu_env(cs);

    if (interrupt_request & CPU_INTERRUPT_HARD) {
        if (pic18_pending_interrupt(env) >= 0) {
            cs->exception_index = EXCP_INT;
            pic18_cpu_do_interrupt(cs);
            return true;
        }
    }
    return false;
}

/* The context the hardware saves: STATUS, WREG, BSR, FSR0-2, PROD, PCLAT. */
static void save_context_main(CPUPIC18State *env)
{
    env->shadow_status = cpu_get_status(env);
    env->shadow_wreg = env->wreg;
    env->shadow_bsr = env->bsr;
    env->shadow_fsr[0] = env->fsr[0];
    env->shadow_fsr[1] = env->fsr[1];
    env->shadow_fsr[2] = env->fsr[2];
    env->shadow_prodl = env->prodl;
    env->shadow_prodh = env->prodh;
    env->shadow_pclath = env->pclath;
    env->shadow_pclatu = env->pclatu;
}

static void save_context_low(CPUPIC18State *env)
{
    env->shadow_lo_status = cpu_get_status(env);
    env->shadow_lo_wreg = env->wreg;
    env->shadow_lo_bsr = env->bsr;
    env->shadow_lo_fsr[0] = env->fsr[0];
    env->shadow_lo_fsr[1] = env->fsr[1];
    env->shadow_lo_fsr[2] = env->fsr[2];
    env->shadow_lo_prodl = env->prodl;
    env->shadow_lo_prodh = env->prodh;
    env->shadow_lo_pclath = env->pclath;
    env->shadow_lo_pclatu = env->pclatu;
}

/* STATUS is restored except for TO and PD, which are not part of it. */
static void restore_context_main(CPUPIC18State *env)
{
    cpu_set_status(env, env->shadow_status);
    env->wreg = env->shadow_wreg;
    env->bsr = env->shadow_bsr;
    env->fsr[0] = env->shadow_fsr[0];
    env->fsr[1] = env->shadow_fsr[1];
    env->fsr[2] = env->shadow_fsr[2];
    env->prodl = env->shadow_prodl;
    env->prodh = env->shadow_prodh;
    env->pclath = env->shadow_pclath;
    env->pclatu = env->shadow_pclatu;
}

static void restore_context_low(CPUPIC18State *env)
{
    cpu_set_status(env, env->shadow_lo_status);
    env->wreg = env->shadow_lo_wreg;
    env->bsr = env->shadow_lo_bsr;
    env->fsr[0] = env->shadow_lo_fsr[0];
    env->fsr[1] = env->shadow_lo_fsr[1];
    env->fsr[2] = env->shadow_lo_fsr[2];
    env->prodl = env->shadow_lo_prodl;
    env->prodh = env->shadow_lo_prodh;
    env->pclath = env->shadow_lo_pclath;
    env->pclatu = env->shadow_lo_pclatu;
}

void pic18_cpu_do_interrupt(CPUState *cs)
{
    CPUPIC18State *env = cpu_env(cs);
    uint32_t ret = env->pc;
    int vector = pic18_pending_interrupt(env);

    cs->exception_index = -1;
    if (vector < 0) {
        return;
    }

    /*
     * Push the return address, save the context into the shadow set for the
     * state being left, advance the state machine and vector. Legacy vectors
     * only (MVECEN = 0): the handler polls the PIR flags to find the source.
     */
    if (pic18_mvecen(env)) {
        qemu_log_mask(LOG_UNIMP, "pic18: MVECEN=1 (vectored interrupts) is "
                      "not implemented; using the legacy vectors\n");
    }

    helper_push_stack(env, ret);

    if (env->intstat == PIC18_INTSTAT_MAIN) {
        save_context_main(env);
        env->intstat = vector == PIC18_HIGH_INT_VECTOR ? PIC18_INTSTAT_HIGH
                                                        : PIC18_INTSTAT_LOW;
    } else {
        /* Only a high-priority request reaches here, from a low ISR. */
        save_context_low(env);
        env->intstat = PIC18_INTSTAT_HIGH_LOW;
    }

    env->pc = vector;

    qemu_plugin_vcpu_interrupt_cb(cs, ret);
}

uint32_t helper_retfie(CPUPIC18State *env, uint32_t s)
{
    /*
     * Return from interrupt: step the state machine back and, for RETFIE 1,
     * restore the context saved on the way in (DS40002147F 11.7). RETFIE 1
     * outside any ISR restores the main shadows, which is what a debugger
     * would see the hardware do as well.
     */
    switch (env->intstat) {
    case PIC18_INTSTAT_HIGH_LOW:
        if (s) {
            restore_context_low(env);
        }
        env->intstat = PIC18_INTSTAT_LOW;
        break;
    default:
        if (s) {
            restore_context_main(env);
        }
        env->intstat = PIC18_INTSTAT_MAIN;
        break;
    }

    return helper_pop_stack(env);
}

/* CALL k, 1 and RETURN 1 use a separate, one-level shadow set. */
void helper_call_shadow(CPUPIC18State *env)
{
    env->cshad_status = cpu_get_status(env);
    env->cshad_wreg = env->wreg;
    env->cshad_bsr = env->bsr;
}

void helper_return_shadow(CPUPIC18State *env)
{
    cpu_set_status(env, env->cshad_status);
    env->wreg = env->cshad_wreg;
    env->bsr = env->cshad_bsr;
}

/*
 * The shadow registers as the SoC exposes them at STATUS_SHAD .. PRODH_SHAD.
 */
static uint32_t *shadow_slot(CPUPIC18State *env, unsigned index, bool low,
                             unsigned *shift)
{
    *shift = 0;
    switch (index) {
    case 0:
        return low ? &env->shadow_lo_status : &env->shadow_status;
    case 1:
        return low ? &env->shadow_lo_wreg : &env->shadow_wreg;
    case 2:
        return low ? &env->shadow_lo_bsr : &env->shadow_bsr;
    case 3:
        return low ? &env->shadow_lo_pclath : &env->shadow_pclath;
    case 4:
        return low ? &env->shadow_lo_pclatu : &env->shadow_pclatu;
    case 5 ... 10:
        *shift = (index - 5) & 1 ? 8 : 0;
        return low ? &env->shadow_lo_fsr[(index - 5) / 2]
                   : &env->shadow_fsr[(index - 5) / 2];
    case 11:
        return low ? &env->shadow_lo_prodl : &env->shadow_prodl;
    case 12:
        return low ? &env->shadow_lo_prodh : &env->shadow_prodh;
    default:
        return NULL;
    }
}

uint8_t pic18_shadow_read(const CPUPIC18State *env, unsigned index, bool low)
{
    unsigned shift;
    uint32_t *slot = shadow_slot((CPUPIC18State *)env, index, low, &shift);

    return slot ? (*slot >> shift) & 0xFF : 0;
}

void pic18_shadow_write(CPUPIC18State *env, unsigned index, bool low,
                        uint8_t value)
{
    unsigned shift;
    uint32_t *slot = shadow_slot(env, index, low, &shift);

    if (slot) {
        *slot = (*slot & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    }
}

hwaddr pic18_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr;
}

bool pic18_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr)
{
    int prot;
    uint32_t paddr;

    address &= TARGET_PAGE_MASK;

    if (mmu_idx == MMU_CODE_IDX) {
        paddr = OFFSET_CODE + address;
        prot = PAGE_READ | PAGE_EXEC;
    } else {
        paddr = OFFSET_DATA + address;
        prot = PAGE_READ | PAGE_WRITE;
    }

    tlb_set_page(cs, address, paddr, prot, mmu_idx, TARGET_PAGE_SIZE);
    return true;
}

/*
 * Core registers reached at run time: through an FSR, through a banked
 * access that lands in bank 4, or the few that the translator does not
 * handle inline. They live in CPUPIC18State, not in memory. Doing this in C
 * also makes the writes safe: TCG spills its globals around a helper call, so
 * updating env here cannot be lost.
 */

static bool is_indirect_reg(uint32_t addr)
{
    return (addr >= PIC18_REG_PLUSW2 && addr <= PIC18_REG_INDF2)
        || (addr >= PIC18_REG_PLUSW1 && addr <= PIC18_REG_INDF1)
        || (addr >= PIC18_REG_PLUSW0 && addr <= PIC18_REG_INDF0);
}

static uint32_t pcon0_read(CPUPIC18State *env)
{
    uint32_t value = env->pcon0 & 0x3F;

    if (env->stkovf) {
        value |= 1u << PIC18_PCON0_STKOVF;
    }
    if (env->stkunf) {
        value |= 1u << PIC18_PCON0_STKUNF;
    }
    if (env->reset_ri) {
        value &= ~(1u << PIC18_PCON0_nRI);
    }
    if (env->reset_wdt) {
        value &= ~(1u << PIC18_PCON0_nRWDT);
    }
    return value;
}

static void pcon0_write(CPUPIC18State *env, uint32_t value)
{
    env->pcon0 = value & 0x3F;
    env->stkovf = (value >> PIC18_PCON0_STKOVF) & 1;
    env->stkunf = (value >> PIC18_PCON0_STKUNF) & 1;
    env->reset_ri = !((value >> PIC18_PCON0_nRI) & 1);
    env->reset_wdt = !((value >> PIC18_PCON0_nRWDT) & 1);
}

static uint32_t core_reg_read(CPUPIC18State *env, uint32_t addr, uintptr_t ra)
{
    switch (addr) {
    case PIC18_REG_INTCON0:
        return env->intcon0;
    case PIC18_REG_INTCON1:
        return env->intstat << 6;
    case PIC18_REG_STATUS:
        return cpu_get_status(env);
    case PIC18_REG_FSR2L:
        return env->fsr[2] & 0xFF;
    case PIC18_REG_FSR2H:
        return (env->fsr[2] >> 8) & 0xFF;
    case PIC18_REG_BSR:
        return env->bsr;
    case PIC18_REG_FSR1L:
        return env->fsr[1] & 0xFF;
    case PIC18_REG_FSR1H:
        return (env->fsr[1] >> 8) & 0xFF;
    case PIC18_REG_WREG:
        return env->wreg;
    case PIC18_REG_FSR0L:
        return env->fsr[0] & 0xFF;
    case PIC18_REG_FSR0H:
        return (env->fsr[0] >> 8) & 0xFF;
    case PIC18_REG_PCON0:
        return pcon0_read(env);
    case PIC18_REG_PCON1:
        return PIC18_PCON1_VALUE;
    case PIC18_REG_CPUDOZE:
        return env->cpudoze;
    case PIC18_REG_PRODL:
        return env->prodl;
    case PIC18_REG_PRODH:
        return env->prodh;
    case PIC18_REG_TABLAT:
        return env->tablat;
    case PIC18_REG_TBLPTRL:
        return env->tblptr & 0xFF;
    case PIC18_REG_TBLPTRH:
        return (env->tblptr >> 8) & 0xFF;
    case PIC18_REG_TBLPTRU:
        return (env->tblptr >> 16) & 0xFF;
    case PIC18_REG_PCL:
        /* env->pc was committed before the call: the next instruction. */
        env->pclath = (env->pc >> 8) & 0xFF;
        env->pclatu = (env->pc >> 16) & 0x1F;
        return env->pc & 0xFF;
    case PIC18_REG_PCLATH:
        return env->pclath;
    case PIC18_REG_PCLATU:
        return env->pclatu;
    case PIC18_REG_STKPTR:
        return env->stkptr;
    case PIC18_REG_TOSL:
        return env->stack[env->stkptr] & 0xFF;
    case PIC18_REG_TOSH:
        return (env->stack[env->stkptr] >> 8) & 0xFF;
    case PIC18_REG_TOSU:
        return (env->stack[env->stkptr] >> 16) & 0x1F;
    default:
        g_assert_not_reached();
    }
}

static void core_reg_write(CPUPIC18State *env, uint32_t addr, uint32_t value,
                           uintptr_t ra)
{
    CPUState *cs = env_cpu(env);

    value &= 0xFF;

    switch (addr) {
    case PIC18_REG_INTCON0:
        env->intcon0 = value;
        /*
         * Enabling interrupts from inside a block: re-arm the exit check so
         * an already asserted request is taken at the next block boundary.
         */
        if (env->intreq) {
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
        }
        break;
    case PIC18_REG_INTCON1:
        break; /* read-only */
    case PIC18_REG_STATUS:
        cpu_set_status(env, value);
        break;
    case PIC18_REG_FSR2L:
        env->fsr[2] = (env->fsr[2] & 0x3F00) | value;
        break;
    case PIC18_REG_FSR2H:
        env->fsr[2] = (env->fsr[2] & 0x00FF) | ((value & 0x3F) << 8);
        break;
    case PIC18_REG_BSR:
        env->bsr = value & 0x3F;
        break;
    case PIC18_REG_FSR1L:
        env->fsr[1] = (env->fsr[1] & 0x3F00) | value;
        break;
    case PIC18_REG_FSR1H:
        env->fsr[1] = (env->fsr[1] & 0x00FF) | ((value & 0x3F) << 8);
        break;
    case PIC18_REG_WREG:
        env->wreg = value;
        break;
    case PIC18_REG_FSR0L:
        env->fsr[0] = (env->fsr[0] & 0x3F00) | value;
        break;
    case PIC18_REG_FSR0H:
        env->fsr[0] = (env->fsr[0] & 0x00FF) | ((value & 0x3F) << 8);
        break;
    case PIC18_REG_PCON0:
        pcon0_write(env, value);
        break;
    case PIC18_REG_PCON1:
        break; /* read-only */
    case PIC18_REG_CPUDOZE:
        env->cpudoze = value;
        break;
    case PIC18_REG_PRODL:
        env->prodl = value;
        break;
    case PIC18_REG_PRODH:
        env->prodh = value;
        break;
    case PIC18_REG_TABLAT:
        env->tablat = value;
        break;
    case PIC18_REG_TBLPTRL:
        env->tblptr = (env->tblptr & 0x3FFF00) | value;
        break;
    case PIC18_REG_TBLPTRH:
        env->tblptr = (env->tblptr & 0x3F00FF) | (value << 8);
        break;
    case PIC18_REG_TBLPTRU:
        env->tblptr = (env->tblptr & 0x00FFFF) | ((value & 0x3F) << 16);
        break;
    case PIC18_REG_PCL:
        /*
         * A write to PCL is a computed jump, taking the high bits from
         * PCLATU:PCLATH. Returning normally would resume at the old address,
         * so unwind to this instruction (which also settles the icount
         * budget), set the new PC and re-enter the main loop there.
         */
        cpu_restore_state(cs, ra);
        env->pc = ((env->pclatu << 16) | (env->pclath << 8) | value)
                  & PIC18_PC_MASK;
        cpu_loop_exit(cs);
        break;
    case PIC18_REG_PCLATH:
        env->pclath = value;
        break;
    case PIC18_REG_PCLATU:
        env->pclatu = value & 0x1F;
        break;
    case PIC18_REG_STKPTR:
        env->stkptr = value & PIC18_STKPTR_MASK;
        break;
    case PIC18_REG_TOSL:
        env->stack[env->stkptr] = (env->stack[env->stkptr] & 0x1FFF00) | value;
        break;
    case PIC18_REG_TOSH:
        env->stack[env->stkptr] = (env->stack[env->stkptr] & 0x1F00FF)
                                  | (value << 8);
        break;
    case PIC18_REG_TOSU:
        env->stack[env->stkptr] = (env->stack[env->stkptr] & 0x00FFFF)
                                  | ((value & 0x1F) << 16);
        break;
    default:
        g_assert_not_reached();
    }
}

uint32_t helper_ld_core(CPUPIC18State *env, uint32_t addr)
{
    return core_reg_read(env, addr, GETPC());
}

void helper_st_core(CPUPIC18State *env, uint32_t addr, uint32_t value)
{
    core_reg_write(env, addr, value, GETPC());
}

/*
 * Data access at a run-time address. The core registers resolve here;
 * everything else is a memory access.
 *
 * The indirect operands (INDFn, POSTINCn, ...) are not physical registers.
 * Reached through an FSR that points at them they read zero and ignore
 * writes (DS40002147F 9.5.3.3); reached directly -- a banked access with
 * BSR = 4 -- they perform the indirect access like the translator's own
 * inline path does, FSR side effects included.
 */
static uint32_t data_read(CPUPIC18State *env, uint32_t addr, bool via_fsr,
                          uintptr_t ra);
static void data_write(CPUPIC18State *env, uint32_t addr, uint32_t value,
                       bool via_fsr, uintptr_t ra);

static void indirect_operand(uint32_t reg, int *n, int *mode)
{
    if (reg >= PIC18_REG_PLUSW0) {
        *n = 0;
        *mode = reg - PIC18_REG_PLUSW0;
    } else if (reg >= PIC18_REG_PLUSW1) {
        *n = 1;
        *mode = reg - PIC18_REG_PLUSW1;
    } else {
        *n = 2;
        *mode = reg - PIC18_REG_PLUSW2;
    }
}

/*
 * The effective address of an indirect operand, and the FSR update it owes.
 * The update is applied only after the access: if the access is MMIO, QEMU
 * unwinds out of the load or store and restarts the instruction
 * (cpu_io_recompile), and an FSR already stepped would be stepped twice.
 */
static uint32_t indirect_addr(CPUPIC18State *env, int n, int mode)
{
    switch (mode) {
    case PIC18_IND_PREINC:
        return (env->fsr[n] + 1) & PIC18_FSR_MASK;
    case PIC18_IND_PLUSW:
        return (env->fsr[n] + (int8_t)env->wreg) & PIC18_FSR_MASK;
    default:
        return env->fsr[n];
    }
}

static void indirect_update(CPUPIC18State *env, int n, int mode, uint32_t addr)
{
    switch (mode) {
    case PIC18_IND_PREINC:
        env->fsr[n] = addr;
        break;
    case PIC18_IND_POSTINC:
        env->fsr[n] = (env->fsr[n] + 1) & PIC18_FSR_MASK;
        break;
    case PIC18_IND_POSTDEC:
        env->fsr[n] = (env->fsr[n] - 1) & PIC18_FSR_MASK;
        break;
    default:
        break;
    }
}

static uint32_t data_read(CPUPIC18State *env, uint32_t addr, bool via_fsr,
                          uintptr_t ra)
{
    addr &= PIC18_DATA_MASK;

    if (addr >= PIC18_CORE_SFR_BASE && addr < PIC18_SFR_END) {
        if (is_indirect_reg(addr)) {
            int n, mode;
            uint32_t target, value;

            if (via_fsr) {
                return 0;
            }
            indirect_operand(addr, &n, &mode);
            target = indirect_addr(env, n, mode);
            value = data_read(env, target, true, ra);
            indirect_update(env, n, mode, target);
            return value;
        }
        return core_reg_read(env, addr, ra);
    }
    return cpu_ldub_mmuidx_ra(env, addr, MMU_DATA_IDX, ra);
}

static void data_write(CPUPIC18State *env, uint32_t addr, uint32_t value,
                       bool via_fsr, uintptr_t ra)
{
    addr &= PIC18_DATA_MASK;

    if (addr >= PIC18_CORE_SFR_BASE && addr < PIC18_SFR_END) {
        if (is_indirect_reg(addr)) {
            int n, mode;
            uint32_t target;

            if (via_fsr) {
                return;
            }
            indirect_operand(addr, &n, &mode);
            target = indirect_addr(env, n, mode);
            data_write(env, target, value, true, ra);
            indirect_update(env, n, mode, target);
            return;
        }
        core_reg_write(env, addr, value, ra);
        return;
    }
    cpu_stb_mmuidx_ra(env, addr, value & 0xFF, MMU_DATA_IDX, ra);
}

/* Through an FSR. */
uint32_t helper_ld_data(CPUPIC18State *env, uint32_t addr)
{
    return data_read(env, addr, true, GETPC());
}

void helper_st_data(CPUPIC18State *env, uint32_t addr, uint32_t value)
{
    data_write(env, addr, value, true, GETPC());
}

/* Banked, with BSR selecting bank 4. */
uint32_t helper_ld_banked(CPUPIC18State *env, uint32_t addr)
{
    return data_read(env, addr, false, GETPC());
}

void helper_st_banked(CPUPIC18State *env, uint32_t addr, uint32_t value)
{
    data_write(env, addr, value, false, GETPC());
}

/*
 * Hardware stack. 127 entries; overflow and underflow set status bits that
 * can force a reset, which arrive with PCON0.
 */

/*
 * A guest-initiated reset is a power cycle: the whole board, not just the
 * core. Requesting one stops this vCPU and hands the work to the main loop,
 * which pauses the machine, resets every device and starts it again -- so
 * execution resumes at the reset vector with the peripherals at their
 * power-on values. The same path the watchdog takes.
 *
 * The request itself only marks the intent; nothing has been reset by the
 * time this returns. Callers must not assume the CPU state is fresh.
 */
static void reset_board(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

/*
 * Overflow and underflow always set their flag. Whether they also reset the
 * device is the STVREN configuration bit (DS40002147F 9.1.3.2).
 *
 * Unlike the RESET instruction this leaves the block running: a CALL or a
 * RETURN ends its block anyway, so the few remaining operations finish and the
 * reset lands at the boundary rather than mid-instruction.
 */
static void stack_fault(CPUPIC18State *env)
{
    if (pic18_stvren(env)) {
        reset_board();
    }
}

void helper_push_stack(CPUPIC18State *env, uint32_t value)
{
    if (env->stkptr >= PIC18_STACK_DEPTH) {
        /* The 128th push: flag it, overwrite the top, leave the pointer. */
        env->stkovf = 1;
        env->stkptr = PIC18_STACK_DEPTH;
        stack_fault(env);
    } else {
        env->stkptr++;
    }
    env->stack[env->stkptr] = value & PIC18_PC_MASK;
}

uint32_t helper_pop_stack(CPUPIC18State *env)
{
    uint32_t value;

    if (env->stkptr == 0) {
        env->stkunf = 1;
        stack_fault(env);
        return 0;
    }
    value = env->stack[env->stkptr];
    env->stkptr--;
    return value;
}

void helper_sleep(CPUPIC18State *env)
{
    CPUState *cs = env_cpu(env);

    env->sregTO = 1;
    env->sregPD = 0;

    cs->exception_index = EXCP_HLT;
    cs->halted = 1;
    cpu_loop_exit(cs);
}

void helper_reset(CPUPIC18State *env)
{
    /*
     * A software reset resets the whole device, not just the core, and leaves
     * PCON0's RI flag clear to say so.
     */
    env->reset_ri = 1;
    reset_board();
    cpu_loop_exit_noexc(env_cpu(env));
}

void helper_clrwdt(CPUPIC18State *env)
{
    env->sregTO = 1;
    env->sregPD = 1;
    qemu_irq_pulse(env_archcpu(env)->clrwdt);
}

/*
 * Table reads fetch one byte of program space at TBLPTR: flash, user IDs,
 * configuration and EEPROM are all there. mm: 0 no change, 1 post-increment,
 * 2 post-decrement, 3 pre-increment.
 */
void helper_tblrd(CPUPIC18State *env, uint32_t mm)
{
    uintptr_t ra = GETPC();
    uint32_t addr = env->tblptr;

    /* Read first, update after: the read may unwind (see data_read). */
    if (mm == 3) {
        addr = (addr + 1) & PIC18_TBLPTR_MASK;
    }
    env->tablat = cpu_ldub_mmuidx_ra(env, addr, MMU_CODE_IDX, ra);
    if (mm == 1) {
        env->tblptr = (addr + 1) & PIC18_TBLPTR_MASK;
    } else if (mm == 2) {
        env->tblptr = (addr - 1) & PIC18_TBLPTR_MASK;
    } else {
        env->tblptr = addr;
    }
}

/*
 * Table writes fill the NVM holding registers, which a programming sequence
 * through NVMCON then commits. Program memory writes are not modelled, so the
 * write is logged and only the pointer update happens.
 */
void helper_tblwt(CPUPIC18State *env, uint32_t mm)
{
    if (mm == 3) {
        env->tblptr = (env->tblptr + 1) & PIC18_TBLPTR_MASK;
    }
    qemu_log_mask(LOG_UNIMP, "pic18: TBLWT to 0x%06x (TABLAT 0x%02x) "
                  "unimplemented\n", env->tblptr, env->tablat);
    if (mm == 1) {
        env->tblptr = (env->tblptr + 1) & PIC18_TBLPTR_MASK;
    } else if (mm == 2) {
        env->tblptr = (env->tblptr - 1) & PIC18_TBLPTR_MASK;
    }
}

/*
 * Decimal adjust: corrects W after adding two packed BCD bytes. The low
 * nibble is adjusted on DC or a value above 9, the high nibble on C or a
 * value above 9; C is set when the high adjustment carries out
 * (DS40002147F 44.1.1, DAW), and otherwise left alone.
 */
void helper_daw(CPUPIC18State *env)
{
    uint32_t w = env->wreg;

    if ((w & 0x0F) > 9 || env->sregDC) {
        w += 0x06;
    }
    if ((w & 0xF0) > 0x90 || env->sregC) {
        w += 0x60;
    }
    if (w > 0xFF) {
        env->sregC = 1;
    }
    env->wreg = w & 0xFF;
}

void helper_unsupported(CPUPIC18State *env)
{
    CPUState *cs = env_cpu(env);

    /*
     * The hardware has no undefined-instruction trap; every 16-bit word
     * decodes to something. Reaching here means the decoder is incomplete,
     * so stop and let the user see where.
     */
    cs->exception_index = EXCP_DEBUG;
    if (qemu_loglevel_mask(LOG_UNIMP)) {
        qemu_log("UNSUPPORTED INSTRUCTION at 0x%06x\n", env->pc);
        cpu_dump_state(cs, stderr, 0);
    }
    cpu_loop_exit(cs);
}
