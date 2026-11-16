<!-- Copyright (C) 2026 Marsupial Solutions LLC
     SPDX-License-Identifier: GPL-3.0-or-later -->
# Per-target parts (layer 3)

Each target's folder holds its implementation of `runtime/pt_port.h`, its picture, sprite and sound formats, and its build rules.
Nothing in the translator, the runtime or the rig depends on a target.

- `pc/`: the first target. A machine model and a renderer of our own, the firmware as the cart, a window through SDL2. See the comments in
  `pc/pc_frame.h`, the one interface its two parts share. Nothing under `pc/` includes anything from `rig/`; `tests/check_tree.py` checks that, and
  `tests/check_pc_link.py` checks what the finished program links. `pc/pc_firmware.py` says which firmware files go in, and why one old module stays out.

The PC program, in `pc/`:

    python -I -B ptool.py pc-build --config <local.json> [--jobs N] [--opt=-O2] [--no-translate]
    <out_dir>/pc_port.exe                               the window, from paprium_pc.cfg beside the program
    <out_dir>/pc_port.exe --rom FILE --run-dir DIR --frames N [--pad FILE | --route DLL --route-arg TEXT] [--save FILE]     headless, for tests

`pc-build` builds the firmware from the tree the local configuration names, runs a small tool that lists what the firmware's boot changes in the program, translates the program
with those changes, and links everything in `out_dir`. Nothing it makes goes in the repository: the generated code and the program hold the game. `sdl_dir` in the local configuration
names SDL2's development files, and the build puts `SDL2.dll` beside the program. Without it the program builds with no window, and only headless runs work.

