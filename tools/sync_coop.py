#!/usr/bin/env python3
"""Copy the coop plugins' sources and the coop SDK headers from a co-op checkout, unchanged.

    python tools/sync_coop.py ../co-op            the checkout's HEAD
    python tools/sync_coop.py ../co-op REV        a commit, tag or branch

Takes the tracked files only (git archive: no build output, no __pycache__) of firmware/plugins/<name> for every plugin
in tools/plugins.py and firmware/sdk/{coop_plugin.h, coop_screen.h, coop_fx_screen.h}, replaces plugins/<name> and
coop-sdk/ with them, and writes the commit into plugins/COOP_SOURCE. The sources are the coop ones byte for byte:
fix a plugin in co-op, then sync here.
"""
import io
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))
import plugins as table  # noqa: E402

SDK_FILES = ('coop_plugin.h', 'coop_screen.h', 'coop_fx_screen.h')


def git(coop, *args):
    return subprocess.run(['git', '-C', str(coop), *args], check=True, stdout=subprocess.PIPE).stdout


def extract(coop, rev, path, dest, strip):
    data = git(coop, 'archive', '--format=tar', rev, path)
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for m in tar.getmembers():
            parts = Path(m.name).parts[strip:]
            if not parts or not m.isfile():
                continue
            out = dest.joinpath(*parts)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(tar.extractfile(m).read())


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    coop = Path(sys.argv[1]).resolve()
    rev = git(coop, 'rev-parse', sys.argv[2] if len(sys.argv) == 3 else 'HEAD').decode().strip()
    for name in table.PLUGINS:
        dest = REPO / 'plugins' / name
        if dest.exists():
            shutil.rmtree(dest)
        extract(coop, rev, f'firmware/plugins/{name}', REPO / 'plugins', 2)
    sdk = REPO / 'coop-sdk'
    for f in SDK_FILES:
        (sdk / f).write_bytes(git(coop, 'show', f'{rev}:firmware/sdk/{f}'))
    (REPO / 'plugins' / 'COOP_SOURCE').write_text(
        f'coldsoldr/co-op {rev}\nfirmware/plugins/<name> and firmware/sdk, copied unchanged by tools/sync_coop.py\n',
        encoding='utf-8')
    print(f'synced {len(table.PLUGINS)} plugins and the coop SDK from co-op {rev[:7]}')


if __name__ == '__main__':
    main()
