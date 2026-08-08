/*
 * QEMU MIPS interrupt support
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "target/mips/cpu.h"

static void cpu_mips_irq_request(void *opaque, int irq, int level)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;
    CPUState *cs = CPU(cpu);

    if (irq < 0 || irq > 7) {
        return;
    }

    BQL_LOCK_GUARD();

    if (level) {
        env->CP0_Cause |= 1 << (irq + CP0Ca_IP);
    } else {
        env->CP0_Cause &= ~(1 << (irq + CP0Ca_IP));
    }

    if (env->CP0_Cause & CP0Ca_IP_mask) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

void cpu_mips_irq_init_cpu(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;
    qemu_irq *qi;
    int i;

    qi = qemu_allocate_irqs(cpu_mips_irq_request, cpu, 8);
    for (i = 0; i < 8; i++) {
        env->irq[i] = qi[i];
    }
    g_free(qi);
}

void cpu_mips_soft_irq(CPUMIPSState *env, int irq, int level)
{
    if (irq < 0 || irq > 2) {
        return;
    }

    qemu_set_irq(env->irq[irq], level);
}

/*
 * An external interrupt controller running the EIC protocol makes its request
 * as a priority level and the offset of the handler that goes with it. The
 * core takes the request when the level exceeds the one in Status, so a level
 * of zero withdraws it.
 *
 * Cause holds the level because that is where the core compares it from, but
 * only the top six bits of the field: the low two are the software interrupt
 * lines, which the controller sees as sources of its own and which must
 * survive being written here.
 */
void cpu_mips_eic_request(MIPSCPU *cpu, unsigned ripl, uint32_t offset)
{
    CPUMIPSState *env = &cpu->env;
    CPUState *cs = CPU(cpu);
    uint32_t sw;

    /*
     * The controller reaches this from a device write, which holds the lock,
     * and from the core's own software interrupt lines, which do not: an mtc0
     * to Cause runs in the translated code's context.
     */
    BQL_LOCK_GUARD();

    sw = env->CP0_Cause & (3 << CP0Ca_IP);
    env->CP0_Cause &= ~CP0Ca_IP_mask;
    env->CP0_Cause |= ((ripl << 2) << CP0Ca_IP) & CP0Ca_IP_mask;
    env->CP0_Cause |= sw;
    env->eic_offset = offset;

    if (ripl) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}
