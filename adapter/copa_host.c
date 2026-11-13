/* The start of an Opus plugin made from coop plugins, and the small services coop's core gave them: the L1 lease and
 * scratch (e4), the tempo (e10), the screens' SVG files.
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

struct opl_self *copa_self;

/* ---- the L1 lease (e4): from an engine's init only, up to 4096 bytes for all engines, zeroed; opus_fast's
 * memory (L1 data, given for good). 0 refused: the engine keeps its SDRAM fallback, as under coop. ---- */
#define L1_LEASE_MAX 4096u
int copa_l1_open;
static uint32_t l1_used;

void *coop_l1_lease(uint32_t bytes)
{
    bytes = (bytes + 31) & ~31u;
    if (!copa_l1_open || !bytes || l1_used + bytes > L1_LEASE_MAX)
        return 0;
    uint32_t *p = opus_fast(copa_self, bytes);
    if (!p)
        return 0;
    for (uint32_t i = 0; i < bytes / 4; ++i)
        p[i] = 0;
    l1_used += bytes;
    return p;
}

/* ---- the per-call scratch (e4): coop's 1312 bytes of L1 scratchpad, valid inside an engine's note_on and render.
 * The OS renders one voice at a time, so one block serves every engine of the file. ---- */
#define L1_SCRATCH 1312u
static void *l1_scratch;

int copa_l1_scratch_make(void)
{
    if (!l1_scratch)
        l1_scratch = opus_fast(copa_self, L1_SCRATCH);
    if (!l1_scratch)
        l1_scratch = opus_alloc_audio(copa_self, L1_SCRATCH);
    if (!l1_scratch)
        l1_scratch = opus_alloc(copa_self, L1_SCRATCH);
    return l1_scratch ? 0 : -1;
}

void *coop_l1_scratch(uint32_t *bytes)
{
    if (bytes)
        *bytes = l1_scratch ? L1_SCRATCH : 0;
    return l1_scratch;
}

/* ---- the tempo (e10): BPM as Q16, 120 << 16 outside 20..400 BPM or unknown. Any thread, the audio included. ---- */
uint32_t coop_tempo_bpm_q16(void)
{
    struct opus_tempo t = { sizeof t, 0, 0, 0, 0, 0, 0 };
    opus_tempo(&t);
    if (t.mbpm < 20000u || t.mbpm > 400000u)
        return 120u << 16;
    return t.mbpm * 8192u / 125u;  /* mbpm x 65536 / 1000, within 32 bits up to 524 BPM */
}

/* ---- the screens: the OS reads an SVG from a file, so the plugin carries its own and writes it into its folder when
 * missing or changed (a temporary file, then a rename: never half a screen at the next start). ---- */
static int same_file(const char *path, const char *data, unsigned len)
{
    static unsigned char buf[512] __attribute__((aligned(32)));
    const int fd = fw_yaffs_open(path, 0, 0);
    if (fd < 0)
        return 0;
    int same = fw_yaffs_lseek(fd, 0, 2) == (int)len && fw_yaffs_lseek(fd, 0, 0) == 0;
    for (unsigned at = 0; same && at < len;) {
        const unsigned n = len - at < sizeof buf ? len - at : sizeof buf;
        same = fw_yaffs_read(fd, buf, n) == (int)n;
        for (unsigned i = 0; same && i < n; ++i)
            same = buf[i] == (unsigned char)data[at + i];
        at += n;
    }
    fw_yaffs_close(fd);
    return same;
}

/* ---- what each member did, as rows on the plugin's settings page (the mods list, the blue push): whether it was
 * added, its screen file, and how often the core ran its screen's init and draw. The screens' check on the device:
 * a hosted effect's screen never showed on an Opus 2 OP-1 (2026-11-13), and these say which step stops. ---- */
#define ROWS 16
static volatile int add_result[ROWS];
static volatile signed char svg_state[ROWS];       /* 0 no screen, 1 its file is there, -1 not written */
static volatile unsigned scr_count[ROWS][COPA_COUNTS];   /* COPA_SCR_*, COPA_E_* */
static volatile int marks[ROWS];
static volatile int seen_kind = -1, seen_t, seen_where, seen_mark;   /* the last module page shown (the frame hook) */

void copa_screen_called(const copa_def *d, int what)
{
    const int i = d ? (int)(d - copa_defs) : -1;
    if (i >= 0 && i < ROWS && what >= 0 && what < COPA_COUNTS)
        ++scr_count[i][what];
}

void copa_mark_of(const copa_def *d, int mark)
{
    const int i = (int)(d - copa_defs);
    if (i >= 0 && i < ROWS)
        marks[i] = mark;
}

