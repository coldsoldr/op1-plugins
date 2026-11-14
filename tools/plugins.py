"""The .opmod files this repository builds (tools/build.py reads this).

Each plugin is a coop plugin folder under plugins/ (its plugin.json says what to compile, as for coop's pack.py). The
file's id is coldsoldr.<name>: the ids te-op1-cfw's Opus ports of the older kennel builds used, so these (version 2)
replace those (1.0.0) on an OP-1 that has them: the core keeps the newer of two files with one id.

A pack is one file holding several plugins: one row in the mods list, one set of the effects' position buffers
(sized to its largest effect) instead of one set each.
"""
AUTHOR = 'coldsoldr'
VERSION = '2.2.5'
CORE = '2.31'                      # hosted effects 2.6, opus_called 2.9, opus_tempo 2.21, opus_page_now 2.31

EFFECTS = ['bitcrusher', 'drive', 'tape_drive', 'wavefolder', 'freq_shift', 'ensemble', 'tape_echo', 'cs_plate',
           'cs_hall', 'petri', 'fickle']
ENGINES = ['saw', 'rings']
PLUGINS = EFFECTS + ENGINES

# At most nine effects a file: on an OP-1 (Opus 2, 2026-11-14) the 10th and 11th hosted effects of one file got a wrong
# mark when chosen from the effect list and played as the plain filter (docs/porting.md). petri and fickle are singles.
PACK_MAX = 9
PACKS = {
    'cs_fx': [e for e in EFFECTS if e not in ('petri', 'fickle')],
}
assert all(len(m) <= PACK_MAX for m in PACKS.values())

# Names in Opus's own lists and the OS's (te-op1-cfw ports/opmod/plugins.py): a plugin with one is refused at build.
TAKEN_NAMES = {'bitcrush', 'chorus', 'plate', 'filter', 'overdrive', 'dimension', 'amp', 'dx', 'outatime', 'iter',
               'cwo', 'delay', 'grid', 'nitro', 'phone', 'punch', 'spring', 'cluster', 'digital', 'dna', 'dr wave',
               'fm', 'phase', 'pulse', 'sampler', 'string', 'voltage', 'pattern', 'endless', 'tombola', 'finger',
               'sketch', 'arpeggio'}
