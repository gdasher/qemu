/*
 * PIC18 firmware loading
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "boot.h"

bool pic18_load_firmware(const char *filename, MemoryRegion *program_mr)
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
     * carries the configuration bytes as well as the program.
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

/*
 * The HEX loader registers the file's contents as ROM blobs and copies them
 * in at the first system reset, so reading the configuration region through
 * the address space at board-init time finds the erased part (all ones). Ask
 * the loader for the blob itself, and fall back to memory only when the image
 * carried no configuration bytes at all.
 */
void pic18_load_config_bytes(PIC18CPU *cpu)
{
    unsigned i;

    for (i = 0; i < PIC18_NUM_CONFIG; i++) {
        hwaddr addr = OFFSET_CODE + PIC18_CONFIG_BASE + i;
        uint8_t byte;
        const uint8_t *blob = rom_ptr_for_as(&address_space_memory, addr, 1);

        if (blob) {
            byte = *blob;
        } else {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, &byte, 1);
        }
        cpu->env.config[i] = byte;
    }
}
