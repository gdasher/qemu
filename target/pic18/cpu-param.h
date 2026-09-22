/*
 * PIC18 CPU parameters
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PIC18_CPU_PARAM_H
#define PIC18_CPU_PARAM_H

/*
 * Data memory is banked in 256-byte units and the core registers at
 * 0x4D6-0x4FF never take the softmmu path (they resolve to CPU state at
 * translation time or in a helper), so the page size only matters for the
 * RAM and MMIO regions. 1 KB matches pic16 and AVR.
 */
#define TARGET_PAGE_BITS 10

/*
 * Code space is 4 MB (flash, user IDs, configuration and EEPROM all reachable
 * through TBLPTR) and data space is 16 KB, placed at OFFSET_DATA above it.
 * 24 bits cover both windows (see OFFSET_CODE and OFFSET_DATA in cpu.h).
 */
#define TARGET_VIRT_ADDR_SPACE_BITS 24

#endif /* PIC18_CPU_PARAM_H */
