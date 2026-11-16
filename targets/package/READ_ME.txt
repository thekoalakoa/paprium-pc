# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
# The players' package: READ_ME.txt, written by targets/pc/pc_package.py. @VERSION@ is the package's version.
#---- everything below this line goes into the package
Paprium on PC, version @VERSION@

WHAT THIS IS

A program that plays Paprium on a Windows PC from your own copy of the game file (the ROM).
This download holds no part of the game. The first time you start it, the program checks your
game file and builds the game from it on your PC, and keeps what it built in your own app-data
folder (see YOUR FILES). The download also carries little_man_firmware.bin, Little-Man
Firmware 1.0, for the CPU MATE partner (see THE CONSOLE, THE 32X, THE MEGA CD AND CPU MATE).
The program was written by our team from our own study of how the cart behaves.

WHAT IT NEEDS

Your own copy of the Paprium game file (the ROM), legally obtained. This download does not come
with it.

64-bit Windows 10 (version 1903 or later) or Windows 11. It has been tried on one Windows 11 PC
so far.

TO PLAY

1. Unzip the whole download into a folder: your Desktop, your Documents or a games folder.
   Nothing is installed, and the program keeps its settings in a file, not in the Windows
   registry. Keep the folder's path short, at most 160 characters: below a longer one, Windows
   may not reach the program's deepest files, and if the build then fails, the program says to
   move the folder.
2. Start Paprium.exe. The program is not signed yet, so Windows may show "Windows protected your
   PC". Click "More info", then "Run anyway". When Windows 11's Smart App Control is on, it
   blocks programs that are not signed and offers no "Run anyway"; the program cannot start
   there until it is signed.
3. Drop your own copy of the Paprium game file on the window, or click "Choose the game file".
4. The first time, the program checks the file and builds the game: about 10 to 20 seconds, with
   a progress bar. Then the game starts. Every later start goes straight to the game.

A file that is not the Paprium game file is refused with the reason, and nothing is changed. A
zip file has to be unzipped first.

If the game cannot be built on your PC, or Windows will not load the game that was built, the
program plays it through its own interpreter instead, which needs nothing built. A message
from the program says so, with the reason, before the game starts. That way plays the same
game, but it is not the build we tested: small things such as timing may differ. The Scum!
cheats need the built game, so that way they are off and grey in the menu, and an arcade stage
chosen in the menu takes effect only from the next Soft Reset. The next start tries the build
again.

KEYS

Player 1: the arrow keys, Z X C for A B C, A S D for X Y Z, Enter for Start, Right Shift for
Mode. Player 2: I J K L, 1 2 3, 4 5 6, Keypad Enter and Keypad 0. Game controllers work at once.
Player 3 has no keys: by default the third game controller (see THREE PLAYERS).
F1 switches the sprite limits between the cart's and none, F2 goes through the picture's shapes
(see ASPECT RATIO), F3 mutes the sound, F4 opens the controls screen, F6 changes the picture
filter, F10 opens the menu bar, P pauses, F11 is full screen, Esc quits.

THE MENU BAR

The bar at the top of the window holds every one of those options, each with its key beside it,
so you can learn the keys from it. Click a title, or press F10, then use the arrows and Enter, or
click an item; Esc closes it. The game pauses while a menu is open. In full screen the bar is
hidden: move the mouse to the top edge, or press F10. F10 is one of the program's keys, so the
controls screen can move it too. The menus:

  Game      Pause, Soft Reset, Console Reset, Quit.
  Video     the six filters and Next filter, Full screen, the four Aspect Ratio shapes and Next
            aspect ratio, the cart's sprite limits, the menu bar's own strip in a window, and the
            renderer and vsync, which take effect from the next start.
  Sound     Mute, and the volume: 100, 75, 50 or 25 percent.
  Controls  the controls screen, and the menu bar's key.
  Scum!     Health, Pills, Lives, Arcade Mode, Arcade Coins, Attract Only, Arcade stage,
            6-Button for each player, 3-Player Mode, and Soft Reset (below).
  Console   Console, Mega CD, 32X, CPU MATE, and Soft Reset (below).
  Help      Keys, a page of every key as you have them now, and About.

