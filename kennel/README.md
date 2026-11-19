# kennel: coop effects as hundekraft `.knl` files

fickle and petri for hundekraft 3.0.3's kennel core, built from the same unchanged coop sources as the `.opmod`
files (`plugins/<id>`). `cokn.h` / `cokn.cc` are the glue: the coop core's calls the effect makes, the audio
(int16 <-> Q31; synth and drum mono, master stereo), the screen's view, and how kennel's presses reach the coop words.

```
python3 tools/build_knl.py          # WSL; needs a te-op1-cfw checkout beside this repo (or $TE_OP1_CFW) for knl_pack.py
```

Kennel has no fn layer: a knob press is either page 2 (words 5..7) or a latch, so the controls differ from coop's:

| | Blue press | Green press | White press | Orange press | Shift + orange press |
|---|---|---|---|---|---|
| fickle | page: wet mode x routing (9 steps: reverb in/both/loop, delay ..., slip ...) | page: loop mode (13 steps) | latch: loop | latch: freeze | dub |
| petri | page: effect (11 steps) | page: filter (shift + turn fine) | page: space (shift + turn fine) | latch: hold | reverse |

The turns are coop's. Steps are a click each and cycle the way the other kennel ports' modes do (`StepOf`).
Shift + orange works because the plugin keeps its own latch bits: kennel's latch flag is only an edge, and the
plugin reads shift when it sees the edge.

Tested in op1emu with hundekraft 3.0.3 (2026-11-19). Both effects show in the T3 list and load, and their screens draw
and follow the presses: fickle's freeze, loop, dub and pages; petri's hold, reverse and effect steps. Both process
audio, with no faults in the emulator. Not yet run on an OP-1.
