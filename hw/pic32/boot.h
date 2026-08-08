/*
 * Loading a PIC32 firmware image
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_BOOT_H
#define HW_PIC32_BOOT_H

#include "qemu/units.h"

/*
 * The flash the image may write to, as physical addresses. These are the
 * largest the family has; a part with less flash simply has segments that fit
 * inside them, and the memory region behind rejects anything that does not.
 */
#define PIC32_FLASH_PHYS 0x1D000000
#define PIC32_FLASH_MAX  (2 * MiB)
#define PIC32_BOOT_PHYS  0x1FC00000
#define PIC32_BOOT_MAX   (20 * KiB)

bool pic32_load_firmware(const char *path);

#endif /* HW_PIC32_BOOT_H */