What you change in the menus is written into paprium_pc.cfg, in your files' folder (see YOUR
FILES), and the other lines of that file stay as they are. Pause, Next filter, Next aspect ratio,
Full screen, the sprite limits and Mute are not written: they do what their keys do, for
this run only. If paprium_pc.cfg cannot be written (it is read-only, say, or another program
holds it), the window says so with the change, and a Soft Reset or the next start goes without
that change. In the smallest window (scale=1) a long list shows in two columns, and a Help page
longer than the window shows in parts: any key or a click goes on to the next part.

Scum! is the PC's form of the Pocket core's menu of the same name. Health: players take no
damage, and a player's throws do no damage to enemies. Pills: using a Blu pill (Down and A) does
not spend one. Lives: dying does not spend a life. They work at once, in any mix; on the Pocket
the same three are one list, Little-Man, with each mix as an entry. Arcade Mode, Arcade Coins,
Attract Only and 3-Player Mode have the Pocket core's names, order and choices; they and the
arcade stage are in ARCADE MODE AND THREE PLAYERS. 6-Button sets a pad to three or six buttons,
with one entry for each player, and every player starts with a six-button pad. The Pocket core
has one 6-Button switch for all players instead, and it starts off: three-button pads for
players 1, 2 and 3, and X, Y, Z and Mode of the pad in the stick's EXT socket not passed on. To
play as on a Pocket with it off, untick 6-Button for each player.

Console picks what the game finds when it starts: the console, from the Pocket core's list,
whose name carries its region; the Mega CD unit, from the Pocket core's list too; a 32X; and CPU
MATE, the game's computer partner, which needs a Mega CD unit the game knows and its brain (see
THE CONSOLE, THE 32X, THE MEGA CD AND CPU MATE). Where a real console would change a choice, a
grey line in the menu says so. The game reads these only when it starts, so use Soft Reset after
a change.

Soft Reset, in the Game, Scum! and Console menus, starts the game again from power-on, with your
save and the settings in paprium_pc.cfg as they are now.

Console Reset, in the Game menu, is the console's own reset button, as the Pocket core's Console
Reset: the console's processor starts again from the cart's warm start, while the video chip, the
work memory, the cart and your save run on, so the game itself decides what follows, as it does
on the cart. Later in the game it starts again at its terminal screen. Pressed in the game's
first seconds after power-on, or during the first-start mini game, it leaves the game stuck on
the screen it shows: the game's own check of its warm start does that. Pressed while the game
zooms a picture (as in its opening), it can lock the game up. Both are as on the cart, and after
a lock-up the cart's chip may stop itself a few seconds later, as on the cart. Once the chip has
stopped (Inf. Credits stops it too), a Console Reset only holds the console. Soft Reset starts
the game again from any of these. The settings that need a Soft Reset still do. One difference
from the Pocket: there, a Console Reset lets the game find the Console, Mega CD and 32X the menu
holds now; here the game finds the ones it started with, so use Soft Reset after changing them.

F4 opens the controls screen over the paused game: every key and pad button for each player,
which pad is whose, for players 1, 2 and 3 (USB or Bluetooth, as Windows sees them), three or six
buttons, the analog stick and its dead zone, and on its Program keys page the program's own keys,
the filter key among them. Pick a line, press Enter, then press the key or the pad button you
want. When the screen closes, what you changed is saved in paprium_pc.cfg, in your files' folder
(see YOUR FILES); the other lines of that file stay. Pads that SDL, the library the program uses
for them, does not know itself are looked up in gamecontrollerdb.txt, beside Paprium.exe.

PICTURE FILTERS

F6 goes through the six looks, and shows each one's name for a moment with the key that changes
it: off (the picture as it always was), sharp, scanlines, crt (a home television), pvm (a Sony
PVM or BVM monitor) and curved. The controls screen can move the filter to another key. To start
with a look, put a line such as filter=pvm in paprium_pc.cfg, in your files' folder; filter=bvm
gives the same look. A filter changes only what you see: the game itself plays the same with any
of them. All but off and sharp need the OpenGL renderer, the one the program uses unless
paprium_pc.cfg says otherwise. Those four are drawn with three shaders from libretro's
collection, built into the program; their sources and notices are in the folder source\shaders
and in NOTICES.txt.

A RetroArch shader preset of your own, a .slangp file, can be used too, through librashader, a
library that is not in this download: put librashader.dll, from its 64-bit Windows build, beside
Paprium.exe, and a line such as filter=C:\Shaders\my look.slangp in paprium_pc.cfg. A preset
needs the OpenGL renderer too. When the program cannot use the library or the preset, it says
why on the window and draws the picture as with off.

