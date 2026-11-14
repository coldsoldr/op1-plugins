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

/* ---- memory the audio reads every block, for a member built with COPA_OPUS (rings' voice states): the audio pool,
 * which the display does not share, else the plain pool; zeroed, 32-byte aligned. From its init (opl_init).
 * Opus loads a plugin's code and zero-filled data into whichever pool has room: on an OP-1 (2026-11-14, Opus's
 * figures: bank 2) rings' landed in the far pool, which costs about twice as much while the display sends a frame,
 * and rings overran the audio block until its voice states came from here. ---- */
void *copa_alloc_audio(uint32_t bytes)
{
    void *p = opus_alloc_audio(copa_self, bytes);
    return p ? p : opus_alloc(copa_self, bytes);
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

#define PATHS 16
const char *copa_svg_path(const copa_def *d, int index)
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

/* ---- DIAGNOSTIC (2.2.2, temporary): Opus's own figures for this plugin (opus_stats, api.md's layout): whether the
 * core switched its audio off and at what cost, its worst block, the blocks over its figure, the audio's late
 * blocks. Asked every 64 frames from the display thread. ---- */
struct st_head { unsigned magic; unsigned short version, head_size; unsigned size, seq, cyc_lo, cyc_hi, up_ms, block; };
struct st_sec { unsigned short tag, entry_size, count, pad; };
struct st_plugin { unsigned ram, base, image; short part; unsigned char state, bank; };
struct st_part {
    unsigned a_lo, a_hi;
    unsigned w[8];
    unsigned budget, worst, over, off_at;
    unsigned char most, off, unlimited, pad;
    short plugin;
    unsigned short name;
};
struct st_audio { unsigned b_lo, b_hi, blocks, late; };
static unsigned st_buf[4096];
static volatile int st_off = -1, st_part = -1;
static volatile unsigned st_off_at, st_worst, st_over, st_late, st_frames;

static void read_stats(void)
{
    if ((st_frames++ & 63) != 0)
        return;
    const int n = opus_stats(st_buf, sizeof st_buf);
    if (n <= 0)
        return;
    const unsigned char *rec = (const unsigned char *)st_buf;
    const struct st_head *h = (const struct st_head *)rec;
    if (h->magic != 0x5453504Fu)
        return;
    const unsigned me = (unsigned)(unsigned long)&opl_init;
    for (int pass = 0; pass < 2; ++pass) {         /* the plugins section names our part, then the parts section */
        unsigned at = (h->head_size + 7u) & ~7u;
        while (at + sizeof(struct st_sec) <= (unsigned)n) {
            const struct st_sec *sec = (const struct st_sec *)(rec + at);
            const unsigned body = at + sizeof *sec;
            for (unsigned i = 0; i < sec->count && body + (i + 1) * sec->entry_size <= (unsigned)n; ++i) {
                const unsigned char *e = rec + body + i * sec->entry_size;
                if (pass == 0 && sec->tag == 4 && sec->entry_size >= sizeof(struct st_plugin)) {
                    const struct st_plugin *pl = (const struct st_plugin *)e;
                    if (me >= pl->base && me < pl->base + pl->image)
                        st_part = pl->part;
                } else if (pass == 0 && sec->tag == 1 && sec->entry_size >= sizeof(struct st_audio)) {
                    st_late = ((const struct st_audio *)e)->late;
                } else if (pass == 1 && sec->tag == 3 && sec->entry_size >= sizeof(struct st_part) &&
                           (int)i == st_part) {
                    const struct st_part *pt = (const struct st_part *)e;
                    st_off = pt->off;
                    st_off_at = pt->off_at;
                    st_worst = pt->worst;
                    st_over = pt->over;
                }
            }
            at = (body + sec->count * sec->entry_size + 7u) & ~7u;
        }
    }
}

static int rows(int i, const char **label, char *value)
{
    if (copa_fx_row(i, label, value))
        return 1;
    int n = 0;
    while (copa_fx_row(n, label, value))
        ++n;
    if (i != n)
        return 0;
    *label = "core";
    fw_snprintf(value, 40, "off %d at %uk worst %uk over %u late %u", st_off, st_off_at / 1000, st_worst / 1000,
                st_over, st_late);
    return 1;
}

static void frame(unsigned short *buf)
{
    read_stats();
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
    opus_on_settings_rows(self, rows);             /* DIAGNOSTIC 2.2.2 */
    copa_input_start();
    return 0;
}
