/*
 * Value change dump of a few signals, on the virtual clock
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_VCD_TRACE_H
#define HW_CHIPS_VCD_TRACE_H

#include "qapi/error.h"

typedef struct VcdTrace VcdTrace;

/*
 * A trace writing to that file, covering the window that starts start_ns into
 * the run and lasts len_ns -- or the rest of the run, if len_ns is zero. The
 * file is opened here so a bad path is reported when the machine is built
 * rather than in the middle of a run.
 */
VcdTrace *vcd_trace_new(const char *path, int64_t start_ns, int64_t len_ns,
                        Error **errp);

/*
 * Declares a signal and returns the handle to drive it with. Every signal has
 * to be declared before the first vcd_trace_set(), because a VCD names them
 * all in its header.
 */
int vcd_trace_add(VcdTrace *t, const char *name, unsigned width);

/* Drives a signal. Values wider than the signal are truncated to it. */
void vcd_trace_set(VcdTrace *t, int id, uint64_t value);

/* Finishes the file. Safe on a NULL trace, and safe to call twice. */
void vcd_trace_close(VcdTrace *t);

#endif /* HW_CHIPS_VCD_TRACE_H */