ASPECT RATIO

The Video menu's Aspect Ratio has the Pocket core's two shapes, then the program's own two:
Original, the shape the Pocket gives the picture (64:49 for its 224 lines, a 320 pixel line and
the 256 pixel line of the first-start mini game alike); Corrected, square pixels for a 320 pixel
line, while a 256 pixel line keeps Original's shape, as on the Pocket; 4:3, the shape a
television gives it; and square pixels, every pixel square, a 256 pixel line narrower in the
middle. 4:3 is the default here, where the Pocket starts on Original. The menu writes the line
display= original, corrected, 4:3 or square; F2 goes through the four for this run only. The
filters draw in every shape.

ARCADE MODE AND THREE PLAYERS

Arcade Mode, Arcade Coins, Attract Only and 3-Player Mode are the Pocket core's entries, with its
names and choices. The Pocket core picks the arcade stage from two lists, Arcade Stage Select
and Arcade Stage Select 2; here they are one list, Arcade stage. The game looks for its arcade
stick and for a multi-player adapter only when it starts, so use Soft Reset after a change.

  Arcade Mode   Off: two pads, the game as it always was. On: the game's own arcade stick, the
                Grand Stick III, in port 1 with its ARCADE switch on, so the game runs its hidden
                ARCADE PAPRIUM mode; player 1's Mode button (Right Shift) puts in a coin, and the
                game takes no credit for the first coin after it starts, as on the cart.
                Multiplayer: On with the stick's 3 PLAYERS switch, player 3 in the stick's EXT
                socket. Stick Only: the stick with every switch off, the normal game, its Mode
                button its own. With On or Multiplayer, the game leaves ORIGINAL and ARENA out
                of its menu: that is the game's rule.
  Arcade Coins  1 Coin 1 Credit, 2 Coins 1 Credit, or Inf. Credits (0012): with that one the game
                stops on its red 0012 screen at start, as with the real stick's switch, and a few
                seconds later the cart's chip stops itself too, as on the cart. To play, pick
                another Arcade Coins, then Soft Reset.
  Attract Only  the attract demo, and no game.
  Arcade stage  the stage the arcade game starts on, the Arena among them; with none, the game's
                own first stage. The list only picks the stage, as on the Pocket: its Off is the
                game's own first stage, and Arcade Mode alone turns the arcade mode on or off
                (the menu's grey line under the stage says so). A stage chosen while the game
                runs is taken for its next arcade game, as on the Pocket, and Arcade Mode counts
                for the stage at once too: after a change to Off or Stick Only, the next arcade
                game starts on the game's own first stage. When the game plays through the
                program's interpreter (see TO PLAY), both wait for the next Soft Reset.
  3-Player Mode with Arcade Mode Off, an EA 4-Way Play on the two ports, players 1, 2 and 3 in
                its sockets, so the game's own ARENA takes three players (once the Arena is
                open); with the stick, player 3 in its EXT socket.

Arcade Coins, Attract Only and Arcade stage act only with Arcade Mode On or Multiplayer. The
menus write these lines in paprium_pc.cfg:

  arcade_mode=    off (the default), on, multiplayer or stick_only
  arcade_coins=   1coin (the default), 2coins or infinite
  attract_only=   off or on
  arcade_stage=   off or 1 to 25 (25 is the Arena)
  three_players=  off or on
  pad3=           six or three, as pad1 and pad2

A paprium_pc.cfg from an earlier version with an arcade_stage line and no arcade_mode line
starts Arcade Mode On at that stage, as it did there. Off in the stage list then keeps Arcade
Mode On; to end it, pick Off in Arcade Mode.

THREE PLAYERS

Player 3 plays only through the EA 4-Way Play or the stick's EXT socket. Their pad is the third
game controller, the first one players 1 and 2 do not have, and they have no keys; the controls
screen's Player 3 page can give them keys or another pad. With neither a pad nor a key player 3
is not plugged in: the game finds no third pad, and with Multiplayer its third place stays
empty. Players 1 and 2 keep their keys. Who comes first for pads depends on whether the game
hears player 3. With 3-Player Mode on, or Arcade Mode Multiplayer, it does, and no player takes
another's pad: a pad of the kind player 3 asked for on the controls screen stays theirs. When
there are fewer pads than players, that pad still goes to player 3 first: with two pads, both of
the kind player 3 asked for, player 3 has one, player 1 the other and player 2 none until a
third pad comes. Otherwise players 1 and 2 come first: when one of them has no pad, they take a
free one, or else player 3's, even one of the kind player 3 asked for, so they get the same pads
as with no player 3. A pad pulled out for a moment (a cable, a pad that went to sleep) goes back
to the player who had it when it comes back, and a pad another player used meanwhile goes back
to its own player too, so no two players end up with each other's pads. Two pads of the same
model are told apart only by a serial number, which some pads do not give; then the first pad of
that model plugged in after the pull counts as the one that came back.

