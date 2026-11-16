# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the programs that run generated code. Everything built goes to the output folder.

  --mode reference   the reference machine alone (no generated code)
  --mode lockstep    the reference machine, the generated code and the lockstep core
  --mode replay      the generated code alone, replaying a lockstep run's record; for timing

--fw-src adds the firmware as the cart (fw/fw_cart.c): the firmware's own source list, from its manifest, on its
x86 board, with no brain. --gpgx names the copy of the reference machine to link.
--shadow, with the reference and the firmware, also runs the PC program's video chip model on the same writes and compares memories each frame (pc_shadow.c).

A generated file is compiled again only when its text or the flags changed, so a change to one
block costs one file.
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(REPO, 'runtime')
RIG = os.path.join(REPO, 'rig')
FW = os.path.join(RIG, 'fw')
GCC = shutil.which('gcc') or r'C:\msys64\mingw64\bin\gcc.exe'


def env():
    e = dict(os.environ)
    e['PATH'] = os.path.dirname(GCC) + os.pathsep + e.get('PATH', '')
    return e


def digest(paths, flags):
    h = hashlib.sha256(' '.join(flags).encode())
    for p in paths:
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()


def compile_one(src, obj, flags, deps):
    stamp = obj + '.hash'
    want = digest([src] + deps, flags)
    if os.path.exists(obj) and os.path.exists(stamp):
        with open(stamp, encoding='ascii') as f:
            if f.read() == want:
                return None
    r = subprocess.run([GCC, '-c', src, '-o', obj] + flags, capture_output=True, text=True, env=env())
    if r.returncode:
        return src + ':\n' + r.stderr[:3000]
    with open(stamp, 'w', encoding='ascii') as f:
        f.write(want)
    return None


