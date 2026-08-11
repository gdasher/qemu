/*
 * Value change dump of a few signals, on the virtual clock
 *
 * A logic analyser's view of a board: a handful of named signals and the
 * virtual times they changed, in the format waveform viewers already read.
 * The times are the virtual clock's, not the host's, because that is the one
 * the guest is paced against under -icount -- a trace stamped with host time
 * would show the emulator's own hesitations rather than the firmware's.
 *
 * A window keeps the file finite. The signals a board wants traced are the
 * ones that move fastest -- a bit-banged LED line changes twice a microsecond
 * for seconds on end -- so dumping a whole run means gigabytes of a waveform
 * nobody will scroll through. The trace instead carries the signal values it
 * was handed before the window opened, dumps them as the state at its start,
 * and closes the file the moment the window ends, which is also what makes a
 * trace survive the machine being killed rather than shut down.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/chips/vcd_trace.h"

#define VCD_MAX_SIGNALS 64

typedef struct {
    char *name;
    unsigned width;
    uint64_t value;
} VcdSignal;

struct VcdTrace {
    FILE *f;
    int64_t start_ns;
    int64_t end_ns;         /* 0 for "until the run ends" */
    bool open;              /* the window has started */
    bool done;              /* the file is finished */
    unsigned since_flush;
    VcdSignal sig[VCD_MAX_SIGNALS];
    unsigned n;
};

VcdTrace *vcd_trace_new(const char *path, int64_t start_ns, int64_t len_ns,
                        Error **errp)
{
    VcdTrace *t;
    FILE *f = fopen(path, "w");

    if (!f) {
        error_setg_errno(errp, errno, "cannot write the logic trace '%s'",
                         path);
        return NULL;
    }

    t = g_new0(VcdTrace, 1);
    t->f = f;
    t->start_ns = start_ns;
    t->end_ns = len_ns ? start_ns + len_ns : 0;
    return t;
}

int vcd_trace_add(VcdTrace *t, const char *name, unsigned width)
{
    VcdSignal *s;

    assert(!t->open);
    assert(t->n < VCD_MAX_SIGNALS);
    assert(width >= 1 && width <= 64);

    s = &t->sig[t->n];
    s->name = g_strdup(name);
    s->width = width;
    return t->n++;
}

/* The identifier a VCD refers to a signal by: one printable character. */
static char vcd_trace_id(int id)
{
    return '!' + id;
}

static void vcd_trace_value(VcdTrace *t, int id)
{
    const VcdSignal *s = &t->sig[id];
    unsigned bit = s->width;

    if (s->width == 1) {
        fprintf(t->f, "%u%c\n", (unsigned)(s->value & 1), vcd_trace_id(id));
        return;
    }
    fputc('b', t->f);
    while (bit--) {
        fputc('0' + ((s->value >> bit) & 1), t->f);
    }
    fprintf(t->f, " %c\n", vcd_trace_id(id));
}

/*
 * Writes the header and the state the signals were already in. Deferred to
 * the first change inside the window rather than done at realize, so that
 * what a trace opens with is the board as it stood then and not as it was
 * reset.
 */
static void vcd_trace_open(VcdTrace *t)
{
    unsigned i;

    fprintf(t->f, "$timescale 1ns $end\n$scope module board $end\n");
    for (i = 0; i < t->n; i++) {
        fprintf(t->f, "$var wire %u %c %s $end\n", t->sig[i].width,
                vcd_trace_id(i), t->sig[i].name);
    }
    fprintf(t->f, "$upscope $end\n$enddefinitions $end\n");

    fprintf(t->f, "#%" PRId64 "\n$dumpvars\n", t->start_ns);
    for (i = 0; i < t->n; i++) {
        vcd_trace_value(t, i);
    }
    fprintf(t->f, "$end\n");
    t->open = true;
}

void vcd_trace_set(VcdTrace *t, int id, uint64_t value)
{
    VcdSignal *s = &t->sig[id];
    int64_t now;

    if (t->done) {
        return;
    }
    if (s->width < 64) {
        value &= (1ULL << s->width) - 1;
    }
    if (value == s->value) {
        return;
    }
    s->value = value;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < t->start_ns) {
        return;                 /* remembered, for the window's first line */
    }
    if (t->end_ns && now > t->end_ns) {
        vcd_trace_close(t);
        return;
    }
    if (!t->open) {
        vcd_trace_open(t);
    }
    fprintf(t->f, "#%" PRId64 "\n", now);
    vcd_trace_value(t, id);

    /*
     * An unbounded trace is never closed -- the machine it belongs to runs
     * until it is killed -- so it has to reach the disk as it goes, or a run
     * that is stopped rather than finished leaves a truncated file.
     */
    if (!t->end_ns && ++t->since_flush >= 4096) {
        t->since_flush = 0;
        fflush(t->f);
    }
}

void vcd_trace_close(VcdTrace *t)
{
    unsigned i;

    if (!t || t->done) {
        return;
    }
    if (!t->open) {
        /* Nothing moved inside the window; say what it would have shown. */
        vcd_trace_open(t);
    }
    t->done = true;
    fclose(t->f);
    t->f = NULL;
    for (i = 0; i < t->n; i++) {
        g_free(t->sig[i].name);
    }
}
