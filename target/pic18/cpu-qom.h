/*
 * PIC18 CPU QOM header (target agnostic)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_PIC18_CPU_QOM_H
#define TARGET_PIC18_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_PIC18_CPU "pic18-cpu"

OBJECT_DECLARE_CPU_TYPE(PIC18CPU, PIC18CPUClass, PIC18_CPU)

#define PIC18_CPU_TYPE_SUFFIX "-" TYPE_PIC18_CPU
#define PIC18_CPU_TYPE_NAME(name) (name PIC18_CPU_TYPE_SUFFIX)

#endif /* TARGET_PIC18_CPU_QOM_H */