With a window the program prints its key list to the console. Player 1 has the arrows, Z X C for A B C, A S D for X Y Z, Enter for Start and Right Shift for Mode; player 2 has I J K L,
1 2 3, 4 5 6, Keypad Enter and Keypad 0; player 3 has no keys, and the third pad. F1 flips the sprite limits between the cart's and none, F2 goes through the picture's shapes (the
Pocket core's Aspect Ratio, Original and Corrected, then 4:3, the default, and square pixels: `pc/pc_window.h`), F3 mutes the sound, F6 goes to the next
picture filter, F10 opens the menu bar, P pauses, F11 is full screen, Esc quits. F4 opens the controls screen over the paused game: every key and pad input of each player, which pad is whose, three or six
buttons, the stick and its dead zone, and the program's own keys, the filter key among them, set by "press it now" and saved in the configuration file when it closes, see
`pc/pc_controls.h`. No two of the default keys are the same. Every control can also be moved in the configuration file, see `pc/pc_config.h`. Game controllers work at once: any pad SDL
knows as a game controller, and any other pad as a plain joystick by its buttons, hats and axes. With `controller_db` in the local configuration the build puts SDL's community
controller database, `gamecontrollerdb.txt`, beside the program, for pads SDL does not know itself. OpenGL draws the window unless `renderer=` in that file says otherwise. If it will not start, the program goes on to the next in a fixed order and says so on the console and in `stalls.txt`: see `pc/pc_window.c`.
The menu bar at the top of the window (`pc/pc_menu.h`) holds every option of those keys, each named with its key, and the Pocket core's Scum!
menu and console settings (`pc/pc_scum.h`): the cheats (Health, Pills, Lives: the patch source's switches, taken at the game's next pad read),
Arcade Mode, Arcade Coins and Attract Only (the game's own ARCADE PAPRIUM mode from a Grand Stick III in port 1 and its switches, `pc/pc_io.h`), the
arcade stage (the start-stage word), 6-Button for each player, 3-Player Mode (an EA 4-Way Play on both ports, or player 3 in the stick's EXT socket),
the console and the Mega CD unit (the Pocket core's two lists), a 32X and CPU MATE; Console Reset, the console's reset button as the Pocket core gives
it (`pc/pc_main.c`, console_reset); and the Aspect Ratio shapes. It is drawn by the program, with its one small font: in a window it has a
strip of its own above the picture (`menu_bar=off` gives it back), in full screen it shows while the mouse is at the top edge or a menu is open.
A menu open pauses the game. What it changes goes into the configuration file through the same writer the controls screen uses, each line in its
own place (a UTF-8 byte order mark at the file's start stays, and a file with zero bytes in it, as one saved as UTF-16 has, is never written over). In
the smallest window a list taller than the window goes on in a second column and a page longer than it shows in parts, so every item can be reached.
Soft Reset ends the run and starts the program again as it was started: a power-on of the whole machine, the cart's chip too, with the
save and the settings as they are; started by the players' launcher, the program asks the launcher to do it (exit code 8,
which no other exit of the program uses), and the launcher starts it with the same arguments but a test's key script, which belongs to the first start.
The picture filters, called looks, change only what is drawn on the window: off (the picture as it always was), sharp, scanlines, crt (a home television with a slot mask), pvm (a Sony
PVM or BVM monitor with an aperture grille; `filter=bvm` is taken as pvm) and curved. `filter=` in the configuration file picks the one to start with; F6 shows each one's name for a moment, with the key that changes
it. The last four are shaders by other authors, kept as they wrote them in `pc/shaders` (see its README.md), and need the OpenGL renderer; on another, those looks are left out with a
plain message. `filter=` can also name a player's own RetroArch `.slangp` preset, which runs through librashader when `librashader.dll` is beside the program; a library that is there but
that Windows will not load is told apart from a missing one, with Windows' error. See `pc/pc_filter.h`. The save goes to `paprium.sav` beside the configuration file (the program's own folder, or the
player's folder when the launcher names the file there with `--config`), in the Pocket's layout, when it changes and at quit. Each load checks the file the way the cart checks its save: in an 8 KB file each of the four places where the cart keeps its saves must be blank or hold one whole
copy, and one bit flipped in such a copy fails the file, as the tests find for each byte of four copies. A 4 KB file, the older layout, passes when it is not blank and holds none of the cart's copies; one
that holds them is an 8 KB save cut short and fails. A place blank in the file but whole in `paprium.sav.bak` fails too, as the cart never blanks a save it has written: to start on purpose
from a save that lacks one the backup has, delete `paprium.sav.bak` first. The first write of a run keeps the file as it was loaded as `paprium.sav.bak` when it checked good. After a start with no file, a blank one or one that
did not check, it keeps `paprium.sav.bak` as it is as `paprium.sav.bak.kept`, which only such a start writes, and a file that did not check as `paprium.sav.failed_check`: so the backup from
before such a start stays on disk however many good runs follow, until the next such start. A copy outside those four places is checked less closely, as its length is not known. See `pc/pc_save.h`. If the last write fails, the program tries again for about two seconds, then says so in a message box that names the file. Only a save file that does not exist starts a blank cart. A file the program cannot read, or one of the wrong size, stops it with a message and is left as it
was, because it may be a good save that another program holds for a moment. The program checks the game file against the hash it was built for, refuses any other, and writes nothing then.
Any pass of the run loop over 100 ms gets a line in `stalls.txt` beside the program, always, with no effect on the game: see `pc/pc_stall.h`. So does each switch of vsync, off or back on, with its
reason (the rule is in `pc/pc_pace.h`), and the sound's start, each time it runs dry with its time and frame, each pause and each skip. Closing the sound device after the
game's last frame, which took about 300 ms on one PC, is no stall: a look during it names the close, and a close longer than 100 ms gets one line saying how long it took.
Nor is the wait that follows, while the log writes its last lines: no look is taken then.
Vsync goes off only when the game, once behind its pace, does not catch up through the frames it shows, with no present counted for more than one refresh of the monitor: a monitor slower
than the game does that. In a model of the loop, bursts of slow presents never did on a 240 Hz monitor; on a 144 Hz one the densest still can: a present 15 to 40 ms longer
in every 3 frames, or 30 to 40 ms longer in every 4, or 40 ms longer in every 5. The switch's line gives the longest present and the longest frame without its present in that second.
Vsync comes back on after a calm spell with no slow frames that recur, which grows each time it has to go off again soon after, as it does when it comes back while a heavy
scene or a slow stretch is still going. Two things end those tries for the run, and the switch's line says which: a switch that takes more than 250 ms in the renderer, as some of
Direct3D 9's did on one PC, and vsync going off again on its first second back on with a monitor whose refresh, as the system gives it, is 58 Hz or less, clearly slower than the game.
A renderer that refuses a switch is not asked again either.
A gap still going on when the run ends gets its line too.
The sound's lines have an allowance of their own, so they never crowd out the freeze lines, and the lines about the sound's delay have a third, so they never crowd out the sound's
gaps; a line says when any allowance is used up.
Each run's block also says how long each part of the start-up took.

**The console the game finds.** Lines of `paprium_pc.cfg` (`pc/pc_console.h`) set it, each with a default that is the machine this program always was, a Japanese
Mega Drive with nothing attached: `console=` one of the Pocket core's Console list (`megadrive`, `genesis`, `megadrive2`, `genesis2`, `genesis3`, `nomad`, `megajet`,
`teradrive`, `unknown`), `megacd=` one of its Mega CD list (`none`, `megacd`, `megacd2`, `wondermega`, `wondermega2`, `laseractive`, `aiwa`, `cdx`, `unknown`),
`x32=off|on`, `cpumate=off|on` (with a unit the game knows) and `cpumate_brain=PATH`; an earlier version's `region=auto|us|jp`, `console=auto` and `megacd=off|on` keep
their meaning. `--console NAME=VALUE` gives the same to a headless run. The game reads its console once per boot: the version register's Japan bit, expansion bit and
version number, the video chip's PAL bit, TMSS (on a console of version 1, whether `$A14101` bit 0 changes the byte at `$000120`), the cart's model bits at `$1FE4`, a
TeraDrive's 128 KB video memory, the 32X's `$A130EC`, and the Mega CD unit's identification words. There is no Europe: a European console differs from one from the USA
only by running at 50 Hz, which this machine does not do (the team's decision: not needed for now), and `region=eu` is refused as any value the program does not
know. `pc/pc_addons.c` answers as the Pocket core's 0.3.0
detection stubs do, with its hardware rules (a unit that is a whole console is the console; the Genesis 3, the Nomad and the Mega Jet take no separate unit): the version
register, TMSS's check (no TMSS ROM and no licence screen), the 32X's "MARS", each Mega CD unit's identification (the words the game compares, no Sega program), its gate
array, its program memory's probe word and marker; `pc/pc_vdp.c` gives the TeraDrive's answer to the game's one 128 KB test, and the cart's `$1FE4` model bits go to the
firmware's option word. The Mega CD's link carries the CPU MATE partner's pad word as the Pocket's does: each update the game sends
raises the busy flag, the brain answers at once, and the word and the end of busy come at the moments the brain counts for the real sub-CPU; the sub-CPU's reset and the
game's bus request stop an update in flight, and the game's writes for the sub-CPU go into the brain's mirror. The brain (`pc/pc_brain.h`) is not part of this program
or this repository: it is read at start from Little-Man Firmware 1.0's file (`little_man_firmware.bin`, the firmware file of the Paprium core 0.3.0 for the
Analogue Pocket, which carries it; the players' download carries that file beside `Paprium.exe`, byte for byte as that release published it, see below),
checked part by part against its SHA-256, and run in an RV32IM interpreter of our own (`pc/pc_rv32.h`). Without it the link gives the stub's own answers and
a partner who joins stands still, as a firmware built without the brain does on the Pocket. With no `cpumate_brain` line the file is looked for in two places, in
this order (the team's decision): beside the program the player started, then beside the configuration file. For the players' program those are the folder
the player unzipped, beside `Paprium.exe` (the launcher starts `pc_port.exe` from its own folder), and the player's files folder, `%LOCALAPPDATA%\Paprium PC` or
`my_copy`; for the developers' `pc_port.exe` they are its own folder and the folder of `paprium_pc.cfg`, often one folder, looked at once. The first place where
anything of that name is there is used, even when it is the wrong file, cut short or unreadable: the window says so and names that place, and the other is not looked
at while the file is there. A `cpumate_brain` line names one place instead (a relative path starts from the configuration file's folder). The window and the menu name
the place in a few words ("beside Paprium.exe", "in %LOCALAPPDATA%\Paprium PC"), the console gives the whole path and why that place. The program only reads
the file. The players' program writes into the folder the player unzipped only inside `my_copy`, when `%LOCALAPPDATA%\Paprium PC` cannot be written (see below).
`tests/check_package.py` takes one firmware file in a package and no other: `little_man_firmware.bin` beside `Paprium.exe`, 256 KB, with the MD5 and the
SHA-256 its release published (`pc/pc_package.py`, `FIRMWARE_PINS`), whose runs it counts and does not judge; with `--firmware required` the package must hold
it. It refuses any other file of that name, in another folder or beside `Paprium.exe` with other bytes; a file named as one of the brain's three parts (`brain.bin`,
`cf.bin`, `rodata.bin`); a file that starts with the firmware file's first word; and, when the local configuration names a brain reference, a run of 16 bytes or
more equal to a part of the brain, as it is or with each 16-bit pair swapped, that the check's strength rule for the game's bytes calls strong. It also refuses an
archive (zip, 7z, RAR, gzip, xz, bzip2 or cabinet, known by its first bytes) unless it is byte-equal to one of the pinned third-party files, such as Python's
`python312.zip` and TinyCC's source archive, which it passes without looking inside. It does not undo other forms (compressed, encoded or reordered bytes). With every line at its
default nothing of this is reached: `pc_addons_on` stays 0 and the bus never asks there. The machine takes a change of these lines at its next reset (`pc_addons_reset`). The game reads them at boot, and
again after a Console Reset, which keeps the ones the run started with (the Pocket core answers that read from its menu as it is then).

See the first lines of `pc/pc_main.c` for the test switches and logs. `pc_clock.h` explains how time works, `pc_vdp.c` the video chip's model, `pc_cart.c` the firmware as the cart,
`pr_render.c` the renderer, `pc_window.h` the four shapes of the picture, and `pc_sound.h` the sound's way from the chips to the device. The sound never makes the game wait and nothing flows back from it,
so with no device, with a file, or with a stuck device the frames and the work RAM are the same. The audio device keeps three frames of sound and its own buffer queued, 60.7 ms with SDL's buffer of
512 samples. A frame that falls a whole game frame behind the pace is not shown (`pc/pc_pace.h`), so slow presents, or a monitor a little slower than the game, slow neither the game nor its sound:
the game keeps its own rate and a picture is left out now and then. Measured in hidden runs on one PC: a single present or frame of work of up to 30 ms, or six presents of 25 ms in a
row, cost no gap; one of 37 to 60 ms cost none or a gap of up to 33 ms; five seconds of 25 ms presents cost none, or one gap of 65 ms with a made-up 60 Hz monitor; and a longer stall stops
the sound for about its own length, less what the queue held. After such a stall the pace drops its time and makes up the last three frames at once, unshown but the last, so the sound comes
back within those frames' own work, on a 60 Hz monitor too (`pc_audio.c`). Once there has been no stall, no gap in the sound and no skip for 3 seconds, what the queue holds
over its 60.7 ms is let go at once, with a line in `stalls.txt`, so the sound keeps its delay after a stall or a holdup of the sound device's own thread. While stalls keep coming
the extra stays, as the cushion that keeps the next one from running the sound dry: a stall within a minute of a let-go doubles the wait for the next, up to 48 seconds. A slow frame
among frames on time, one that ends half a frame or more further behind the game's pace than the frame before it, counts as a stall here. A delay that stays more than a frame over
for 10 seconds gets a line of its own.
`sound_cores` in the local configuration names the folders of the three sound chips' cores, which `pc_build.py` pins by hash.
The game's own Options screen chooses the sound's path: the VM DAC row ticked is the YM2612's DAC, unticked is the cart's own DAC.
`pc_watch` in the local configuration lists block addresses whose first line and count go in the run's log; it is how a local tool can read the game's own load measure.

**The players' package.** A player must never need Python, a compiler or this repository, and no download may hold any part of the game. So

    python -I -B targets/pc/pc_package.py --config <local.json> --out FOLDER --version TEXT [--firmware-zip ZIP]

makes a folder that holds our programs and the tools to build the game, and nothing of the game. Beside `Paprium.exe` it puts Little-Man Firmware 1.0's file,
`little_man_firmware.bin`, so CPU MATE works with no other download: taken only from that release's own `Little-Man_Firmware_1.0.zip`, named by `--firmware-zip`
or by `firmware_zip` in the local configuration, after the zip's SHA-256 and the file's MD5 and SHA-256 matched the published values (`FIRMWARE_PINS`); with
neither, the script stops, and `--no-firmware` makes a package without the file, as the tests' packages are made. This repository never holds that file. A player starts `Paprium.exe` (`pc/pl_launcher.c`)
and drops their own ROM on its window. The launcher checks the file's SHA-256 against `tools/rules.json` (a file in the other byte order, or with a
copier's header, is put right in the copy it keeps), keeps a copy in the player's folder, and builds the game there: `tools/pc_boot_image.exe` lists
what the firmware's boot changes, `pc/pl_translate.py` runs pc-build's own translation on the package's Python, and TinyCC compiles the C into
`pc_game.dll`. The player's folder is `%LOCALAPPDATA%\Paprium PC`, the player's own app-data folder, so the folder a player unzipped never holds
the copy of the game file or the game built from it (the team's decision); it also holds the game's save, settings and freeze log, which the
launcher names to the game program. Only when that folder cannot be written does the launcher use `my_copy` beside itself, and the window and
`READ_ME.txt` say which. Then it starts `pc_port.exe`, built with `PT_GAME_FROM_DLL`: everything the developers' program holds but the generated code,
exporting the runtime's names the DLL imports (`pc_build.py`'s `GAME_DLL_DATA` and `GAME_DLL_FUNCTIONS`), and loading the DLL at start
(`pc/pc_game_dll.h`). A later start finds the DLL and its record, `build_record.txt`, and starts the game at once; a new package version, a
changed DLL or a build cut short is seen from the record, and the game is built again. The DLL plays byte for byte as the program `pc-build` links:
the frames, the cart log and the sound file of the same pads are equal. `pc_port.exe --interpreter` plays with no DLL at all, through the fallback;
its timing model is finer, so its logs are not byte-equal to the translated build's. When the game cannot be built, or Windows will not load the DLL,
the launcher starts the game that way and says plainly, with the reason, that this is not the tested build (the team's decision): in its
window's status line and a message box when the window is up, in a message box on a start from the cache. The Scum! cheats need the translated game's
switched code, so that way they are off: the Scum! menu shows them grey, and the window says so when the settings ask for them. A DLL that is not byte-equal to the tested
one is never started from the cache: every start builds the game again, so a fresh unzip after a damaged or changed package file puts it right,
and the player is told while the new build still differs. The launcher writes the settings file only when there is none. TinyCC is built
from its own source archive by `pc/pc_tinycc.py`, and the package carries that archive and the script, so the compiler and its source correspond
exactly. Our three programs go into the package without their symbol tables. The players' programs carry a manifest that makes their code page
UTF-8 and asks Windows for long paths (`pc/pc_utf8.manifest`), so a folder name with any letters works. `tools/rules.json` holds rule data: the ROM's hash, the hash of the
boot-patched image, the watched block addresses and a list of code addresses for discovery, and the hash of the DLL the package's maker built and
tested; none of it is any use without the player's own ROM. `tests/test_launcher.py` plays a player's cases on a package made for a made-up ROM,
and `tests/check_package.py` searches every file of a package for pieces of the ROM, and for CPU MATE's brain outside Little-Man Firmware 1.0's own file: by a
file's name and first word, and by its bytes when the local configuration names a brain reference (`brain_reference`, a copy of that file). The package's deepest file lies 97 characters
below its folder and Windows' limit is 259, so a folder path of more than 160 characters is written in the log, and a failure there tells the
player to move the folder. The build's deepest file lies 44 characters below the player's folder (a header of TinyCC's copied into the build
folder), so a player's folder of more than 215 characters, an app-data folder in a very long profile path, is written in the log too, and a
failure then names that folder as the cause.

Its tests run from `tests/test_all.py`: `tests/pc_machine_test.c` the machine alone on made-up data, `tests/pc_cart_test.c` the firmware on a made-up ROM, and
`tests/test_pc_machine.py` those two with one rule of the source broken at a time, `tests/pr_render_test.c` the renderer on made-up scenes with its planted faults, `tests/pc_config_test.c`
the configuration, the hash, the save file and the vsync rule, `tests/pc_stall_test.c` the freeze log alone, `tests/pc_window_test.c` the window, the keys and virtual game controllers on SDL's dummy video driver, `tests/pc_controls_test.c` the controls, made-up pads of any layout plugged in and out, the controller database and the controls screen driven by made-up keys and pads, `tests/pc_console_test.c` the
console's lines, the 32X's and the Mega CD's answers through the bus, the Mega CD's link with a made-up brain of a few RV32 instructions written there, and the RV32IM
interpreter, which `tests/test_pc_machine.py` also compares with unicorn's RISC-V on random programs, then the whole program built the same way on a made-up ROM, and on
another whose small program reads the console as the game does and sends the Mega CD one update. For the sound: `tests/pc_z80_test.c` the Z80's bus and clocks, `tests/pc_z80_cart_test.c` the Z80 against the firmware's turns, `tests/pc_mix_test.c` the mix against the Pocket's chain in integers, `tests/pc_audio_test.c`
the ring and the pull on made-up clocks, and `tests/pc_sound_join_test.c` each source reaching the sound file, with each connection broken in turn.
