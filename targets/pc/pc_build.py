# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the PC program's pieces with gcc: the firmware from its own tree, this folder, the runtime and the generated code.

Used by ptool.py's pc-boot-image and pc-build commands, and by the tests. A file is compiled again only when its text, what it includes, or the flags changed. Everything built goes
to the output folder given. Nothing here reads from rig/.
"""
import concurrent.futures
import glob
import hashlib
import importlib.util
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RUNTIME = os.path.join(REPO, 'runtime')
GCC = shutil.which('gcc') or r'C:\msys64\mingw64\bin\gcc.exe'

# The machine and the renderer, in this folder. pc_boot_image.c is added where it is wanted.
# The sound work's two halves each add their files to their own line here. See pc_sound.h.
SOUND_Z = ('pc_ym', 'pc_psg')
SOUND_C = ('pc_synth', 'pc_mix')
# The add-ons the game can find (a 32X, a Mega CD and its link to the CPU MATE brain, pc_addons.h) are machine parts too. No brain is built in: pc_brain.h.
MACHINE = ('pc_clock', 'pc_vdp', 'pc_bus', 'pc_io', 'pc_z80', 'pc_save', 'pc_config', 'pc_scum', 'pc_addons', 'pc_brain', 'pc_rv32') + SOUND_Z
RENDERER = ('pr_render', 'pr_planes', 'pr_sprites')
# The files that see SDL: the window and its picture filters, the keys and pads with the controls screen and its font, the sound device. The menu bar's
# model and drawing (pc_menu.c) need no SDL, but go with the window; a program built without SDL takes it and the font beside its stand-ins (NO_WINDOW).
WINDOW = ('pc_window', 'pc_filter', 'pc_osd', 'pc_input', 'pc_controls', 'pc_font', 'pc_audio', 'pc_menu')
NO_WINDOW = ('pc_nowindow', 'pc_menu', 'pc_font')
# The picture filters' shaders, as their authors wrote them, from libretro's glsl-shaders collection at commit a35069864b4b: their text is built into the
# program (see shader_text_file), which takes these files, with exactly this text, and no other. See THIRD_PARTY.md and shaders/README.md.
SHADERS = os.path.join(HERE, 'shaders')
SHADER_PINS = {'crt-geom.glsl': '6f4c98cc71e8d021d141f3b2f517bbbfeb93a6c9701469c5c855a5c67aaad2e9',
               'crt-hyllian.glsl': '466c1d88ceda53d77518b9f47e535e074e221aaa6c3513ccb9d68dcd06bc25b1',
               'crt-lottes.glsl': '699a947fc2744eb7e2dc4adb2c08a8b74e31685baa77cbc1face9593106b57ef'}
CART = ('pc_cart', 'pc_fw_vars') + SOUND_C


# The three sound chips' cores, from their authors' own sources. None is in this repository: the local configuration's sound_cores names a folder for each, and the build takes these files, with
# exactly this text, and no other.
SOUND_CORE_PINS = {
    'z80': {'chips/z80.h': 'e97952a5559606b026ad408d9afb1148bcce45819f5a249c6c645ac6b2a07ad2',
            'LICENSE': 'fc07ee01564ff9ef8149c38fdff522a76414a9ec9cc6d9a47bd878b64cbc970f'},
    'fm': {'ym3438.c': '416a96457a1ab17cb115ee61b3fb1eee55f009883aa2b88c900c5da5aeaa8a73',
           'ym3438.h': 'f55589f78a6424cae29e8ce6c2a49d0ad9ab702b983e597490abcb0a1e314a41',
           'LICENSE': '7ffe1954587c77dfba1cf8eb9b2ea743671fa6e63f9e7a2f258119d42e14eefe'},
    'psg': {'ympsg.c': 'f2a803f1003919b249148503934854cdeacc7f75e0ad1d3c19390fac7eadfad7',
            'ympsg.h': 'c11d2792e71a81ebcbb7d623fe0b0a63f8fe830c1f110e5e193477034c096407',
            'LICENSE': '189b1af95d661151e054cea10c91b3d754e4de4d3fecfb074c1fb29476f7167b'},
}

# Each core's licence goes beside the program, under these names, so the program's package carries it. See THIRD_PARTY.md.
SOUND_CORE_LICENCES = {'z80': 'Z80_LICENSE.txt', 'fm': 'NUKED_OPN2_LICENSE.txt', 'psg': 'NUKED_PSG_LICENSE.txt'}

# SDL's community controller database, SDL_GameControllerDB, at its commit c0f11d40b91f57853ce9714c703e119d8243f91c of 2026-11-09: the pads SDL does
# not know itself (pc_input.c). Not in this repository: the local configuration's controller_db names a folder that holds these two files with exactly
# this text. It goes beside the program as it is, with its licence (zlib) beside it.
CONTROLLER_DB_PINS = {'gamecontrollerdb.txt': 'a2299ef257b68c62b9fcce39fc59b5fb3a5a7068214f766a96c21595b55a11c3',
                      'LICENSE': '97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127'}
CONTROLLER_DB_LICENCE = 'GAMECONTROLLERDB_LICENSE.txt'


def controller_db_files(folder):
    """The database and its licence, after both matched their pins: (database, licence)."""
    if not folder:
        raise RuntimeError('the local configuration names no controller_db: the folder of the gamecontrollerdb.txt and LICENSE of SDL_GameControllerDB')
    paths = []
    for name, want in CONTROLLER_DB_PINS.items():
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            raise RuntimeError('controller_db: no file ' + path)
        with open(path, 'rb') as f:
            if hashlib.sha256(f.read()).hexdigest() != want:
                raise RuntimeError('controller_db: %s is not the pinned text of the database' % path)
        paths.append(path)
    return paths[0], paths[1]


def copy_controller_db(folder, out_dir):
    """Puts the database beside the program, and its licence as CONTROLLER_DB_LICENCE."""
    db, licence = controller_db_files(folder)
    shutil.copy2(db, os.path.join(out_dir, 'gamecontrollerdb.txt'))
    shutil.copy2(licence, os.path.join(out_dir, CONTROLLER_DB_LICENCE))


def sound_core_flags(cores):
    """Checks the cores against their pins and returns the flags that find their headers. cores is the configuration's sound_cores."""
    if not cores:
        raise RuntimeError('the local configuration names no sound_cores: the folders of chips, Nuked-OPN2 and Nuked-PSG')
    flags = []
    for name, files in SOUND_CORE_PINS.items():
        if name not in cores:
            raise RuntimeError('sound_cores names no folder for ' + name)
        for rel, want in files.items():
            path = os.path.join(cores[name], rel)
            if not os.path.exists(path):
                raise RuntimeError('sound_cores: no file ' + path)
            with open(path, 'rb') as f:
                if hashlib.sha256(f.read()).hexdigest() != want:
                    raise RuntimeError('sound_cores: %s is not the pinned text of that core' % path)
        flags += ['-I', cores[name]]
    return flags


