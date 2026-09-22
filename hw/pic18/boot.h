/*
 * PIC18 firmware loading
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC18_BOOT_H
#define HW_PIC18_BOOT_H

#include "hw/core/boards.h"
#include "target/pic18/cpu.h"

/**
 * pic18_load_firmware:
 * @filename: Intel HEX or raw binary image
 *
 * XC8 emits Intel HEX addressed in program-memory bytes, with the
 * configuration bytes at 0x300000, the user IDs at 0x200000 and the EEPROM
 * image at 0x380000 -- the same layout the SoC maps at OFFSET_CODE, so the
 * file loads in a single pass.
 *
 * Returns true on success, reporting the error itself on failure.
 */
bool pic18_load_firmware(const char *filename, MemoryRegion *program_mr);

/**
 * pic18_load_config_bytes:
 *
 * Copies CONFIG1..CONFIG10 out of the loaded image into the CPU, which needs
 * them before the guest runs (the watchdog asks about WDTE and WDTCPS).
 * Call after pic18_load_firmware(); unprogrammed bytes read as 0xFF.
 */
void pic18_load_config_bytes(PIC18CPU *cpu);

#endif /* HW_PIC18_BOOT_H */
