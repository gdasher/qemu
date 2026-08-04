/*
 * PIC16 CPU migration state
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
    cpu_set_status(opaque, qemu_get_byte(f));
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

const VMStateDescription vms_pic16_cpu = {
    .name = "cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.pc_w, PIC16CPU),
        VMSTATE_UINT32(env.wreg, PIC16CPU),
        VMSTATE_UINT32(env.bsr, PIC16CPU),
        VMSTATE_UINT32(env.pclath, PIC16CPU),
        VMSTATE_UINT32(env.intcon, PIC16CPU),
        VMSTATE_UINT32_ARRAY(env.fsr, PIC16CPU, 2),

        VMSTATE_SINGLE(env, PIC16CPU, 0, vms_status, CPUPIC16State),

        VMSTATE_UINT32_ARRAY(env.stack, PIC16CPU, PIC16_STACK_DEPTH),
        VMSTATE_UINT32(env.stkptr, PIC16CPU),

        VMSTATE_UINT32(env.shadow_wreg, PIC16CPU),
        VMSTATE_UINT32(env.shadow_status, PIC16CPU),
        VMSTATE_UINT32(env.shadow_bsr, PIC16CPU),
        VMSTATE_UINT32_ARRAY(env.shadow_fsr, PIC16CPU, 2),
        VMSTATE_UINT32(env.shadow_pclath, PIC16CPU),

        VMSTATE_END_OF_LIST()
    }
};
