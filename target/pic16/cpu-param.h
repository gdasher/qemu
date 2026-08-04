/*
 * PIC16 CPU parameters
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PIC16_CPU_PARAM_H
#define PIC16_CPU_PARAM_H

/*
 * Data memory is banked in 128-byte units, but SFR accesses always take the
 * MMIO path, so page size only matters for the RAM regions. 1 KB matches AVR.
 */
#define TARGET_PAGE_BITS 10

/*
 * Code and data are separate spaces that both start at zero, so they are
 * given disjoint windows in the host address space (see OFFSET_CODE and
 * OFFSET_DATA in cpu.h). 17 bits would do; round up.
 */
#define TARGET_VIRT_ADDR_SPACE_BITS 24

#endif /* PIC16_CPU_PARAM_H */
