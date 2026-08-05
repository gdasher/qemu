/*
 * PIC16 CPU helpers
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

bool pic16_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUPIC16State *env = cpu_env(cs);

    if (interrupt_request & CPU_INTERRUPT_HARD) {
        if (cpu_interrupts_enabled(env) && env->intsrc != 0) {
            cs->exception_index = EXCP_INT;
            pic16_cpu_do_interrupt(cs);
            return true;
        }
    }
    return false;
}

void pic16_cpu_do_interrupt(CPUState *cs)
{
    CPUPIC16State *env = cpu_env(cs);
    uint32_t ret = env->pc_w;

    /*
     * Push the return address, save the context the hardware shadows, clear
     * GIE and vector. There is a single interrupt vector; the handler polls
     * the PIR flags to find the source.
     */
    helper_push_stack(env, ret);

    env->shadow_wreg = env->wreg;
    env->shadow_status = cpu_get_status(env);
    env->shadow_bsr = env->bsr;
    env->shadow_fsr[0] = env->fsr[0];
    env->shadow_fsr[1] = env->fsr[1];
    env->shadow_pclath = env->pclath;

    env->intcon &= ~(1u << PIC16_INTCON_GIE);
    env->pc_w = PIC16_INT_VECTOR;

    cs->exception_index = -1;

    qemu_plugin_vcpu_interrupt_cb(cs, ret);
}

hwaddr pic16_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr;
}

bool pic16_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
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
 * Indirect data access.
 *
 * Everything reached through an FSR goes through here rather than through
 * generated loads and stores, because the FSR address space is not flat: the
 * low twelve bytes of every bank are the core registers (which live in
 * CPUPIC16State, not in memory), 0x2000-0x2FEF is a packed view of the GPR
 * blocks, and 0x8000 and up reads program memory. Doing this in C also makes
 * writes to the core registers safe: TCG spills its globals around a helper
 * call, so updating env here cannot be lost.
 */

static uint32_t linear_to_banked(uint32_t addr)
{
    uint32_t offset = addr - PIC16_LINEAR_BASE;
    uint32_t bank = offset / PIC16_GPR_SIZE;
    uint32_t index = offset % PIC16_GPR_SIZE;

    return bank * PIC16_BANK_SIZE + PIC16_GPR_BASE + index;
}

/*
 * The top sixteen bytes of every bank are the same common RAM. Direct
 * addressing resolves that at translation time, because f >= 0x70 needs no
 * BSR; reaching it through an FSR has to fold the bank away here.
 */
static uint32_t fold_common(uint32_t addr)
{
    if ((addr & (PIC16_BANK_SIZE - 1)) >= PIC16_COMMON_BASE) {
        return PIC16_COMMON_BASE | (addr & (PIC16_COMMON_SIZE - 1));
    }
    return addr;
}

static uint32_t core_reg_read(CPUPIC16State *env, uint32_t reg, uintptr_t ra)
{
    switch (reg) {
    case 0x00: /* INDF0 */
    case 0x01: /* INDF1 */
        /*
         * INDF is not a physical register. Reaching it through an FSR that
         * points at it reads zero, as on hardware.
         */
        return 0;
    case 0x02: /* PCL */
        return env->pc_w & 0xFF;
    case 0x03: /* STATUS */
        return cpu_get_status(env);
    case 0x04: /* FSR0L */
        return env->fsr[0] & 0xFF;
    case 0x05: /* FSR0H */
        return (env->fsr[0] >> 8) & 0xFF;
    case 0x06: /* FSR1L */
        return env->fsr[1] & 0xFF;
    case 0x07: /* FSR1H */
        return (env->fsr[1] >> 8) & 0xFF;
    case 0x08: /* BSR */
        return env->bsr;
    case 0x09: /* WREG */
        return env->wreg;
    case 0x0A: /* PCLATH */
        return env->pclath;
    case 0x0B: /* INTCON */
        return env->intcon;
    default:
        g_assert_not_reached();
    }
}