def sound_core_jobs(cores, obj_dir, opt):
    """The two cores that are C files. The Z80's is a header, which pc_z80.c compiles."""
    sound_core_flags(cores)
    os.makedirs(obj_dir, exist_ok=True)
    flags = ['-w', '-std=gnu11', opt]
    return [(os.path.join(cores[name], source), os.path.join(obj_dir, source[:-2] + '.o'), flags, [os.path.join(cores[name], source[:-2] + '.h')])
            for name, source in (('fm', 'ym3438.c'), ('psg', 'ympsg.c'))]


def env():
    e = dict(os.environ)
    e['PATH'] = os.path.dirname(GCC) + os.pathsep + e.get('PATH', '')
    return e


def _firmware_list():
    spec = importlib.util.spec_from_file_location('pc_firmware', os.path.join(HERE, 'pc_firmware.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _digest(paths, flags):
    h = hashlib.sha256(' '.join(flags).encode())
    for p in paths:
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()


def _compile_one(job):
    src, obj, flags, deps = job
    stamp = obj + '.hash'
    want = _digest([src] + deps, flags)
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


def compile_jobs(jobs, workers):
    """Compiles what changed. Returns the objects in the jobs' order, or raises with the compiler's words."""
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        errors = [e for e in pool.map(_compile_one, jobs) if e]
    if errors:
        raise RuntimeError('\n'.join(errors))
    return [j[1] for j in jobs]


def link(objects, exe, libs=()):
    rsp = exe + '.rsp'
    with open(rsp, 'w', newline='\n') as f:
        for o in objects:
            f.write('"%s"\n' % o.replace('\\', '/'))
    r = subprocess.run([GCC, '-o', exe, '@' + rsp, '-Wl,--stack,67108864', '-lm'] + list(libs), capture_output=True, text=True, env=env())
    if r.returncode:
        raise RuntimeError(r.stderr[:4000])


def firmware_jobs(tree, obj_dir, opt):
    """The firmware as its RELEASE image is built, on its x86 board, without the files pc_firmware.py leaves out."""
    import json
    fw = _firmware_list()
    os.makedirs(obj_dir, exist_ok=True)
    with open(os.path.join(tree, 'fwbuild.json'), encoding='utf-8') as f:
        s1 = json.load(f)['s1']
    include = [HERE] + [os.path.join(tree, x) for x in s1['host']['include']] + [os.path.join(tree, 'core', 'mw'), os.path.join(tree, 'core', 'wavpack'), RUNTIME]
    flags = ['-w', '-std=gnu11', opt, '-fno-strict-aliasing', '-funsigned-char', '-DFW_S1=1', '-DFW_S5=1', '-DFW_MW=1', '-DPPM_RELEASE=1', '-DBRD_CLK_HZ=53693175',
             '-include', os.path.join(HERE, 'pc_board_impl.h')]
    flags += [y for x in include for y in ('-I', x)]
    # The wave bank's decoder wants signed chars, by its own rule, as in the firmware's boot overlay.
    wav_flags = ['-w', '-std=gnu11', opt, '-fno-strict-aliasing', '-fsigned-char', '-DFW_S1=1', '-DFW_S5=1', '-I', os.path.join(tree, 'core', 'wavpack')]
    deps = sorted(glob.glob(os.path.join(tree, 'core', '*', '*.h')) + glob.glob(os.path.join(tree, 'board', '*.h')) + glob.glob(os.path.join(tree, 'board', 'host', '*.h')) +
                  glob.glob(os.path.join(HERE, '*.h')))
    jobs = []
    for name in fw.sources(tree):
        jobs.append((os.path.join(tree, name), os.path.join(obj_dir, 'fw_' + name.replace('/', '_')[:-2] + '.o'), flags, deps))
    for name in fw.wavpack_sources():
        jobs.append((os.path.join(tree, name), os.path.join(obj_dir, 'fw_wv_' + os.path.basename(name)[:-2] + '.o'), wav_flags, deps))
    for name in CART:
        jobs.append((os.path.join(HERE, name + '.c'), os.path.join(obj_dir, name + '.o'), flags, deps))
    return jobs


def machine_jobs(obj_dir, opt, names=MACHINE, extra=()):
    """This folder's files and nothing of the firmware, so they see only their own headers."""
    os.makedirs(obj_dir, exist_ok=True)
    flags = ['-w', '-std=gnu11', opt, '-foptimize-sibling-calls', '-DPT_PC_BUILD', '-I', RUNTIME, '-I', HERE] + list(extra)
    deps = sorted(glob.glob(os.path.join(HERE, '*.h')) + glob.glob(os.path.join(RUNTIME, '*.h')))
    return [(os.path.join(HERE, n + '.c'), os.path.join(obj_dir, n + '.o'), flags, deps) for n in names]


def runtime_jobs(obj_dir, opt, extra=()):
    os.makedirs(obj_dir, exist_ok=True)
    flags = ['-w', '-std=gnu11', opt, '-foptimize-sibling-calls', '-DPT_PC_BUILD', '-I', RUNTIME] + list(extra)
    deps = sorted(glob.glob(os.path.join(RUNTIME, '*.h')))
    return [(os.path.join(RUNTIME, n + '.c'), os.path.join(obj_dir, n + '.o'), flags, deps) for n in ('pt_rt', 'pt_fallback')]


def generated_jobs(gen_dir, obj_dir, opt, extra=()):
    os.makedirs(obj_dir, exist_ok=True)
    flags = ['-w', '-std=gnu11', opt, '-foptimize-sibling-calls', '-DPT_PC_BUILD', '-I', RUNTIME, '-I', gen_dir] + list(extra)
    deps = [os.path.join(gen_dir, 'pt_gen.h')] + sorted(glob.glob(os.path.join(RUNTIME, '*.h')))
    return [(src, os.path.join(obj_dir, os.path.basename(src)[:-2] + '.o'), flags, deps) for src in sorted(glob.glob(os.path.join(gen_dir, 'gen_*.c')))]


def build_boot_image(tree, out_dir, opt='-O1', workers=2):
    """The tool that lists what the firmware's boot changes in the program."""
    obj = os.path.join(out_dir, 'obj_fw')
    jobs = firmware_jobs(tree, obj, opt)
    fw_flags = jobs[0][2]
    jobs.append((os.path.join(HERE, 'pc_boot_image.c'), os.path.join(obj, 'pc_boot_image.o'), fw_flags, jobs[0][3]))
    objects = compile_jobs(jobs, workers)
    exe = os.path.join(out_dir, 'pc_boot_image.exe')
    link(objects, exe)
    return exe


def _write_build_facts(obj_dir, rom_sha256, image_check):
    """What the program was translated from, as a C file in the output folder, so the program can refuse any other game file. The folder never holds a tracked file."""
    os.makedirs(obj_dir, exist_ok=True)
    path = os.path.join(obj_dir, 'pc_build_facts.c')
    text = 'const char pc_built_rom_sha256[] = "%s";\nconst char pc_built_image_check[] = "%s";\n' % (rom_sha256 or '', image_check or '')
    if not os.path.exists(path) or open(path, encoding='ascii').read() != text:
        with open(path, 'w', encoding='ascii', newline='\n') as f:
            f.write(text)
    return path


def _sdl_folder(sdl_dir):
    """The SDL2 development folder holds include/SDL2, lib and bin, for one processor. A release's top folder is taken too."""
    for folder in (sdl_dir, os.path.join(sdl_dir, 'x86_64-w64-mingw32')):
        if os.path.exists(os.path.join(folder, 'include', 'SDL2', 'SDL.h')):
            return folder
    raise RuntimeError('no SDL2 development files in ' + sdl_dir)


def shader_text_file(obj_dir):
    """The shaders folder's .glsl files as C text for pc_filter.c: pc_shader_files, pc_shader_texts and pc_shader_count, in a C file written into obj_dir when it would change.
    Each file's bytes are kept exactly, notices and all. Returns the C file's path."""
    os.makedirs(obj_dir, exist_ok=True)
    names = sorted(n for n in os.listdir(SHADERS) if n.endswith('.glsl'))
    if names != sorted(SHADER_PINS):
        raise RuntimeError('targets/pc/shaders holds %s, and the build takes exactly %s' % (', '.join(names), ', '.join(sorted(SHADER_PINS))))
    out = ['/* Written by targets/pc/pc_build.py from the files in targets/pc/shaders, for pc_filter.c. Each text is its file exactly. */']
    for i, name in enumerate(names):
        with open(os.path.join(SHADERS, name), 'rb') as f:
            data = f.read()
        if hashlib.sha256(data).hexdigest() != SHADER_PINS[name]:
            raise RuntimeError('targets/pc/shaders/%s is not the pinned text of that shader' % name)
        text = data.decode('ascii')
        out.append('static const char text_%d[] =' % i)
        for line in text.splitlines(True):
            out.append('    "%s"' % line.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r').replace('\n', '\\n'))
        out.append('    "";')
    out.append('const char *const pc_shader_files[] = {%s};' % ', '.join('"%s"' % n for n in names))
    out.append('const char *const pc_shader_texts[] = {%s};' % ', '.join('text_%d' % i for i in range(len(names))))
    out.append('const int pc_shader_count = %d;' % len(names))
    text = '\n'.join(out) + '\n'
    path = os.path.join(obj_dir, 'pc_shader_text.c')
    if not os.path.exists(path) or open(path, encoding='ascii').read() != text:
        with open(path, 'w', encoding='ascii', newline='\n') as f:
            f.write(text)
    return path


def window_jobs(obj_dir, opt, sdl_dir):
    """The window and its picture filters, the keys and pads with their controls screen, and the audio device, which alone see SDL, and the filters' shader text."""
    folder = _sdl_folder(sdl_dir)
    os.makedirs(obj_dir, exist_ok=True)
    flags = ['-w', '-std=gnu11', opt, '-I', HERE, '-I', os.path.join(folder, 'include', 'SDL2')]
    deps = sorted(glob.glob(os.path.join(HERE, '*.h')))
    text = shader_text_file(obj_dir)
    return [(os.path.join(HERE, n + '.c'), os.path.join(obj_dir, n + '.o'), flags, deps) for n in WINDOW] + [(text, text[:-2] + '.o', flags, [])]


def sdl_link_args(sdl_dir):
    return ['-L' + os.path.join(_sdl_folder(sdl_dir), 'lib'), '-lSDL2']


def sdl_dll(sdl_dir):
    return os.path.join(_sdl_folder(sdl_dir), 'bin', 'SDL2.dll')


def manifest_object(manifest, obj_dir):
    """A resource object that holds one of this folder's manifests, for the link: windres sits beside gcc."""
    os.makedirs(obj_dir, exist_ok=True)
    name = os.path.splitext(os.path.basename(manifest))[0]
    rc = os.path.join(obj_dir, name + '.rc')
    obj = os.path.join(obj_dir, name + '.res.o')
    with open(rc, 'w', encoding='ascii', newline='\n') as f:
        f.write('1 24 "%s"\n' % os.path.abspath(manifest).replace('\\', '/'))
    windres = os.path.join(os.path.dirname(GCC), 'windres.exe')
    r = subprocess.run([windres, rc, '-O', 'coff', '-o', obj], capture_output=True, text=True, env=env())
    if r.returncode:
        raise RuntimeError('windres: ' + r.stderr[:2000])
    return obj


# ---- the players' build. The package a player downloads holds no translated game: the launcher builds pc_game.dll on the player's PC, and the
# program below loads it (pc_game_dll.h). These are the runtime's names the DLL imports from the program, by the program's file name: its data
# first, then its functions. tests/test_launcher.py checks that each is declared in runtime/pt_rt.h or runtime/pt_port.h, and that leaving one
# out makes TinyCC refuse the DLL.
GAME_DLL_DATA = ('pt_irq_level', 'pt_run_exit_pc', 'pt_fallback_uses', 'pt_interp_only', 'pt_rom', 'pt_rom_size', 'pt_low_device_end', 'pt_rom_bus_end',
                 'pt_ram', 'pt_low_from_ram', 'pt_low_ram', 'pt_low_ram_end', 'pt_clock', 'pt_deadline', 'pt_at', 'pt_switches')
GAME_DLL_FUNCTIONS = ('pt_enter', 'pt_take_irq', 'pt_stopped', 'pt_halted', 'pt_fallback', 'pt_ramcode_same', 'pt_clocks_divu', 'pt_clocks_divs',
                      'pt_port_read', 'pt_port_write', 'pt_port_tick', 'pt_port_delay', 'pt_watch_hit', 'pt_port_irq_ack', 'pt_port_irq_return',
                      'pt_run', 'pt_map_has', 'pt_fallback_one', 'pt_switch_point')
GAME_EXE = 'pc_port.exe'


def game_dll_def(path, exe_name=GAME_EXE, kind='import', leave_out=()):
    """The list of the program's exports as a .def file. kind 'program': for the program's own link (NAME, so the linker keeps it a program).
    kind 'import': for TinyCC, which builds the DLL against it (LIBRARY, the module the DLL imports from)."""
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        f.write('; Copyright (C) 2026 Marsupial Solutions LLC\n; SPDX-License-Identifier: GPL-3.0-or-later\n')
        f.write('; What the game DLL imports from the game program: see targets/pc/pc_game_dll.h. Written by targets/pc/pc_build.py.\n')
        f.write(('NAME %s\n' if kind == 'program' else 'LIBRARY %s\n') % exe_name)
        f.write('EXPORTS\n')
        for name in GAME_DLL_DATA:
            if name not in leave_out:
                f.write('  %s DATA\n' % name)
        for name in GAME_DLL_FUNCTIONS:
            if name not in leave_out:
                f.write('  %s\n' % name)
    return path


def build_player_program(tree, out_dir, sdl_dir, rom_sha256, image_check, sound_cores, workers=2, opt='-O2', fw_opt='-O1', exe_name=GAME_EXE):
    """The players' game program: everything build_program links but the generated code, built to load the game DLL (PT_GAME_FROM_DLL), exporting
    the runtime's names the DLL imports, with the UTF-8 manifest. Its objects go to their own folders, apart from the developers' build. Writes a
    linker map beside the program for the package's leak check, and the program's twin without a symbol table (link_shipped)."""
    flag = ['-DPT_GAME_FROM_DLL']
    obj = os.path.join(out_dir, 'obj_player')
    objects = compile_jobs(firmware_jobs(tree, os.path.join(out_dir, 'obj_fw'), fw_opt), workers)
    objects += compile_jobs(runtime_jobs(os.path.join(obj, 'rt'), opt, flag), workers)
    names = MACHINE + RENDERER + ('pc_main', 'pc_stall', 'pc_game_dll')
    objects += compile_jobs(machine_jobs(os.path.join(obj, 'pc'), opt, names, sound_core_flags(sound_cores) + flag), workers)
    objects += compile_jobs(sound_core_jobs(sound_cores, os.path.join(obj, 'cores'), opt), workers)
    facts = _write_build_facts(os.path.join(obj, 'facts'), rom_sha256, image_check)
    objects += compile_jobs([(facts, facts[:-2] + '.o', ['-w', '-std=gnu11'], [])], 1)
    objects += compile_jobs(window_jobs(os.path.join(obj, 'win'), opt, sdl_dir), workers)
    objects.append(manifest_object(os.path.join(HERE, 'pc_utf8.manifest'), os.path.join(obj, 'res')))
    exe_def = game_dll_def(os.path.join(obj, 'program.def'), exe_name, 'program')
    exe = os.path.join(out_dir, exe_name)
    link_shipped(objects, exe, [exe_def, '-Wl,-Map,' + exe + '.map'] + sdl_link_args(sdl_dir) + ['-lwinmm'])
    shutil.copy2(sdl_dll(sdl_dir), os.path.join(out_dir, 'SDL2.dll'))
    return exe


def pe_view(path):
    """What a 64-bit PE program runs, read from its file: the entry point, the image base, the section alignment, the subsystem and its flags,
    the stack and heap sizes, every data directory but the debug one, and every section but the debug information, each section as its name,
    address, size, flags and contents (less the zero bytes at its end). Two programs with equal views run the same code and data at the same
    addresses. Also returns the count of symbol-table records the file holds."""
    with open(path, 'rb') as f:
        data = f.read()
    u16 = lambda at: int.from_bytes(data[at:at + 2], 'little')
    u32 = lambda at: int.from_bytes(data[at:at + 4], 'little')
    u64 = lambda at: int.from_bytes(data[at:at + 8], 'little')
    pe = u32(0x3C)
    if data[pe:pe + 4] != b'PE\0\0' or u16(pe + 24) != 0x20B:
        raise RuntimeError('%s is not a 64-bit PE program' % path)
    coff, opt = pe + 4, pe + 24
    nsec, symtab, nsym, opt_size = u16(coff + 2), u32(coff + 8), u32(coff + 12), u16(coff + 16)
    head = (u32(opt + 16), u64(opt + 24), u32(opt + 32), u16(opt + 68), u16(opt + 70), u64(opt + 72), u64(opt + 80), u64(opt + 88), u64(opt + 96),
            tuple((u32(opt + 112 + 8 * i), u32(opt + 116 + 8 * i)) for i in range(u32(opt + 108)) if i != 6))
    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        name = data[s:s + 8].rstrip(b'\0')
        if name.startswith(b'/'):          # a long name, kept in the string table after the symbol records
            at = symtab + 18 * nsym + int(name[1:])
            name = data[at:data.index(b'\0', at)]
        vsize, va, rsize, rptr, flags = u32(s + 8), u32(s + 12), u32(s + 16), u32(s + 20), u32(s + 36)
        if name.startswith(b'.debug'):
            continue
        # A name is compared by its first 8 bytes: with no string table the linker keeps only those (".bss_upper" becomes ".bss_upp"), and
        # Windows loads a section by its place, never by its name.
        sections.append((name[:8], va, vsize, flags, data[rptr:rptr + min(rsize, vsize)].rstrip(b'\0') if rptr else b''))
    return (head, tuple(sections)), nsym


def shipped(exe):
    """The twin link_shipped makes of exe, the one a package carries."""
    return os.path.join(os.path.dirname(exe), 'ship', os.path.basename(exe))


def link_shipped(objects, exe, libs=()):
    """Links exe from objects twice, with no time stamp in either, so a build can be repeated byte for byte: exe itself, with its symbol table
    and the debug information the compiler's runtime libraries bring (its linker map, tests/check_pc_link.py and a debugger read them), and its
    twin, ship/<name> beside it, linked with -s, so it carries neither. The twin is the one the players' package carries (the team's decision:
    no symbol table in the programs we ship). Raises unless the twin runs exactly what exe runs (pe_view) and holds no symbol
    record. Returns the twin's path."""
    link(objects, exe, list(libs) + ['-Wl,--no-insert-timestamp'])
    twin = shipped(exe)
    os.makedirs(os.path.dirname(twin), exist_ok=True)
    link(objects, twin, [x for x in libs if not x.startswith('-Wl,-Map,')] + ['-s', '-Wl,--no-insert-timestamp'])
    full, _ = pe_view(exe)
    lean, records = pe_view(twin)
    if full != lean or records:
        raise RuntimeError('%s without its symbol table does not run the same code and data as with it (or keeps %d symbol records)' % (exe, records))
    return twin


def build_player_boot_image(tree, out_dir, workers=2):
    """The boot image tool as the package carries it: build_boot_image's, with the UTF-8 manifest and a linker map, and its twin without a
    symbol table (link_shipped)."""
    obj = os.path.join(out_dir, 'obj_fw')
    jobs = firmware_jobs(tree, obj, '-O1')
    jobs.append((os.path.join(HERE, 'pc_boot_image.c'), os.path.join(obj, 'pc_boot_image.o'), jobs[0][2], jobs[0][3]))
    objects = compile_jobs(jobs, workers)
    objects.append(manifest_object(os.path.join(HERE, 'pc_utf8.manifest'), os.path.join(out_dir, 'obj_player', 'res')))
    exe = os.path.join(out_dir, 'pc_boot_image.exe')
    link_shipped(objects, exe, ['-Wl,-Map,' + exe + '.map'])
    return exe


def build_launcher(out_dir, workers=2, exe_name='Paprium.exe'):
    """The players' launcher, pl_launcher.c: a windowed program with wide-character file names. It takes the SHA-256 from pc_config.c. Its twin
    without a symbol table is made too (link_shipped)."""
    obj = os.path.join(out_dir, 'obj_launcher')
    os.makedirs(obj, exist_ok=True)
    flags = ['-w', '-std=gnu11', '-O2', '-municode', '-I', HERE]
    deps = sorted(glob.glob(os.path.join(HERE, '*.h')))
    objects = compile_jobs([(os.path.join(HERE, 'pl_launcher.c'), os.path.join(obj, 'pl_launcher.o'), flags, deps),
                            (os.path.join(HERE, 'pc_config.c'), os.path.join(obj, 'pc_config.o'), flags, deps)], workers)
    objects.append(manifest_object(os.path.join(HERE, 'pl_launcher.manifest'), obj))
    exe = os.path.join(out_dir, exe_name)
    link_shipped(objects, exe, ['-mwindows', '-municode', '-Wl,-Map,' + exe + '.map', '-lcomctl32', '-lcomdlg32', '-lshell32', '-lshlwapi', '-lgdi32'])
    return exe


def build_program(tree, gen_dir, out_dir, opt='-O2', workers=2, fw_opt='-O1', sdl_dir=None, rom_sha256='', image_check='', sound_cores=None, controller_db=None):
    """The whole PC program. With sdl_dir it has its window, and SDL2.dll goes beside it. Without, only headless runs work. With controller_db, the
    controller database goes beside it too."""
    obj = os.path.join(out_dir, 'obj')
    objects = compile_jobs(firmware_jobs(tree, os.path.join(out_dir, 'obj_fw'), fw_opt), workers)
    objects += compile_jobs(generated_jobs(gen_dir, os.path.join(obj, 'gen'), opt), workers)
    objects += compile_jobs(runtime_jobs(os.path.join(obj, 'rt'), opt), workers)
    names = MACHINE + RENDERER + ('pc_main', 'pc_stall') + (() if sdl_dir else NO_WINDOW)
    objects += compile_jobs(machine_jobs(os.path.join(obj, 'pc'), opt, names, sound_core_flags(sound_cores)), workers)
    objects += compile_jobs(sound_core_jobs(sound_cores, os.path.join(obj, 'cores'), opt), workers)
    facts = _write_build_facts(os.path.join(obj, 'facts'), rom_sha256, image_check)
    objects += compile_jobs([(facts, facts[:-2] + '.o', ['-w', '-std=gnu11'], [])], 1)
    libs = ['-lwinmm']
    if sdl_dir:
        objects += compile_jobs(window_jobs(os.path.join(obj, 'win'), opt, sdl_dir), workers)
        libs = sdl_link_args(sdl_dir) + libs
    exe = os.path.join(out_dir, 'pc_port.exe')
    link(objects, exe, libs)
    if sdl_dir:
        shutil.copy2(sdl_dll(sdl_dir), os.path.join(out_dir, 'SDL2.dll'))
        if controller_db:
            copy_controller_db(controller_db, out_dir)
    for name, licence in SOUND_CORE_LICENCES.items():
        shutil.copy2(os.path.join(sound_cores[name], 'LICENSE'), os.path.join(out_dir, licence))
    return exe