static int status_row(int i, const char **label, char *value)
{
    const int n = copa_def_count < ROWS ? copa_def_count : ROWS;
    if (i == n) {                  /* the last module page shown: its kind, T key, position and word 4 */
        *label = "page";
        fw_snprintf(value, 40, "k%d t%d w%d m%04x", seen_kind, seen_t, seen_where, seen_mark & 0xFFFF);
        return 1;
    }
    if (i < 0 || i > n)
        return 0;
    *label = copa_defs[i].name;
    volatile unsigned *c = scr_count[i];
    if (add_result[i] < 0)
        fw_snprintf(value, 40, "not added (%d)", add_result[i]);
    else if (copa_defs[i].kind == COPA_ENGINE)
        fw_snprintf(value, 40, "q%u n%u i%u f%u x%u d%u m%04x", c[COPA_E_QUEUED], c[COPA_E_MADE], c[COPA_E_INIT],
                    c[COPA_E_FLIP], c[COPA_E_DIFF], c[COPA_SCR_OWN], marks[i] & 0xFFFF);
    else
        fw_snprintf(value, 40, "svg %s i%u d%u o%u p%u m%04x",
                    svg_state[i] > 0 ? "ok" : svg_state[i] < 0 ? "failed" : "none", c[0], c[1], c[2], c[3],
                    marks[i] & 0xFFFF);
    return 1;
}

#define PATHS 16
static const char *svg_path(const copa_def *d, int index);
const char *copa_svg_path(const copa_def *d, int index)
{
    const char *p = svg_path(d, index);
    if (index >= 0 && index < ROWS)
        svg_state[index] = p ? 1 : -1;
    return p;
}

static const char *svg_path(const copa_def *d, int index)
{
    static char paths[PATHS][80];
    if (!d->svg_file || index < 0 || index >= PATHS)
        return 0;
    char *path = paths[index];
    char tmp[88];
    fw_snprintf(path, sizeof paths[0], "%s/%s", copa_self->folder, d->svg_file);
    if (same_file(path, d->svg, d->svg_len))
        return path;
    if (opus_flash_headroom() < (int)d->svg_len + 4096) {
        opus_log(copa_self, "no flash room for a screen");
        return 0;
    }
    fw_yaffs_mkdir(copa_self->folder, 0);
    fw_snprintf(tmp, sizeof tmp, "%s.tmp", path);
    const int fd = fw_yaffs_open(tmp, 2 | 0x40 | 0x200, 0x180);
    if (fd < 0)
        return 0;
    const int wrote = fw_yaffs_write(fd, d->svg, d->svg_len);
    fw_yaffs_close(fd);
    if (wrote != (int)d->svg_len) {
        fw_yaffs_unlink(tmp);
        return 0;
    }
    fw_yaffs_unlink(path);
    return opus_rename(tmp, path) == OPUS_RENAME_OK ? path : 0;
}

/* The display thread's frame: the effects' page count (the settings rows), then the controls' popup. */
static void frame(unsigned short *buf)
{
    struct opus_page pg;
    if (opus_page_now(&pg)) {
        seen_kind = pg.kind;
        seen_t = pg.t;
        seen_where = pg.where;
        seen_mark = pg.par ? pg.par[4] : pg.live[4];
    }
    copa_fx_frame();
    copa_frame(buf);
}

int opl_init(struct opl_self *self)
{
    copa_self = self;
    uint32_t scratch = 0;
    int engines = 0;
    for (int i = 0; i < copa_def_count; ++i) {
        if (copa_defs[i].kind == COPA_FX && copa_defs[i].scratch > scratch)
            scratch = copa_defs[i].scratch;
        engines |= copa_defs[i].kind == COPA_ENGINE;
    }
    if (scratch && copa_fx_buffers(scratch) < 0)
        opus_log(self, "no room for the effects' buffers");
    if (engines && copa_l1_scratch_make() < 0)
        opus_log(self, "no room for the engines' scratch");
    int added = 0;
    for (int i = 0; i < copa_def_count; ++i) {
        const copa_def *d = &copa_defs[i];
        const int r = d->kind == COPA_FX ? copa_fx_add(d, i) : copa_engine_add(d, i);
        if (i < ROWS)
            add_result[i] = r;
        if (r >= 0)
            ++added;
        else {
            char line[48];
            fw_snprintf(line, sizeof line, "%s not added (%d)", d->name, r);
            opus_log(self, line);
        }
    }
    if (!added)
        return -1;
    opus_on_frame(self, frame);
    opus_on_settings_rows(self, status_row);
    copa_input_start();
    return 0;
}
