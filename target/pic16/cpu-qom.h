/*
 * PIC16 CPU QOM header (target agnostic)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_PIC16_CPU_QOM_H
#define TARGET_PIC16_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_PIC16_CPU "pic16-cpu"

OBJECT_DECLARE_CPU_TYPE(PIC16CPU, PIC16CPUClass, PIC16_CPU)

#define PIC16_CPU_TYPE_SUFFIX "-" TYPE_PIC16_CPU
#define PIC16_CPU_TYPE_NAME(name) (name PIC16_CPU_TYPE_SUFFIX)

#endif /* TARGET_PIC16_CPU_QOM_H */