static void core_reg_write(CPUPIC16State *env, uint32_t reg, uint32_t value,
                           uintptr_t ra)
{
    switch (reg) {
    case 0x00: /* INDF0 */
    case 0x01: /* INDF1 */
        break;
    case 0x02: /* PCL */
        /*
         * A write to PCL is a computed jump, taking the high bits from
         * PCLATH. Returning normally would resume at the old address, so
         * unwind and re-enter the main loop at the new one.
         */
        env->pc_w = ((env->pclath & 0x7F) << 8) | (value & 0xFF);
        cpu_loop_exit_restore(env_cpu(env), ra);
        break;
    case 0x03: /* STATUS: TO and PD are read-only */
        env->sregC = (value >> PIC16_STATUS_C) & 1;
        env->sregDC = (value >> PIC16_STATUS_DC) & 1;
        env->sregZ = (value >> PIC16_STATUS_Z) & 1;
        break;
    case 0x04: /* FSR0L */
        env->fsr[0] = (env->fsr[0] & 0xFF00) | (value & 0xFF);
        break;
    case 0x05: /* FSR0H */
        env->fsr[0] = (env->fsr[0] & 0x00FF) | ((value & 0xFF) << 8);
        break;
    case 0x06: /* FSR1L */
        env->fsr[1] = (env->fsr[1] & 0xFF00) | (value & 0xFF);
        break;
    case 0x07: /* FSR1H */
        env->fsr[1] = (env->fsr[1] & 0x00FF) | ((value & 0xFF) << 8);
        break;
    case 0x08: /* BSR */
        env->bsr = value & 0x3F;
        break;
    case 0x09: /* WREG */
        env->wreg = value & 0xFF;
        break;
    case 0x0A: /* PCLATH */
        env->pclath = value & 0x7F;
        break;
    case 0x0B: /* INTCON */
        env->intcon = value & 0xFF;
        break;
    default:
        g_assert_not_reached();
    }
}

uint32_t helper_ld_data(CPUPIC16State *env, uint32_t addr)
{
    uintptr_t ra = GETPC();

    addr &= 0xFFFF;

    if (addr >= PIC16_PFM_BASE) {
        /* Program memory reads return the low byte of the addressed word. */
        uint32_t word = addr & 0x7FFF;
        return cpu_ldub_mmuidx_ra(env, word * 2, MMU_CODE_IDX, ra);
    }
    if (addr >= PIC16_LINEAR_BASE) {
        if (addr >= PIC16_LINEAR_BASE + PIC16_LINEAR_SIZE) {
            return 0; /* unimplemented, reads as zero */
        }
        addr = linear_to_banked(addr);
    } else {
        uint32_t offset = addr & (PIC16_BANK_SIZE - 1);

        if (offset < PIC16_CORE_REGS) {
            return core_reg_read(env, offset, ra);
        }
        addr = fold_common(addr);
    }

    return cpu_ldub_mmuidx_ra(env, addr, MMU_DATA_IDX, ra);
}

void helper_st_data(CPUPIC16State *env, uint32_t addr, uint32_t value)
{
    uintptr_t ra = GETPC();

    addr &= 0xFFFF;

    if (addr >= PIC16_PFM_BASE) {
        /* Program memory cannot be written through an FSR. */
        return;
    }
    if (addr >= PIC16_LINEAR_BASE) {
        if (addr >= PIC16_LINEAR_BASE + PIC16_LINEAR_SIZE) {
            return;
        }
        addr = linear_to_banked(addr);
    } else {
        uint32_t offset = addr & (PIC16_BANK_SIZE - 1);

        if (offset < PIC16_CORE_REGS) {
            core_reg_write(env, offset, value, ra);
            return;
        }
        addr = fold_common(addr);
    }

    cpu_stb_mmuidx_ra(env, addr, value & 0xFF, MMU_DATA_IDX, ra);
}

/*
 * Hardware stack. It is 16 entries deep and wraps; overflow and underflow set
 * status bits that can force a reset, which arrives with the SoC's PCON
 * registers.
 */

/*
 * Reset the core, from a context where the guest asked for it.
 *
 * The whole-machine path -- qemu_system_reset_request() -- is a main-loop
 * operation: it stops the vCPU and leaves the reset to be performed later.
 * Requesting it from a TCG helper reliably stopped the guest without the reset
 * ever being serviced, so the guest-initiated cases reset the core here and
 * leave peripheral state alone. Hardware resets the peripherals too; the
 * difference is observable, and is noted in the porting plan.
 *
 * The watchdog does not go through here. It expires in a timer callback, which
 * already runs in the main loop, so it takes the whole-machine path and does
 * reset the peripherals.
 */