def firmware_jobs(src, obj_dir, opt):
    """The compile jobs of the firmware as the 0.3.0 RELEASE image is built, on the x86 board."""
    with open(os.path.join(src, 'fwbuild.json'), encoding='utf-8') as f:
        s1 = json.load(f)['s1']
    sources = list(s1['pocket']['sources']) + list(s1['pocket']['variant_sources']['s5release'])
    keep = []
    for name in sources:
        # What only the Pocket has, and the CPU MATE glue: no brain here. The fatal halt is fw_cart.c's.
        if name.startswith('board/pocket/') or name.startswith('buddy/') or name == 'core/trace/fatal.c':
            continue
        if name not in keep:
            keep.append(name)
    keep.append('board/host/s1_board.c')
    include = [FW] + [os.path.join(src, x) for x in s1['host']['include']] + [os.path.join(src, 'core', 'mw'),
                                                                               os.path.join(src, 'core', 'wavpack')]
    flags = ['-w', '-std=gnu11', opt, '-fno-strict-aliasing', '-funsigned-char', '-DFW_S1=1', '-DFW_S5=1', '-DFW_MW=1',
             '-DPPM_RELEASE=1', '-DBRD_CLK_HZ=53693175']
    flags += [y for x in include for y in ('-I', x)]
    # The wave bank's decoder wants signed chars, by its own rule, as in the firmware's boot overlay.
    wav_flags = ['-w', '-std=gnu11', opt, '-fno-strict-aliasing', '-fsigned-char', '-DFW_S1=1', '-DFW_S5=1',
                 '-I', os.path.join(src, 'core', 'wavpack')]
    deps = sorted(glob.glob(os.path.join(src, 'core', '*', '*.h')) + glob.glob(os.path.join(src, 'board', '*.h')) +
                  glob.glob(os.path.join(src, 'board', 'host', '*.h')) + glob.glob(os.path.join(FW, '*.h')))
    jobs = []
    for name in keep:
        jobs.append((os.path.join(src, name), os.path.join(obj_dir, 'fw_' + name.replace('/', '_')[:-2] + '.o'), flags, deps))
    jobs.append((os.path.join(FW, 'fw_cart.c'), os.path.join(obj_dir, 'fw_cart.o'), flags, deps))
    for name in ('bits', 'metadata', 'unpack', 'words', 'wputils', 'wvbank'):
        jobs.append((os.path.join(src, 'core', 'wavpack', name + '.c'), os.path.join(obj_dir, 'fw_wv_' + name + '.o'), wav_flags, deps))
    return jobs


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--config', required=True)
    p.add_argument('--mode', required=True, choices=('reference', 'lockstep', 'replay'))
    p.add_argument('--gen', help='the folder of generated code')
    p.add_argument('--obj', required=True)
    p.add_argument('--exe', required=True)
    p.add_argument('--opt', default='-O1')
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--define', action='append', default=[], help='an extra macro, such as PT_STRICT_READS')
    p.add_argument('--fw-src', help='a copy of the firmware tree: it answers as the cart')
    p.add_argument('--gpgx', help='the copy of the reference machine; default out_root/gpgx')
    p.add_argument('--shadow', action='store_true', help='with --mode reference: pc_vdp.c in the shadow of the reference')
    args = p.parse_args()
    with open(args.config, encoding='utf-8') as f:
        cfg = json.load(f)
    os.makedirs(args.obj, exist_ok=True)
    gpgx = args.gpgx or os.path.join(cfg['out_root'], 'gpgx')
    jobs = []
    rt_h = [os.path.join(RT, x) for x in ('pt_rt.h', 'pt_flags.h', 'pt_port.h')] + [os.path.join(RIG, 'ls_core.h')]

    mode_flag = {'lockstep': ['-DPT_LOCKSTEP'], 'replay': ['-DPT_REPLAY'], 'reference': ['-DRIG_REFERENCE_ONLY']}[args.mode]
    base = ['-w', '-std=gnu11', args.opt, '-foptimize-sibling-calls', '-I', RT, '-I', RIG] + mode_flag
    base += ['-D' + d for d in args.define]

    def add(src, name=None, extra=()):
        name = name or os.path.splitext(os.path.basename(src))[0]
        jobs.append((src, os.path.join(args.obj, name + '.o'), base + list(extra), rt_h))

    if args.mode != 'reference':
        gen_h = os.path.join(args.gen, 'pt_gen.h')
        for src in sorted(glob.glob(os.path.join(args.gen, 'gen_*.c'))):
            jobs.append((src, os.path.join(args.obj, os.path.basename(src)[:-2] + '.o'),
                         base + ['-I', args.gen], rt_h + [gen_h]))
        add(os.path.join(RT, 'pt_rt.c'))
        add(os.path.join(RT, 'pt_fallback.c'))
    if args.mode == 'lockstep':
        add(os.path.join(RIG, 'ls_core.c'))
    if args.mode in ('lockstep', 'reference'):
        if args.fw_src:
            add(os.path.join(RIG, 'gpgx', 'rig_main.c'), extra=('-DRIG_FW_CART', '-I', FW) + (('-DRIG_PC_SHADOW',) if args.shadow else ()))
            fw = firmware_jobs(args.fw_src, args.obj, args.opt)
            jobs += fw
            if args.shadow:
                pc = os.path.join(REPO, 'targets', 'pc')
                fw_inc = fw[0][2]
                jobs.append((os.path.join(RIG, 'pc_shadow.c'), os.path.join(args.obj, 'pc_shadow.o'), fw_inc + ['-DPT_PC_BUILD', '-I', RT, '-I', pc], rt_h))
                jobs.append((os.path.join(pc, 'pc_vdp.c'), os.path.join(args.obj, 'pc_vdp.o'), base + ['-UPT_LOCKSTEP', '-DPT_PC_BUILD', '-I', pc], rt_h))
        else:
            add(os.path.join(RIG, 'gpgx', 'rig_main.c'))
    if args.mode == 'replay':
        add(os.path.join(RT, 'pt_main_replay.c'))

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        errors = [e for e in pool.map(lambda j: compile_one(*j), jobs) if e]
    if errors:
        for e in errors:
            print(e)
        print('RESULT: FAIL')
        return 1
    objs = [j[1] for j in jobs]
    if args.mode in ('lockstep', 'reference'):
        with open(os.path.join(gpgx, 'objects.txt'), encoding='ascii') as f:
            objs += [os.path.join(gpgx, line.strip()[2:]) for line in f if line.strip()]
    rsp = args.exe + '.rsp'
    with open(rsp, 'w', newline='\n') as f:
        for o in objs:
            f.write('"%s"\n' % o.replace('\\', '/'))
    extra_libs = ['-lm'] if args.mode == 'replay' else ['-lm', '-lwinmm']
    r = subprocess.run([GCC, '-o', args.exe, '@' + rsp] + extra_libs, capture_output=True, text=True, env=env())
    if r.returncode:
        print(r.stderr[:4000])
        print('RESULT: FAIL')
        return 1
    print('built', args.exe, os.path.getsize(args.exe))
    print('RESULT: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
