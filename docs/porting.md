# coop plugins on Opus 2

The thirteen coldsoldr coop plugins (eleven effects, two synth engines) as Opus 2 `.opmod` files. Their sources are the
coop sources byte for byte (`plugins/`, from co-op `firmware/plugins`); one adapter (`adapter/`, "copa") gives them the
coop plugin interface on top of Opus's core, so a fix in co-op reaches these with `tools/sync_coop.py` and a build.

Ported from the `.coop` versions, not the kennel `.knl` ones: the coop versions have the usability work (declared
controls with fine steps, latches, the popup, the e9 screens). te-op1-cfw's earlier Opus files (`coldsoldr.*` 1.0.0)
were built from the kennel versions; these use the same ids at 2.0.0, so the core loads these instead.

**Status (2.1.0, 2026-11-14): played on an OP-1, Opus 2 build 7 dev 146.** Saw and rings play with their FN controls,
popups and screens; rings plays every model and polyphony, and model changes while notes ring, without a stall (the
owner heard no clicks). The effects load with their own screens; bitcrusher was checked in full. What the runs taught
is below ("What the OP-1 taught"). Not yet measured: the CPU figures.

The build keeps `build/<name>.build/plugin_0.elf` for the crash logs:
`python plugin-kit/tools/crashlog.py N.txt --plugin coldsoldr.NAME=build/NAME.build/plugin_0.elf`.

## How the coop interface maps

