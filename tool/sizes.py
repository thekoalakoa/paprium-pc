# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiles the generated code for several targets and reports its size in bytes, by kind.

  python -m tool.sizes --gen GEN_FOLDER --out OUT_FOLDER --target 'NAME|CC|SIZE_TOOL|FLAGS' [...] [--define MACRO] [--jobs N]

Kinds:
  translated code       the text of the generated C functions
  switch tables         read-only data of the generated C functions (jump tables and the like)
  address map           the sorted address map and the overlay list
  runtime               the dispatcher, the interrupt entry
  fallback interpreter  the small interpreter

A target's FLAGS are separated by spaces. Objects stay in OUT_FOLDER/<NAME>. A file is compiled again only
when its object is missing, so give a new NAME for new flags or new code.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(REPO, 'runtime')
RIG = os.path.join(REPO, 'rig')


def sections(size_tool, obj, env):
    """Bytes per class of section, from `size -A`."""
    r = subprocess.run([size_tool, '-A', obj], capture_output=True, text=True, env=env, check=True)
    out = {'text': 0, 'rodata': 0, 'data': 0, 'bss': 0}
    for line in r.stdout.splitlines():
        w = line.split()
        if len(w) < 2 or not w[1].isdigit():
            continue
        name, n = w[0], int(w[1])
        if name.startswith('.text'):
            out['text'] += n
        elif name.startswith(('.rdata', '.rodata', '.srodata')):
            out['rodata'] += n
        elif name.startswith(('.data', '.sdata')):
            out['data'] += n
        elif name.startswith(('.bss', '.sbss')):
            out['bss'] += n
    return out


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('--gen', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--target', action='append', required=True)
    p.add_argument('--define', action='append', default=[])
    p.add_argument('--jobs', type=int, default=8)
    args = p.parse_args(argv)
    env = dict(os.environ)
    gcc_dir = os.path.dirname(shutil.which('gcc') or r'C:\msys64\mingw64\bin\gcc.exe')
    env['PATH'] = gcc_dir + os.pathsep + env.get('PATH', '')
    gen_files = sorted(glob.glob(os.path.join(args.gen, 'gen_*.c')))
    results = {}
    for spec in args.target:
        name, cc, size_tool, flags = spec.split('|', 3)
        d = os.path.join(args.out, name)
        os.makedirs(d, exist_ok=True)
        base = ['-w', '-std=gnu11', '-foptimize-sibling-calls', '-I', args.gen, '-I', RT, '-I', RIG] + flags.split()
        base += ['-D' + x for x in args.define]
        jobs = []
        for f in gen_files + [os.path.join(RT, 'pt_rt.c'), os.path.join(RT, 'pt_fallback.c')]:
            o = os.path.join(d, os.path.basename(f)[:-2] + '.o')
            jobs.append((f, o))

        def one(job):
            f, o = job
            if os.path.exists(o):
                return None
            r = subprocess.run([cc, '-c', f, '-o', o] + base, capture_output=True, text=True, env=env)
            return None if r.returncode == 0 else f + ':\n' + r.stderr[:2000]

        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            errors = [e for e in pool.map(one, jobs) if e]
        if errors:
            print('\n'.join(errors))
            return 1
        kinds = {'translated code': 0, 'switch tables': 0, 'address map': 0, 'runtime': 0, 'fallback interpreter': 0, 'data': 0}
        for f, o in jobs:
            s = sections(size_tool, o, env)
            base_name = os.path.basename(f)
            if base_name == 'gen_map.c':
                kinds['address map'] += s['text'] + s['rodata'] + s['data']
            elif base_name == 'pt_rt.c':
                kinds['runtime'] += s['text'] + s['rodata']
                kinds['data'] += s['data']
            elif base_name == 'pt_fallback.c':
                kinds['fallback interpreter'] += s['text'] + s['rodata']
                kinds['data'] += s['data']
            else:
                kinds['translated code'] += s['text']
                kinds['switch tables'] += s['rodata']
                kinds['data'] += s['data']
        kinds['total'] = sum(v for k, v in kinds.items())
        results[name] = kinds
        print(name, json.dumps(kinds), flush=True)
    with open(os.path.join(args.out, 'sizes.json'), 'a', newline='\n') as f:
        f.write(json.dumps(results) + '\n')
    print('RESULT: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
