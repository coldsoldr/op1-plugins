#!/usr/bin/env python3
"""Build the coop effects that have a kennel glue (kennel/<id>/) as hundekraft .knl files: build/knl/<id>.knl.

    python3 tools/build_knl.py            # every one (fickle, petri)
    python3 tools/build_knl.py petri      # one

Run in WSL (the Blackfin toolchain: $BFIN_BIN or ~/op1-toolchain/bfin/bin). The packer is te-op1-cfw's
tools/knl_pack.py (it drives hundekraft's own pack.py and checks the imports against the 3.0.3 core's exports), from
a te-op1-cfw checkout beside this repo or $TE_OP1_CFW. The sources are the synced coop ones (plugins/<id>), unchanged;
kennel/cokn.h and cokn.cc are the glue (what kennel's presses do, the audio and the screen).
"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
KENNEL = REPO / 'kennel'
OUT = REPO / 'build' / 'knl'


def main(argv):
    cfw = Path(os.environ.get('TE_OP1_CFW') or REPO.parent / 'te-op1-cfw')
    pack = cfw / 'tools' / 'knl_pack.py'
    if not pack.exists():
        sys.exit(f'error: {pack} not found (set $TE_OP1_CFW to a te-op1-cfw checkout)')
    ids = argv or sorted(p.name for p in KENNEL.iterdir() if (p / 'plugin.json').exists())
    for pid in ids:
        if not (KENNEL / pid / 'plugin.json').exists():
            sys.exit(f'error: no kennel glue for {pid} (kennel/{pid}/plugin.json)')
        r = subprocess.run([sys.executable, str(pack), str(KENNEL / pid), '-o', str(OUT / f'{pid}.knl')])
        if r.returncode:
            sys.exit(r.returncode)


if __name__ == '__main__':
    main(sys.argv[1:])