| coop core (co-op `firmware/core/sdram`) | here |
|---|---|
| an effect in the filter slot, `params[4] = 0x5300 + n` | `opus_effect_host` (core 2.6): the filter slot and a mark in word 4, as coop. The mark is the core's (`mark` 0): coop's numbers differ between OP-1s, and a sound finds a hosted effect by its name |
| `render(slot, int32 *l, int32 *r, params, 64)` | the hosted `render` / `render_stereo`: int16 widened to Q31 (`<< 16`), back with saturation, as `plugin_host.cc` does. At the synth and drum both channels start as the instrument's mono block (Opus's instrument bus is mono, coop's is stereo) and the result is their mean |
| `params[0..3]` in the filter type's ranges | the hosted page's words, kept inside R246's filter range row (`0x01DE5C50 + 5 x 0x30`) as coop's `fx_sanitize` |
| `params[5..7]` the controls' words, `params[8]` the latches (e6) | the same: words clamped per control, latches per position, cleared when the effect starts at a position |
| `init(slot)` at the first block after a change | the same check in the render (coop's `g_cur`); the hosted `init` forces it |
| position buffers, `coop_fx_scratch` (e8) | three buffers per file (synth, drum, master) from `opus_alloc_audio`, as big as the file's largest `scratch`. The pack shares one set among its nine effects, as coop's core does |
| effect screens with `coop_fx_view_t` (e9) | the hosted screen; the view filled from `opus_page_now` (the position, the live words with the LFO), the latches, the position buffer while the effect runs there |
| engines on iter's voice loop | `opus_engine_add`: 128 samples at 88.2 kHz, the same phase step, the OS's envelope, velocity and mix in both. `COOP_ENGINE_F_44K` doubled to 128 samples as coop does. The note's velocity is applied by the OS, so the engine gets full scale. `type` is the coop name, so a coop sound of the engine loads |
| the declared controls, the popup (e4, e6) | `copa_ctl.c`: coop's layers (fn = Opus's FN key), steps, detents, wrap, step, fine step and remainder, clicks on release with the layer kept, an undeclared layer taken; the popup top left in the encoder's colour, drawn from `opus_on_frame` |
| the screen calls (`coop_scr_*`) | `copa_screen.c`: the R246 functions coop's core calls, at the same addresses (`fw_svg_node_find`, `fw_screen_raster` where the kit names them) |
| `coop_l1_lease` (engines' init, 4096 bytes) | `opus_fast`, from `opl_init`, same limit, zeroed |
| `coop_l1_scratch` (1312 bytes) | one block of `opus_fast` (else the audio pool) for the file's engines |
| an engine's `note_on` / `note_off` in the audio thread | queued from Opus's key thread and made at the next render, one note on a block (below) |
| the declared engine controls | `opus_on_knob` and `opus_on_key_filter` on the engine's T1 (`copa_input.c`): an engine's own `turn` and `key` never see FN's layers |
| `coop_tempo_bpm_q16` (e10) | `opus_tempo` (core 2.21) |

A click on a steps control (freq shift's mode, tape echo's heads, saw's sub) writes the OS's live words and, at the
synth and drum, the sound's patch through `Patch::SetFxParams`, the R246 path coop's core uses; Opus's hosted `key` has
no `par` to write. At the master the clicked word plays at once and is saved with the next turn of a knob.

## Why hosted effects, not effect types

Opus's newer effect types (`opus_effect_register`, with `page_key` / `page_turn` since core 2.31) can also run in a
manager's second slot (Fx overdrive). They were not used because:
- the OS's effect list has 16 types, 8 of them the OS's: the starter pack's effects and nine of ours do not fit, and a
  hosted effect takes no type;
- the hosted model is coop's own (the filter slot, words in its ranges, the mark in word 4, an OS screen object), so the
  render, the controls and the screens need no translation.

The cost: these effects never appear in a manager's second slot.

## Files

| file | id | holds |
|---|---|---|
| `bitcrusher.opmod` ... `cs_hall.opmod` | `coldsoldr.<name>` | one effect each |
| `saw.opmod`, `rings.opmod` | `coldsoldr.saw`, `coldsoldr.rings` | one engine each |
| `cs_fx.opmod` | `coldsoldr.cs_fx` | the nine older effects: one row in the mods list, one set of position buffers (3 x 132 KB instead of 3 x 313 KB for the nine singles). petri and fickle are singles only (below: at most nine effects a file) |

All need core 2.31 (`opus_page_now`). `cpu` is `unmeasured` until measured (below). Rings' voice states (556 KB) come
from the audio pool at its init (`copa_alloc_audio`), not its zero fill.

## On the OP-1: what to try

1. Developer mode on (Opus's menu: synth + drum + tape + mixer, then T1), so a slow or crashing plugin is not left out.
2. Disk mode (shift + COM, then T3): copy `build/cs_fx.opmod` (or single effects), `saw.opmod` and `rings.opmod` into
   `mods`. Not the pack and a single effect together. Remove older `coldsoldr.*` 1.0.0 files or let the core refuse them
   ("an older copy"). Eject, restart.
3. The mods list (the menu, T2): each row says "loaded", or why not.
4. Effects, at the synth, the drum and the master (shift + T3 to choose):
   - the screen shows and its parts move with the four encoders;
   - FN + turn moves the declared control and the popup shows its name and value (FN + blue: the gain on bitcrusher,
     drive, tape drive, wavefolder, freq shift, ensemble, tape echo; cs hall / cs plate: FN + blue, green, white);
     shift + FN + turn moves it finely;
   - the clicks: orange freezes cs hall and cs plate, green holds tape echo, white cycles tape echo's heads, green
     switches freq shift's mode; the popup shows each;
   - switch effects at one position and back: no noise burst (the shared buffer is reset by each effect's init);
   - save the sound, load another and back: the controls' words come back.
5. Engines (shift + T1): saw and rings play chords; FN + blue turns saw's shape and rings' model, FN + green rings'
   polyphony, shift + FN + orange clicks saw's sub octave.
6. Tape echo with sync on (FN + green): the time follows the tempo.
7. CPU: the heaviest case (rings: sympathetic strings at polyphony 1, chords; cs hall on the master), then the
   plugin's row details (orange push):
   "audio: worst N". Put `cycles N` in `plugins/<name>/cpu.txt` and build again.

If the OP-1 crashes: the crash log is in the disk's `logs` folder; `python plugin-kit/tools/crashlog.py N.txt` reads
it. Holding Stop while switching on starts with no plugins.

## What the OP-1 taught (2026-11-13 to 14)

Each found on the device, then fixed; the version that fixed it in brackets.

- **`opus_called()` is 0 in a hosted effect's `screen_init`** at the start, and in other calls of a hosted effect or
  engine: each record has screen entries of its own, and the plugin is found by its mark in word 4 (2.0.1).
- **An engine's own `turn` and `key` never see FN's layers** (FN + turn went to the OS and moved knob 3). The engines'
  declared controls go through `opus_on_knob` and `opus_on_key_filter` on their T1; the effects' hosted `turn` and `key`
  do see FN and keep it (2.0.2).
- **Opus calls an engine's `note_on` and `release` in the key thread**, which the audio interrupt cuts into; rings swaps
  and copies voice states in its note on, and corrupted the voices it was playing. They are queued and made at the
  next render, in order (2.0.4).
- **A block that runs over 580,525 cycles by itself switches the plugin off at once.** One note on a block (a chord's
  start spreads over up to 8.7 ms; not heard), and a valve: past 400,000 cycles of the file's engines in a block, the
  block's other voices are silent (2.0.8 to 2.0.10).
- **Opus loads a plugin's code and zero fill into whichever pool has room**; rings' landed in the far pool (Opus's
  figures: bank 2), which costs about twice as much while the display sends a frame. Rings 0.9 takes its voice states
  from `copa_alloc_audio` when built with `COPA_OPUS` (this build defines it for every member); this was the fix that
  ended the stalls (2.0.14).
- Rings itself (co-op, synced): a model or polyphony change cuts the voices struck with the old one (0.6); a chord's
  steals go silent without a fade or a copy, one shorter fade at a time (0.7); a string's delay line is cleared as the
  string reaches it, not 64 KB at a strike (0.8, bit-identical in the host test).
- **No effect click acted** (2.2.1; reported on petri and fickle, true of every effect with a click: tape echo's hold and
  heads, the reverbs' freeze, freq shift's mode). Opus runs every key filter before a hosted effect's `key`, and the
  engines' filter (`copa_input.c`, started in every file) handled every encoder release: on an effect page it consumed
  the press the hosted key had recorded and took the release. The filter now acts on a release only when it took the
  press; `tests/input_test.c` runs the filter beside the hosted path (it fails on 2.2.0's adapter).
- **At most nine hosted effects a file** (2.2.5). With petri and fickle in `cs_fx.opmod` as its 10th and 11th effects,
  choosing either from the effect list gave the sound a mark that is none of the file's (`3e9b` every time) and it
  played and showed the plain filter; with the two moved to the front of the pack they played. A settings-page readout
  (2.2.2-2.2.4: each effect's mark, the page's words as heard and stored, Opus's switch-off figures) showed the mark,
  and that Opus never switched the plugin off. The pack is back to the nine it had (`tools/plugins.py` `PACK_MAX`
  checks it); petri and fickle ship as single files, which play.
- **Effects chosen under the single files are another mark under `cs_fx.opmod`** (and back): such a sound shows the
  filter until the effect is chosen again.

## Open points

- The CPU figures: rings' heaviest case is sympathetic strings at polyphony 1 (8 strings a voice); its code still lies
  in whichever pool the core chose.
- Stereo at the synth and drum is mono in, mono out (Opus's instrument bus): cs hall's width and the stereo effects'
  spread are heard on the master only.
- The OS's popups draw under the control popup (it is drawn from `opus_on_frame`).
