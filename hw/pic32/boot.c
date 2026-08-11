/*
 * Loading a PIC32 firmware image
 *
 * XC32 releases an ELF, and unlike the PIC16 world that is the useful form:
 * the segments carry their own addresses, so nothing here has to be told which
 * part of the image is program flash and which is boot flash.
 *
 * What it does have to do is load only the flash. An XC32 image also describes
 * where its initialised data will end up in RAM, and where the interrupt
 * controller's vector offsets go, and neither of those is something a
 * programmer writes: the startup code copies both out of flash before main().
 * Loading them here would be loading the same bytes twice -- QEMU notices,
 * because the cached and uncached views of one RAM buffer overlap as physical
 * addresses -- and worse, it would hide a startup that had failed to do its
 * own copying behind data that was already right.
 *
 * Intel HEX and raw binaries are accepted too, for images released without an
 * ELF. Both describe flash only, so neither needs the filtering.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "elf.h"
#include "hw/core/loader.h"
#include "boot.h"

/*
 * Addresses in the image are virtual and all of them are in KSEG0 or KSEG1,
 * which the core's fixed-mapping MMU turns into physical ones by masking off
 * the top three bits.
 */
static uint32_t pic32_kseg_to_phys(uint32_t addr)
{
    return addr & 0x1FFFFFFF;
}

static bool pic32_is_flash(uint32_t phys, uint32_t size)
{
    return (phys >= PIC32_FLASH_PHYS &&
            phys + size <= PIC32_FLASH_PHYS + PIC32_FLASH_MAX) ||
           (phys >= PIC32_BOOT_PHYS &&
            phys + size <= PIC32_BOOT_PHYS + PIC32_BOOT_MAX);
}

/*
 * The segments the filtering exists for, which are skipped without comment:
 * initialised data, whose addresses are RAM -- everything below flash -- and
 * the interrupt vector offsets, which XC32 places at the addresses of the
 * OFFx registers themselves, in the SFR window. Anything else that is not
 * flash is a segment the machine has no way to take, and losing one silently
 * turns a linker script problem into an inexplicable crash somewhere after
 * boot.
 */
static bool pic32_is_startup_copied(uint32_t phys)
{
    return phys < PIC32_FLASH_PHYS ||
           (phys >= 0x1F800000 && phys < 0x1F900000);
}

/*
 * Loads the PT_LOAD segments that land in flash. Returns the number loaded, or
 * -1 if the file is not an ELF this machine could run, which is the caller's
 * cue to try the other formats.
 */
static int pic32_load_elf(const char *path)
{
    g_autofree char *image = NULL;
    gsize len = 0;
    Elf32_Ehdr ehdr;
    unsigned i;
    int loaded = 0;

    if (!g_file_get_contents(path, &image, &len, NULL) || len < sizeof(ehdr)) {
        return -1;
    }
    memcpy(&ehdr, image, sizeof(ehdr));

    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
        le16_to_cpu(ehdr.e_machine) != EM_MIPS) {
        return -1;
    }
    if (le16_to_cpu(ehdr.e_phentsize) != sizeof(Elf32_Phdr)) {
        return -1;
    }

    for (i = 0; i < le16_to_cpu(ehdr.e_phnum); i++) {
        uint64_t off = le32_to_cpu(ehdr.e_phoff) + i * sizeof(Elf32_Phdr);
        Elf32_Phdr phdr;
        uint32_t phys, filesz;

        if (off + sizeof(phdr) > len) {
            error_report("'%s' has a program header past the end of the file",
                         path);
            return -1;
        }
        memcpy(&phdr, image + off, sizeof(phdr));

        filesz = le32_to_cpu(phdr.p_filesz);
        if (le32_to_cpu(phdr.p_type) != PT_LOAD || filesz == 0) {
            continue;
        }
        if (le32_to_cpu(phdr.p_offset) + filesz > len) {
            error_report("'%s' has a segment past the end of the file", path);
            return -1;
        }

        phys = pic32_kseg_to_phys(le32_to_cpu(phdr.p_paddr));
        if (!pic32_is_flash(phys, filesz)) {
            if (!pic32_is_startup_copied(phys)) {
                warn_report("'%s' has a %u byte segment at 0x%08x, which is "
                            "not flash on this machine; it was not loaded",
                            path, filesz, phys);
            }
            continue;
        }

        rom_add_blob_fixed(path, image + le32_to_cpu(phdr.p_offset), filesz,
                           phys);
        loaded++;
    }

    if (!loaded) {
        error_report("'%s' has nothing in it that belongs in flash", path);
    }
    return loaded;
}

bool pic32_load_firmware(const char *path)
{
    uint64_t entry;
    int segments;
    ssize_t size;

    segments = pic32_load_elf(path);
    if (segments > 0) {
        return true;
    }
    if (segments == 0) {
        return false;
    }

    size = load_targphys_hex_as(path, &entry, NULL);
    if (size > 0) {
        return true;
    }

    /*
     * A raw image is boot flash: that is where the core starts, and an image
     * with no addresses in it cannot say otherwise.
     */
    size = load_image_targphys(path, PIC32_BOOT_PHYS, PIC32_BOOT_MAX, NULL);
    if (size > 0) {
        return true;
    }

    error_report("could not load '%s' as an ELF, Intel HEX or raw image",
                 path);
    return false;
}
