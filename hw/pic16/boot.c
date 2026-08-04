/*
 * PIC16 firmware loading
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "boot.h"

bool pic16_load_firmware(const char *filename, MemoryRegion *program_mr)
{
    g_autofree char *path = NULL;
    ssize_t bytes_loaded;
    uint64_t entry = 0;

    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, filename);
    if (path == NULL) {
        error_report("unable to find firmware image '%s'", filename);
        return false;
    }

    /*
     * Try Intel HEX first: it is what every PIC toolchain emits, and it
     * carries the configuration words as well as the program.
     */
    bytes_loaded = load_targphys_hex_as(path, &entry, &address_space_memory);
    if (bytes_loaded >= 0) {
        return true;
    }

    bytes_loaded = load_image_mr(path, program_mr);
    if (bytes_loaded < 0) {
        error_report("unable to load '%s' as Intel HEX or raw binary",
                     filename);
        return false;
    }
    return true;
}
