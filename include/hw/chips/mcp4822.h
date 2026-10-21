/*
 * Microchip MCP4822 dual 12-bit voltage-output DAC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_MCP4822_H
#define HW_CHIPS_MCP4822_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_MCP4822 "mcp4822"
OBJECT_DECLARE_SIMPLE_TYPE(MCP4822State, MCP4822)

/*
 * CS is the SSI chip select (SSI_GPIO_CS, active low). LDAC is a GPIO input
 * named MCP4822_LDAC_GPIO, active low; leave it unconnected and it reads high
 * (outputs then never update unless the board ties it low with the
 * "ldac-tied-low" property).
 */
#define MCP4822_LDAC_GPIO "ldac"

/* Command word: bit 15 channel (0 = A), bit 13 /GA, bit 12 /SHDN, 11..0 data. */
#define MCP4822_CMD_B         0x8000
#define MCP4822_CMD_NGA       0x2000   /* 1: gain 1x (2.048 V), 0: 2x (4.096 V) */
#define MCP4822_CMD_NSHDN     0x1000   /* 1: active, 0: channel shut down */
#define MCP4822_DATA_MASK     0x0FFF

/* Called whenever an output register changes (code, gain and shutdown per channel). */
typedef void (*MCP4822OutputSink)(void *opaque, const uint16_t code[2],
                                  const bool gain1x[2], const bool active[2]);

struct MCP4822State {
    SSIPeripheral parent_obj;

    bool ldac_tied_low;

    bool selected;
    uint8_t nbytes;          /* bytes clocked in this select */
    uint16_t word;

    uint16_t in_code[2];
    bool in_gain1x[2];
    bool in_active[2];
    bool in_valid[2];

    uint16_t out_code[2];
    bool out_gain1x[2];
    bool out_active[2];

    bool ldac_level;         /* pin level: high = idle */

    uint32_t frames;         /* valid 16-bit writes */
    uint32_t bad_frames;     /* selects that clocked other than 16 bits */
    uint32_t updates;        /* output register updates */

    MCP4822OutputSink sink;
    void *sink_opaque;
};

void mcp4822_set_output_sink(MCP4822State *s, MCP4822OutputSink sink, void *opaque);

/** Output voltage of a channel in microvolts (0 while shut down). */
uint32_t mcp4822_output_uv(MCP4822State *s, unsigned ch);

#endif /* HW_CHIPS_MCP4822_H */
