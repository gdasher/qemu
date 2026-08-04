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

bool pic16_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUPIC16State *env = cpu_env(cs);

    /*
     * A pending skip belongs to the instruction pair it was set by; taking an
     * interrupt between them would lose it, as the skip is not part of the
     * saved context.
     */
    if (env->skip) {
        return false;
    }

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
     * Push the return address, save the context that the hardware shadows,
     * clear GIE and vector. There is a single interrupt vector; the handler
     * polls the PIR flags to find the source.
     */
    env->stack[env->stkptr & (PIC16_STACK_DEPTH - 1)] = ret;
    env->stkptr = (env->stkptr + 1) & (PIC16_STACK_DEPTH - 1);

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