Rules of the game itself, as on the Pocket: with Arcade Mode On (two player places), the game
takes the pad in the stick's EXT socket as a second pad of player 1's; with Stick Only, the
normal game, its own ARENA takes the pad in the EXT socket as player 3 (once the Arena is open),
as it does with the EA 4-Way Play; and with 3-Player Mode and Arcade Mode Off, the first-start
mini game listens only to player 3's socket, so with no player 3 it gets no input.

THE CONSOLE, THE 32X, THE MEGA CD AND CPU MATE

These lines in paprium_pc.cfg, in your files' folder, set the console the game finds. The game
looks at them only when it starts, so change them, then start the game again. The Console menu
writes them for you. Its two lists are the Pocket core's, with the same names in the same order.

  console=   megadrive    Mega Drive (Japan), the default
             genesis      Genesis (USA)
             megadrive2   Mega Drive 2 (Japan)
             genesis2     Genesis Model 2 (USA)
             genesis3     Genesis 3 (USA)
             nomad        Nomad (USA)
             megajet      Mega Jet (Japan)
             teradrive    TeraDrive (Japan)
             unknown      Unknown (Japan): a console the game does not trust
  megacd=    none         no Mega CD, the default
             megacd       Mega-CD / Sega CD
             megacd2      Mega-CD 2 / Sega CD 2
             wondermega   Wondermega (Japan)
             wondermega2  Wondermega M2 / X'Eye
             laseractive  LaserActive
             aiwa         Aiwa CSD-GM1 (Japan)
             cdx          CDX (USA)
             unknown      Unknown CD: a unit the game does not know
  x32=on                  a 32X: some things you can throw become 32X boxes
  cpumate=on              the CPU MATE partner plays by itself, with any Mega CD unit but none
                          and unknown

With none of these lines the game finds what it always found here: a Japanese Mega Drive with
nothing attached. The game tells the consoles apart by their region, by whether they have TMSS
(the check later consoles make before a game starts), by the cart's own word that names the
model, and the TeraDrive by its larger video memory; the Mega CD units by the letters and digits
of their BIOS. The program answers those checks as the Pocket core does. Nothing of a console's
TMSS, the 32X or the Mega CD is in the program, only the answers to the game's own checks: there
is no licence screen. As on the real hardware, the Wondermega, Wondermega M2 / X'Eye,
LaserActive, Aiwa and CDX are a whole console in one, so the game finds them and not the console
line (the Wondermega M2 / X'Eye and the LaserActive take the console line's region); the Genesis
3, the Nomad and the Mega Jet have no port for a separate Mega CD (megacd, megacd2, unknown), so
the Mega CD is off with them. Lines from an earlier version still work: region=us or jp with
console=auto, genesis or megadrive picks the Genesis or the Mega Drive, and megacd=on is the
Mega-CD / Sega CD. There is no Europe.

