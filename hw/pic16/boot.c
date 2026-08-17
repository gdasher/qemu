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
#include "exec/cpu-common.h"
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

/*
 * The configuration words decide things the peripherals ask about before the
 * guest runs an instruction -- whether the watchdog is on, for one -- so they
 * are read once, here, at board init. The HEX loader does not write memory
 * then, though: it registers the file's contents as ROM blobs and copies them
 * in at the first system reset, so reading the config region through the
 * address space at this point finds the erased part (all ones: watchdog on,
 * at its longest period). Ask the loader for the blob itself, and fall back
 * to memory only when the image carried no configuration words at all.
 */
void pic16_load_config_words(PIC16CPU *cpu)
{
    unsigned i;

    for (i = 0; i < PIC16_NUM_CONFIG; i++) {
        hwaddr addr = OFFSET_CONFIG +
                      (PIC16_CONFIG_BASE - PIC16_PFM_BASE + i) * 2;
        uint8_t word[2];
        const uint8_t *blob = rom_ptr_for_as(&address_space_memory, addr,
                                             sizeof(word));

        if (blob) {
            memcpy(word, blob, sizeof(word));
        } else {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, word, sizeof(word));
        }
        cpu->env.config[i] = (word[0] | (word[1] << 8)) & 0x3FFF;
    }
}
