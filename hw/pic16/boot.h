/*
 * PIC16 firmware loading
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_BOOT_H
#define HW_PIC16_BOOT_H

#include "hw/core/boards.h"
#include "target/pic16/cpu.h"

/**
 * pic16_load_firmware:
 * @filename: Intel HEX or raw binary image
 *
 * XC8 emits Intel HEX with program memory at byte address = word address * 2
 * and the configuration words above word 0x8000, which matches the region
 * layout set up by the machine, so the file loads in a single pass.
 *
 * Returns true on success, reporting the error itself on failure.
 */
bool pic16_load_firmware(const char *filename, MemoryRegion *program_mr);

/**
 * pic16_load_config_words:
 *
 * Copies the configuration words out of the loaded image into the CPU, which
 * needs them to know things like whether a stack fault forces a reset. Call
 * after pic16_load_firmware(); unprogrammed words read as all ones, which is
 * also the state when no image was supplied.
 */
void pic16_load_config_words(PIC16CPU *cpu);

#endif /* HW_PIC16_BOOT_H */