static void reset_core(CPUPIC16State *env)
{
    cpu_reset(env_cpu(env));
}

/*
 * Overflow and underflow always set their flag. Whether they also reset the
 * device is the STVREN configuration bit; with it clear the stack behaves as a
 * circular buffer (DS40002637A 9.5).
 */
static void stack_fault(CPUPIC16State *env)
{
    if (pic16_stvren(env)) {
        reset_core(env);
    }
}

void helper_push_stack(CPUPIC16State *env, uint32_t value)
{
    uint32_t next = (env->stkptr + 1) & PIC16_STKPTR_MASK;

    if (next >= PIC16_STACK_DEPTH) {
        env->stkovf = 1;
        next = 0;
        stack_fault(env);
    }
    env->stkptr = next;
    env->stack[next] = value & 0x7FFF;
}

uint32_t helper_pop_stack(CPUPIC16State *env)
{
    uint32_t value;

    if (env->stkptr == PIC16_STKPTR_EMPTY) {
        env->stkunf = 1;
        stack_fault(env);
        return 0;
    }
    value = env->stack[env->stkptr];
    env->stkptr = (env->stkptr - 1) & PIC16_STKPTR_MASK;
    return value;
}

uint32_t helper_retfie(CPUPIC16State *env)
{
    /*
     * Returning from an interrupt restores the shadowed context. STATUS is
     * shadowed except for TO and PD, which are not part of the saved context
     * (DS40002637A 12.9).
     */
    env->wreg = env->shadow_wreg;
    env->sregC = (env->shadow_status >> PIC16_STATUS_C) & 1;
    env->sregDC = (env->shadow_status >> PIC16_STATUS_DC) & 1;
    env->sregZ = (env->shadow_status >> PIC16_STATUS_Z) & 1;
    env->bsr = env->shadow_bsr;
    env->fsr[0] = env->shadow_fsr[0];
    env->fsr[1] = env->shadow_fsr[1];
    env->pclath = env->shadow_pclath;

    env->intcon |= 1u << PIC16_INTCON_GIE;

    return helper_pop_stack(env);
}

void helper_sleep(CPUPIC16State *env)
{
    CPUState *cs = env_cpu(env);

    env->sregTO = 1;
    env->sregPD = 0;

    cs->exception_index = EXCP_HLT;
    cs->halted = 1;
    cpu_loop_exit(cs);
}

void helper_reset(CPUPIC16State *env)
{
    /*
     * A software reset resets the whole device, not just the core, and leaves
     * PCON0's RI flag clear to say so.
     */
    env->reset_ri = 1;
    reset_core(env);
    cpu_loop_exit_noexc(env_cpu(env));
}

void helper_clrwdt(CPUPIC16State *env)
{
    env->sregTO = 1;
    env->sregPD = 1;
    qemu_irq_pulse(env_archcpu(env)->clrwdt);
}

/*
 * The legacy TRIS instruction loads a port's direction register from W. Only
 * ports A, B and C are addressable, and their TRIS registers are consecutive,
 * so this is exactly a store to the matching address.
 */
void helper_tris(CPUPIC16State *env, uint32_t port)
{
    cpu_stb_mmuidx_ra(env, PIC16_TRIS_BASE + (port - 5), env->wreg,
                      MMU_DATA_IDX, GETPC());
}

void helper_unsupported(CPUPIC16State *env)
{
    CPUState *cs = env_cpu(env);

    /*
     * The hardware has no undefined-instruction trap; every 14-bit word
     * decodes to something. Reaching here means the decoder is incomplete,
     * so stop and let the user see where.
     */
    cs->exception_index = EXCP_DEBUG;
    if (qemu_loglevel_mask(LOG_UNIMP)) {
        qemu_log("UNSUPPORTED INSTRUCTION at word 0x%04x\n", env->pc_w);
        cpu_dump_state(cs, stderr, 0);
    }
    cpu_loop_exit(cs);
}
