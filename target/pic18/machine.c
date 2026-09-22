/*
 * PIC18 CPU migration state
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "migration/qemu-file-types.h"
#include "migration/vmstate.h"

static int get_status(QEMUFile *f, void *opaque, size_t size,
                      const VMStateField *field)
{
    CPUPIC18State *env = opaque;
    uint8_t status = qemu_get_byte(f);

    cpu_set_status(env, status);
    env->sregPD = (status >> PIC18_STATUS_PD) & 1;
    env->sregTO = (status >> PIC18_STATUS_TO) & 1;
    return 0;
}

static int put_status(QEMUFile *f, void *opaque, size_t size,
                      const VMStateField *field, JSONWriter *vmdesc)
{
    qemu_put_byte(f, cpu_get_status(opaque));
    return 0;
}

static const VMStateInfo vms_status = {
    .name = "status",
    .get = get_status,
    .put = put_status,
};

const VMStateDescription vms_pic18_cpu = {
    .name = "cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.pc, PIC18CPU),
        VMSTATE_UINT32(env.wreg, PIC18CPU),
        VMSTATE_UINT32(env.bsr, PIC18CPU),
        VMSTATE_UINT32(env.pclath, PIC18CPU),
        VMSTATE_UINT32(env.pclatu, PIC18CPU),
        VMSTATE_UINT32(env.intcon0, PIC18CPU),
        VMSTATE_UINT32(env.intstat, PIC18CPU),
        VMSTATE_UINT32(env.cpudoze, PIC18CPU),
        VMSTATE_UINT32_ARRAY(env.fsr, PIC18CPU, 3),
        VMSTATE_UINT32(env.prodl, PIC18CPU),
        VMSTATE_UINT32(env.prodh, PIC18CPU),
        VMSTATE_UINT32(env.tablat, PIC18CPU),
        VMSTATE_UINT32(env.tblptr, PIC18CPU),

        VMSTATE_SINGLE(env, PIC18CPU, 0, vms_status, CPUPIC18State),

        VMSTATE_UINT32_ARRAY(env.stack, PIC18CPU, PIC18_STACK_DEPTH + 1),
        VMSTATE_UINT32(env.stkptr, PIC18CPU),
        VMSTATE_UINT32(env.stkovf, PIC18CPU),
        VMSTATE_UINT32(env.stkunf, PIC18CPU),
        VMSTATE_UINT32(env.reset_ri, PIC18CPU),
        VMSTATE_UINT32(env.reset_wdt, PIC18CPU),
        VMSTATE_UINT32(env.pcon0, PIC18CPU),

        VMSTATE_UINT32(env.shadow_status, PIC18CPU),
        VMSTATE_UINT32(env.shadow_wreg, PIC18CPU),
        VMSTATE_UINT32(env.shadow_bsr, PIC18CPU),
        VMSTATE_UINT32_ARRAY(env.shadow_fsr, PIC18CPU, 3),
        VMSTATE_UINT32(env.shadow_prodl, PIC18CPU),
        VMSTATE_UINT32(env.shadow_prodh, PIC18CPU),
        VMSTATE_UINT32(env.shadow_pclath, PIC18CPU),
        VMSTATE_UINT32(env.shadow_pclatu, PIC18CPU),

        VMSTATE_UINT32(env.shadow_lo_status, PIC18CPU),
        VMSTATE_UINT32(env.shadow_lo_wreg, PIC18CPU),
        VMSTATE_UINT32(env.shadow_lo_bsr, PIC18CPU),
        VMSTATE_UINT32_ARRAY(env.shadow_lo_fsr, PIC18CPU, 3),
        VMSTATE_UINT32(env.shadow_lo_prodl, PIC18CPU),
        VMSTATE_UINT32(env.shadow_lo_prodh, PIC18CPU),
        VMSTATE_UINT32(env.shadow_lo_pclath, PIC18CPU),
        VMSTATE_UINT32(env.shadow_lo_pclatu, PIC18CPU),

        VMSTATE_UINT32(env.cshad_status, PIC18CPU),
        VMSTATE_UINT32(env.cshad_wreg, PIC18CPU),
        VMSTATE_UINT32(env.cshad_bsr, PIC18CPU),

        VMSTATE_UINT32(env.intreq, PIC18CPU),

        VMSTATE_END_OF_LIST()
    }
};
