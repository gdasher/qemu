/*
 * PIC32 peripheral pin select
 *
 * Storage, and deliberately nothing more. PPS decides which package pin a
 * peripheral's signal appears on, and in this machine the peripherals that
 * have remappable signals are reached through a bus rather than through pins:
 * a UART talks to a character device, an SPI controller to devices on an SSI
 * bus. So no routing here has anything to route.
 *
 * What the registers do earn is being visible. Firmware programs them early
 * -- this one writes nine before it touches anything else -- and a machine
 * that answered those writes from the unimplemented-region catch-all would
 * bury nine real log lines in the noise of a block it never reads back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pic32/pic32_pps.h"
#include "hw/pic32/pic32_regs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

static uint32_t pic32_pps_read(void *opaque, hwaddr addr)
{
    PIC32PpsState *s = opaque;

    return s->regs[addr / 4];
}

static void pic32_pps_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32PpsState *s = opaque;
    unsigned reg = addr / 4;

    /* Both the input and the output selects are five bits wide. */
    s->regs[reg] = value & 0x1F;
    if (s->out_notify && addr >= 0x200) {
        s->out_notify(s->out_opaque, reg, s->regs[reg]);
    }
}

void pic32_pps_set_out_notifier(PIC32PpsState *s,
                                void (*fn)(void *opaque, unsigned reg,
                                           unsigned sel),
                                void *opaque)
{
    s->out_notify = fn;
    s->out_opaque = opaque;
}

unsigned pic32_pps_out_get(PIC32PpsState *s, unsigned reg)
{
    return reg < PIC32_PPS_REGS ? s->regs[reg] : 0;
}

static const PIC32RegsOps pic32_pps_regs_ops = {
    .read = pic32_pps_read,
    .write = pic32_pps_write,
};

static void pic32_pps_reset_hold(Object *obj, ResetType type)
{
    PIC32PpsState *s = PIC32_PPS(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void pic32_pps_realize(DeviceState *dev, Error **errp)
{
    PIC32PpsState *s = PIC32_PPS(dev);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_pps_regs_ops, s,
                       "pic32-pps", PIC32_PPS_SIZE, PIC32_REGS_PLAIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription pic32_pps_vmstate = {
    .name = "pic32-pps",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PIC32PpsState, PIC32_PPS_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_pps_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_pps_realize;
    dc->vmsd = &pic32_pps_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic32_pps_reset_hold;
}

static const TypeInfo pic32_pps_types[] = {
    {
        .name = TYPE_PIC32_PPS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32PpsState),
        .class_init = pic32_pps_class_init,
    },
};

DEFINE_TYPES(pic32_pps_types)