This download carries the file little_man_firmware.bin beside Paprium.exe: Little-Man Firmware
1.0, the firmware of the Paprium core 0.3.0 for the Analogue Pocket, byte for byte the public
file of that core's 0.3.0 release (github.com/thekoalakoa/paprium-pocket), MD5
f8cda67209d0e4d35beb077ae88fb16b, SHA-256
6fb90204322c6a5596bfb4d3a743696cbdced5744cf8d4b9f09aa9fc322873e2. It carries the CPU MATE
partner's brain, a reverse-engineered emulation of the game's CPU partner, so CPU MATE needs no
other download: in the Console menu pick a Mega CD unit the game knows (any but None and Unknown
CD), tick CPU MATE and use Soft Reset. The program looks for little_man_firmware.bin in two
places: beside Paprium.exe first, then in your files' folder, %LOCALAPPDATA%\Paprium PC, beside
paprium_pc.cfg (in my_copy when your files are kept there, see YOUR FILES), and uses the first
little_man_firmware.bin it finds there. If that file is the wrong one, cut short or cannot be
read, the window says so and names its place, and the other place is not looked at while that
file is there. A line such as cpumate_brain=C:\Downloads\little_man_firmware.bin in
paprium_pc.cfg names any other place instead; a short path such as
brains\little_man_firmware.bin starts from the folder of paprium_pc.cfg. The program only reads
that file: it takes only the brain from it, checks it, and uses nothing else of it. For a few
seconds after the start the window says which file it used, for example little_man_firmware.bin
beside Paprium.exe. Without the file, the game still offers CPU MATE and a partner who joins
stands still, and the window says so and where it looked: no little_man_firmware.bin beside
Paprium.exe or in %LOCALAPPDATA%\Paprium PC. A file put in either place while the game runs is
found at the next Soft Reset. Little-Man Firmware is free software under the GNU General Public
License, version 3 or later: NOTICES.txt says where its source is, and its own notices are in
licences\THIRD_PARTY_NOTICES_firmware.md.

To bring the partner in, switch CPU MATE on in the game's Options; then at the character select
press Start on player 2's controls to add the partner, press it again to pick its character, and
pick your own. With one six-button pad, hold Mode and press Start to add the partner, still
holding Mode press Start again to pick its character, then let go of Mode and pick your own: that
is the game's own way with a Mega CD. The partner then plays by itself. Using player 2's controls
takes over from it for a while.

YOUR FILES

The program keeps your copy of the game file, the game built from it, your save, your settings
and its logs in a folder of your own, your app-data folder:

  %LOCALAPPDATA%\Paprium PC

To open it, type that line into the address bar of a File Explorer window. The program never
writes into its own folder, the one you unzipped, so copying or zipping that folder never takes
your copy of the game file or the game built from it along. It does read there the
little_man_firmware.bin the download carries (see THE CONSOLE, THE 32X, THE MEGA CD AND CPU
MATE), and the files of yours that you put beside Paprium.exe yourself: for a filter of your
own, librashader.dll and a preset given by a short path such as filter=my look.slangp, which
starts from that folder (see PICTURE FILTERS). Those files go along when the folder is copied or
zipped. Your copy of the game file and the game built from it are yours alone: never share them
and never sell them.

Only when your app-data folder cannot be written does the program keep the same files in the
folder my_copy, beside Paprium.exe, instead. The window says which folder is used, and so does
launcher_log.txt.

To update, unzip a new version and start it: your files stay where they are, and the game is
built again for the new version. This version reads the paprium_pc.cfg of every earlier version
and keeps its meaning.

To remove everything, delete the program's folder and the
folder %LOCALAPPDATA%\Paprium PC.

YOUR SAVE AND SETTINGS

The game keeps its save as paprium.sav in your files' folder, in the same layout as the
Pocket's. To play on with a save from the Pocket or from an earlier PC build, close the game and
copy that file there as paprium.sav. Keys and other settings go in paprium_pc.cfg in the same
folder: the program writes that file the first time, with the line that names the save, the
controls screen (F4) writes its keys and pads there, and the menus their settings. It is a plain
text file, and the game reads it when it starts; when you edit it yourself, save it as UTF-8 or
plain text, not as UTF-16 (Notepad's Unicode). An earlier version of this program may refuse to
start with a paprium_pc.cfg that the menus or the controls screen have written new lines into:
keep a copy of the file before you go back to one.

IF SOMETHING GOES WRONG

Send us the text files from your files' folder: launcher_log.txt, build_log.txt, game_log.txt
and stalls.txt. Never send the game file or pc_game.dll. The text files show the names of
folders on your PC, which can include your Windows user name, and stalls.txt also names your
PC's sound device, monitor and graphics card.

THE TERMS

As given, word for word:

  "gpl would apply to everything but the games rom itself the output your tool makes would still
  be partially their ip and cannot be sold."

  "sharing output is forbidden and not allowed just like sharing a rom"

What the program builds on your PC from your game file is, in the words given,
"a Patch copy of the Legally obtained rom".

About the name Paprium, as given: "there is no affiliation and no commercial use."

The program is free software under the GNU General Public License, version 3 or (at your option)
any later version, and comes with no warranty. The game is not ours and is not under that
licence. See NOTICES.txt for the licences of the program and of the tools it carries.
