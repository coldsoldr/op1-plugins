#!/usr/bin/env python3
"""Build coop plugins (plugins/) as Opus 2 .opmod files (docs/porting.md).

    python3 tools/build.py NAME ...        a plugin or a pack of tools/plugins.py -> build/NAME.opmod
    python3 tools/build.py all             every plugin and pack
    python3 tools/build.py --list          the names

The Blackfin toolchain: $OP1_BFIN_TOOLCHAIN (the folder holding bin/bfin-elf-gcc), else ~/op1-toolchain/bfin, else
what the kit finds (plugin-kit/tools/op1mod/build.py toolchain_places). Run it where that toolchain runs (WSL here).

Why a driver of our own, not `plugin_tool.py build`: the kit's build compiles C and assembly at a folder's top level;
coop plugins are C++ with sources under src/, a plugin.json naming them, and coop's compile flags. So this compiles
each plugin as coop's pack.py does (co-op firmware/tools/plugin/pack.py: the same flags, -DCOOP_FX_SCRATCH_BYTES),
links it into one relocatable object keeping only its descriptor global (a pack holds several plugins that reuse
names), links those with the adapter (adapter/) and generated table (copa_defs.c), and hands the one object to the
kit's own import stubs, two-base link and file writer, the same steps as the kit's plugin.build. So the file format and
its checks stay the kit's.

Refuses: a static initialiser (the core runs no .init_array), an import the core has no name for (only opus_* and
fw_*), a name in TAKEN_NAMES, a file over the kit's limits.
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
KIT = REPO / 'plugin-kit'
sys.path.insert(0, str(REPO / 'tools'))
sys.path.insert(0, str(KIT / 'tools' / 'op1mod'))
import plugin as kit  # noqa: E402
import opmod as kit_opmod  # noqa: E402
import plugins as table  # noqa: E402

ADAPTER = REPO / 'adapter'
COOP_SDK = REPO / 'coop-sdk'
KIT_SDK = KIT / 'sdk'
PLUGINS = REPO / 'plugins'
MERGE_LD = REPO / 'tools' / 'merge.ld'
OUT = REPO / 'build'

# coop's flags for a plugin (co-op firmware/Makefile BFIN_CFLAGS, pack.py), with sections per function for --gc-sections
BASE = ['-O2', '-mcpu=bf524', '-ffreestanding', '-fno-builtin', '-nostdlib', '-fno-common', '-ffunction-sections',
        '-fdata-sections', '-Wall']
CXX = ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit']
# the adapter: the kit's own flags (plugin-kit/tools/op1mod/build.py CFLAGS), warnings as errors
ADAPTER_CFLAGS = ['-mcpu=bf524-any', '-O2', '-fno-indirect-inlining', '-ffreestanding', '-fno-builtin', '-fno-common',
                  '-ffunction-sections', '-fdata-sections', '-fno-tree-loop-distribute-patterns', '-std=gnu11',
                  '-Wall', '-Wextra', '-Werror']
SCRATCH_MAX = 180224               # COOP_FX_SCRATCH_MAX
SVG_MAX = 128 * 1024


class Error(Exception):
    pass


def tc_dir():
    env = os.environ.get('OP1_BFIN_TOOLCHAIN')
    if env:
        return env
    home = Path.home() / 'op1-toolchain' / 'bfin'
    if (home / 'bin' / 'bfin-elf-gcc').exists():
        return str(home)
    import build as kit_build
    return kit_build.toolchain()


def tool(name):
    return kit.tool(tc_dir(), name)


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    if r.returncode:
        raise Error(' '.join(str(c) for c in cmd) + '\n' + r.stdout)
    return r.stdout


def libgcc():
    return run([tool('gcc'), '-mcpu=bf524', '-print-libgcc-file-name']).strip()


def display(name):
    shown = name.replace('_', ' ')
    if shown in table.TAKEN_NAMES:
        raise Error(f'{name}: the name {shown!r} is taken by Opus or the OS')
    if len(shown) > 15:
        raise Error(f'{name}: the name {shown!r} is longer than 15 characters')
    return shown


ACTIONS = {'click': 'COPA_CLICK', 'shift_click': 'COPA_SHIFT_CLICK', 'fn_turn': 'COPA_FN_TURN',
           'fn_click': 'COPA_FN_CLICK', 'shift_fn_turn': 'COPA_SHIFT_FN_TURN', 'shift_fn_click': 'COPA_SHIFT_FN_CLICK'}
ENCODERS = ['blue', 'green', 'white', 'orange']


class Member:
    """One coop plugin of a file."""

    def __init__(self, name, index, work):
        self.name, self.index = name, index
        self.dir = PLUGINS / name
        self.meta = json.loads((self.dir / 'plugin.json').read_text(encoding='utf-8'))
        if self.meta.get('name') != name:
            raise Error(f'{name}: plugin.json names {self.meta.get("name")!r}')
        self.kind = {'fx': 'COPA_FX', 'synth': 'COPA_ENGINE'}.get(self.meta['kind'])
        if not self.kind:
            raise Error(f'{name}: kind {self.meta["kind"]!r} has no Opus form here')
        self.work = Path(work) / name
        self.work.mkdir(parents=True, exist_ok=True)
        self.prefix = f'copa{index}_'
        self.scratch = self.meta.get('scratch')
        if self.scratch is not None and not (type(self.scratch) is int and 0 < self.scratch <= SCRATCH_MAX):
            raise Error(f'{name}: scratch must be 1..{SCRATCH_MAX}')
        self.controls = self.meta.get('controls', [])

    def compile(self):
        m = self.meta
        includes = ['-I' + str(self.dir)] + ['-I' + str((self.dir / d).resolve()) for d in m.get('include_dirs', [])]
        includes.append('-I' + str(COOP_SDK))
        defs = [f'-DCOOP_FX_SCRATCH_BYTES={self.scratch}'] if self.scratch else []
        objs = []
        for i, src in enumerate(m['sources']):
            path = (self.dir / src).resolve()
            if not path.exists():
                raise Error('source not found: ' + str(path))
            o = self.work / f'{i:02d}_{path.stem}.o'
            if path.suffix in ('.cc', '.cpp'):
                cmd = [tool('g++'), *BASE, *CXX]
            elif path.suffix in ('.c', '.S', '.s'):
                cmd = [tool('gcc'), *BASE]
            else:
                raise Error('unknown source type: ' + src)
            out = run(cmd + defs + m.get('cflags', []) + includes + ['-c', str(path), '-o', str(o)])
            if out.strip():
                print(out.rstrip())
            objs.append(str(o))
        return objs

    def link(self):
        """One relocatable object; only the descriptor stays global, renamed for this file."""
        objs = self.compile()
        ro = self.work / 'member.ro'
        sym = self.meta['symbol']
        run([tool('ld'), '-r', '--gc-sections', '--force-group-allocation', '-u', '_' + sym, '-T', MERGE_LD, *objs,
             libgcc(), '-o', ro])
        syms = run([tool('nm'), ro])
        bad = sorted({l.split()[-1] for l in syms.splitlines()
                      if '_GLOBAL__sub_I' in l or '__cxa_guard' in l or '.init_array' in l})
        if bad:
            raise Error(f'{self.name}: a static initialiser or a guarded static: ' + ', '.join(bad))
        heads = run([tool('objdump'), '-h', ro])
        if '.init_array' in heads or '.ctors' in heads:
            raise Error(f'{self.name}: static constructors (.init_array)')
        keep = self.work / 'keep.txt'
        keep.write_text('_' + sym + '\n')
        run([tool('objcopy'), f'--keep-global-symbols={keep}', ro])
        run([tool('objcopy'), '--redefine-sym', f'_{sym}=_{self.prefix}{sym}', ro])
        return str(ro)

    def svg(self):
        screen = self.meta.get('screen')
        if not isinstance(screen, dict):
            return None
        data = (self.dir / screen['svg']).read_bytes().replace(b'\r\n', b'\n')
        if len(data) > SVG_MAX:
            raise Error(f'{self.name}: its SVG is {len(data)} bytes, over {SVG_MAX}')
        return f'{self.name}.svg', data


def c_str(data):
    out, line = [], ''
    for b in data:
        line += f'\\x{b:02x}'
        if len(line) >= 112:
            out.append(f'    "{line}"')
            line = ''
    out.append(f'    "{line}"')
    return '\n'.join(out)


def c_text(s):
    if not re.fullmatch(r'[ -~]*', s) or '"' in s or '\\' in s:
        raise Error(f'not plain text: {s!r}')
    return f'"{s}"'


def controls_c(m):
    """The member's copa_ctl table, checked as coop's pack.py checks a declaration."""
    lines, rows = [], []
    for k, c in enumerate(m.controls):
        where = f'{m.name}: control {c.get("id", k)}'
        action = ACTIONS.get(c.get('action'))
        if not action:
            raise Error(f'{where}: action {c.get("action")!r}')
        if c.get('encoder') not in ENCODERS:
            raise Error(f'{where}: encoder {c.get("encoder")!r}')
        kind = {'steps': 's', 'continuous': 'c', 'latch': 'l'}.get(c.get('kind'))
        if not kind:
            raise Error(f'{where}: kind {c.get("kind")!r}')
        steps, count = '0', 0
        if kind == 'l':
            if m.kind != 'COPA_FX' or 'click' not in c['action'] or not 0 <= c.get('bit', -1) <= 15:
                raise Error(f'{where}: a latch is an effect\'s click with a bit 0..15')
            word = c['bit']
        else:
            word = c.get('word')
            if word not in (5, 6, 7):
                raise Error(f'{where}: word must be 5, 6 or 7')
        if kind == 's':
            names = c.get('steps', [])
            count = len(names)
            if not 2 <= count <= 16:
                raise Error(f'{where}: 2..16 steps')
            steps = f'ctl{m.index}_{k}_steps'
            lines.append(f'static const char *const {steps}[] = {{ {", ".join(c_text(s) for s in names)} }};')
        default = int(c.get('default', 0))
        if kind == 's' and not 0 <= default < count or kind == 'c' and not 0 <= default <= 32767:
            raise Error(f'{where}: default {default}')
        if kind == 'c' and 'turn' not in c['action']:
            raise Error(f'{where}: a continuous control is a turn')
        detents = int(c.get('detents', 1))
        step, fine_step = int(c.get('step', 0)), int(c.get('fine_step', 0))
        rows.append(f'    {{ .name = {c_text(c.get("name", ""))}, .steps = {steps}, .def = {default}, '
                    f'.step = {step}, .fine_step = {fine_step}, .action = {action}, '
                    f'.enc = {ENCODERS.index(c["encoder"])}, .word = {word}, .kind = \'{kind}\', .count = {count}, '
                    f'.detents = {detents}, .wrap = {1 if c.get("wrap") else 0}, .fine = {1 if c.get("fine") else 0} }},')
    if not rows:
        return lines, '0'
    name = f'ctl{m.index}'
    lines += [f'static const copa_ctl {name}[] = {{'] + rows + ['};']
    return lines, name


def defs_c(members):
    lines = ['/* Generated by tools/build.py from the plugins\' plugin.json: do not edit. */', '#include "copa.h"', '']
    rows = []
    for m in members:
        desc = m.prefix + m.meta['symbol']
        typ = 'coop_fx_t' if m.kind == 'COPA_FX' else 'coop_engine_t'
        lines.append(f'extern const {typ} {desc};')
        svg = m.svg()
        svg_file, svg_name, svg_len = '0', '0', 0
        if svg:
            svg_name, svg_len = f'svg{m.index}', len(svg[1])
            svg_file = f'"{svg[0]}"'
            lines.append(f'static const char {svg_name}[] =\n{c_str(svg[1])};')
        ctl_lines, ctl = controls_c(m)
        lines += ctl_lines
        d = (list(m.meta.get('defaults') or [0, 0, 0, 0]) + [0] * 4)[:4]
        rows.append(f'    {{ .kind = {m.kind}, .name = "{display(m.name)}", .desc = &{desc}, .svg_file = {svg_file}, '
                    f'.svg = {svg_name}, .svg_len = {svg_len}, .defaults = {{ {", ".join(map(str, d))} }}, '
                    f'.scratch = {m.scratch or 0}, .ctl = {ctl}, .nctl = {len(m.controls)} }},')
    lines += ['', 'const copa_def copa_defs[] = {'] + rows + ['};', f'const int copa_def_count = {len(rows)};', '']
    return '\n'.join(lines) + '\n'


def uses_lines(members):
    """The controls the file takes, declared for the core's clash check. All are on the plugin's own page (modal)."""
    out, seen = [], set()
    single = len(members) == 1

    def add(key, line):
        if key not in seen:
            seen.add(key)
            out.append(line)
    for m in members:
        screen = 'synth t1 ' if m.kind == 'COPA_ENGINE' else ''
        for c in m.controls:
            enc = c['encoder']
            what = c.get('name', 'a control') if single else ('fn: a control' if 'turn' in c['action'] else
                                                               'a click control')
            if 'turn' in c['action']:
                add(('turn', enc, screen), f'uses      {enc}_turn fn {screen}modal "{what[:27]}"')
            else:
                mods = {'click': '', 'shift_click': 'shift ', 'fn_click': 'fn ', 'shift_fn_click': 'shift fn '}
                add(('push', enc, screen, c['action']),
                    f'uses      {enc}_push {mods[c["action"]]}{screen}modal "{what[:27]}"')
    return out


def manifest(pid, members):
    kinds = {m.kind for m in members}
    desc = 'effects' if kinds == {'COPA_FX'} else 'engines' if kinds == {'COPA_ENGINE'} else 'effects and engines'
    out = [f'# coldsoldr.{pid}: coldsoldr\'s coop {desc} on Opus 2 (built by tools/build.py: do not edit)',
           'opmod     1', f'id        coldsoldr.{pid}', f'name      {pid.replace("_", " ")}',
           f'author    {table.AUTHOR}', f'version   {table.VERSION}', f'core      {table.CORE}']
    for m in members:
        out.append(f'adds      {"effect" if m.kind == "COPA_FX" else "engine"} {display(m.name)}')
    out += uses_lines(members)
    if len(out) > 40:
        raise Error(f'{pid}: manifest too long')
    return '\n'.join(out) + '\n'


def build(pid, names):
    if not re.fullmatch(r'[a-z0-9_]+', pid):
        raise Error('bad name ' + pid)
    OUT.mkdir(parents=True, exist_ok=True)
    work = OUT / f'{pid}.build'                # kept, as the kit's build keeps it: crashlog.py reads plugin_0.elf
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    if True:
        tmp = str(work)
        members = [Member(n, i, tmp) for i, n in enumerate(names)]
        names_seen = [display(m.name) for m in members]
        if len(set(names_seen)) != len(names_seen):
            raise Error(f'{pid}: two plugins with one name')
        objs = [m.link() for m in members]
        defs = Path(tmp) / 'copa_defs.c'
        defs.write_text(defs_c(members))
        kinds = {m.kind for m in members}
        sources = [defs, ADAPTER / 'copa_host.c', ADAPTER / 'copa_ctl.c', ADAPTER / 'copa_screen.c',
                   ADAPTER / 'copa_input.c']
        if 'COPA_FX' in kinds:
            sources.append(ADAPTER / 'copa_fx.c')
        else:
            sources.append(ADAPTER / 'copa_nofx.c')
        if 'COPA_ENGINE' in kinds:
            sources.append(ADAPTER / 'copa_engine.c')
        else:
            sources.append(ADAPTER / 'copa_noengine.c')
        includes = ['-I' + str(ADAPTER), '-I' + str(KIT_SDK), '-I' + str(COOP_SDK)]
        for s in sources:
            o = Path(tmp) / (s.stem + '.o')
            out = run([tool('gcc'), *ADAPTER_CFLAGS, *includes, '-c', str(s), '-o', str(o)])
            if out.strip():
                print(out.rstrip())
            objs.append(str(o))
        whole = Path(tmp) / 'plugin.ro'
        run([tool('ld'), '-r', '--gc-sections', '--force-group-allocation', '-u', '_opl_init', '-T', MERGE_LD, *objs,
             libgcc(), '-o', whole])
        return write_file(pid, members, str(whole), tmp)


def write_file(pid, members, ro, tmp):
    """The kit's own steps (plugin-kit/tools/op1mod/plugin.py build) on one relocatable object."""
    tc = tc_dir()
    text = manifest(pid, members)
    man = kit.read_manifest(text)
    objs = [ro]
    stubbed = kit.called_imports(tc, objs)
    if stubbed:
        stubs = os.path.join(tmp, '_stubs.s')
        kit.write_stubs(stubbed, stubs)
        kit._run([kit.tool(tc, 'as'), '-mcpu=bf524', '-o', stubs + '.o', stubs])
        objs.append(stubs + '.o')
    imports, relocs, summary = kit_opmod.link_two_bases(tc, objs, tmp, 'plugin')
    strip = lambda n: n[len(kit.STUB_PREFIX):] if n and n.startswith(kit.STUB_PREFIX) else n  # noqa: E731
    imports = [strip(n) for n in imports]
    relocs = [(s, k, strip(n), v) for s, k, n, v in relocs]
    bad = sorted(n for n in imports if not re.match(r'(opus_|fw_)', n))
    if bad:
        raise Error(f'{pid}: imports the core has no name for: ' + ', '.join(bad))
    elf0 = os.path.join(tmp, 'plugin_0.elf')
    base, image, zero, _ = kit_opmod.relocations(elf0)
    if len(image) > kit.MAX_IMAGE or zero > kit.MAX_IMAGE:
        raise Error(f'{pid}: image {len(image)} bytes, zero fill {zero}: over {kit.MAX_IMAGE}')
    entry = dict(kit.exports_of(tc, elf0, base, ['opl_init']))['opl_init']
    deep = sorted(n for n in imports if n.startswith('fw_'))
    built = [f'deep      {" ".join(deep) if deep else "none"}', f'ram       image {len(image)} zero {zero}',
             f'imports   {len(imports)}', f'cpu       {cpu(members)}']
    out_text = (text.rstrip('\n') + kit.AUTHOR_END + '\n'.join(built) + '\n').encode('utf-8')
    data = kit.write_opmod(out_text, image, zero, entry, imports, relocs, apart=True)
    kit.read_opmod(data)                                       # the core loader's checks, in Python
    path = OUT / (pid + '.opmod')
    path.write_bytes(data)
    print(f'{path.name}: {len(data)} bytes, image {len(image)}, zero {zero}; {man["id"]} {man["version"]}; '
          f'imports {len(imports)}')
    return {'id': man['id'], 'bytes': len(data), 'image': len(image), 'zero': zero, 'imports': imports}


def cpu(members):
    """The figure from cpu.txt beside a single plugin's plugin.json (measured on an OP-1 under Opus), else unmeasured."""
    if len(members) == 1:
        n = kit.read_cpu(str(members[0].dir))
        if n:
            return str(n)
    return 'unmeasured'


def targets(names):
    if names == ['all']:
        names = list(table.PLUGINS) + list(table.PACKS)
    for n in names:
        if n in table.PACKS:
            yield n, table.PACKS[n]
        elif n in table.PLUGINS:
            yield n, [n]
        else:
            raise Error('no such plugin or pack: ' + n)


def main():
    names = sys.argv[1:]
    if not names or names == ['--list']:
        print('plugins:', ' '.join(table.PLUGINS))
        print('packs:', ' '.join(table.PACKS))
        return
    failed = []
    for pid, members in targets(names):
        try:
            build(pid, members)
        except (Error, kit.PluginError, kit_opmod.OpmodError) as e:
            print(f'FAILED {pid}: {e}')
            failed.append(pid)
    if failed:
        sys.exit('failed: ' + ' '.join(failed))


if __name__ == '__main__':
    main()
