# op1-plugins

coldsoldr's plugins for custom OP-1 firmware (the original OP-1, firmware #246), built for **Opus 2** as `.opmod`
files from their **coop** sources.

| Plugin | Kind | What |
|---|---|---|
| bitcrusher | effect | sample-rate and bit-depth reduction |
| drive | effect | oversampled soft-clip overdrive |
| tape drive | effect | tape saturation, wow and flutter, age |
| wavefolder | effect | sine wavefolder, anti-aliased |
| freq shift | effect | frequency shifter and ring modulator |
| ensemble | effect | string-machine ensemble chorus |
| tape echo | effect | three-head tape echo, tempo sync |
| cs plate | effect | plate reverb with freeze |
| cs hall | effect | eight-line FDN hall reverb with freeze |
| saw | synth engine | two detuned oscillators and a sub |
| rings | synth engine | Mutable Instruments Rings, fixed point |

Each plugin's README (`plugins/<name>/README.md`) says what its encoders do. The `cs fx` pack holds all nine effects in
one file.

**Status: every file builds and reads back with the kit's checks, and the adapter's control logic passes its host test.
None of the files has run on an OP-1 yet**; see [docs/porting.md](docs/porting.md) for how to try them and what to
look for.

## What is here

| Folder | What |
|---|---|
| `plugin-kit/` | The Opus 2 plugin kit (SDK, docs, build tool), from [LizardMan245/opus](https://github.com/LizardMan245/opus/tree/main/plugin-kit) at `42c3692`, unmodified. GPL-3.0 with its own additional terms (its README). |
| `plugins/` | The coop plugins' sources, copied unchanged from [coldsoldr/co-op](https://github.com/coldsoldr/co-op) `firmware/plugins` (`plugins/COOP_SOURCE` names the commit). The `.coop` versions, not the older kennel `.knl` ones. |
| `coop-sdk/` | The coop plugin headers those sources include, from co-op `firmware/sdk`, unchanged. |
| `adapter/` | copa: the coop plugin interface implemented on Opus 2's core. |
| `tools/` | `build.py` (builds the `.opmod` files), `plugins.py` (what is built), `sync_coop.py` (copies the sources from a co-op checkout). |
| `tests/` | Host tests of the adapter. |

## Building

You need Python 3.8+ and the Blackfin toolchain (`bfin-elf-gcc` and `g++`); here it is the one co-op builds with, in WSL
at `~/op1-toolchain/bfin` (or set `OP1_BFIN_TOOLCHAIN` to the folder that holds its `bin/`).

```
python3 tools/build.py all         # every plugin and the pack -> build/*.opmod
python3 tools/build.py cs_hall     # one
make -C tests                      # the adapter's host tests
python plugin-kit/plugin_tool.py show build/cs_hall.opmod
```

To take newer plugin sources from co-op: `python tools/sync_coop.py ../co-op`, then build.

## Installing

Opus 2 on the OP-1, then in disk mode (shift + COM, then T3) copy the `.opmod` files into the disk's `mods` folder,
eject and restart. Use either the single effect files or `cs_fx.opmod`, not both: the second copy of an effect name is
refused. These replace the earlier `coldsoldr.*` 1.0.0 Opus files built from the kennel versions (same ids, version
2.0.0).

## Licence

GPL-3.0-or-later with the OP-1 firmware permission ([LICENSE](LICENSE) and [NOTICE](NOTICE)). Parts
carry their own notices: Mutable Instruments code (Émilie Gillet, MIT; `LICENSE-MIT` in each plugin that has it) and
the plugin kit (GPL-3.0, LizardMan, with its additional terms).
