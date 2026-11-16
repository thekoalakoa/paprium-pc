/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cart's battery save as a file: 8 KB, in the Pocket's .sav layout, where the firmware's byte i is the file's byte i ^ 1. So a player's Pocket save works as a copy.
 *
 * THE BACKUP. Each load checks the file the way the cart checks its save (pc_save_check). The first time a run replaces the file, before it does:
 *   - a file that checked good is kept as it was loaded, at the same name with .bak added. So .bak only ever receives a save that loaded and checked good.
 *   - after any other start, with no file, a blank one or one that did not check, .bak is left alone, and a copy of it as it is now goes to .bak.kept. The save this run writes checks good at the
 *     next run, and that run's first replacement puts it in .bak: the copy in .bak.kept is what keeps the backup from before such a start. Only a run that started without a good save writes
 *     .bak.kept, so later good runs never touch it.
 *   - a file that did not check also goes to .failed_check. The game cannot tell a good save in another byte order from a blank one, and the first thing it does with a blank one is write over
 *     it: this copy is what is left of such a file. A blank file, every byte 0x00 or every byte 0xFF, is not kept.
 * So a save that does not check, or a missing one, leaves the last good backup on disk: in .bak through that run, and in .bak.kept from then on, however many good runs follow, until the next
 * time a run starts without a good save, which keeps the .bak of that moment there instead. Besides damage inside a copy, two things make a file not check: a 4 KB file that holds the cart's
 * copies, which is an 8 KB save cut short, and a place blank in the file but whole in .bak, a save the file has lost. If a save that lacks one the backup has is put there on purpose, delete
 * .bak first, or it does not check until the game writes that save again: each run till then keeps .bak as it is. tests/pc_config_test.c runs these routes on made-up saves, a bit
 * flipped in each of the four copies of a save among them, and tests/test_pc_machine.py runs them in the program. What the check cannot see is in pc_save.c, above pc_save_check.
 */
#ifndef PC_SAVE_H
#define PC_SAVE_H

#include <stddef.h>
#include <stdint.h>

#define PC_SAVE_BYTES 0x2000u
#define PC_SAVE_BYTES_OLD 0x1000u       /* an older layout, which the firmware imports once */

#define PC_SAVE_BACKUP ".bak"
#define PC_SAVE_BACKUP_KEPT ".bak.kept"
#define PC_SAVE_KEPT_APART ".failed_check"

/* What pc_save_check makes of a file's bytes, in the firmware's byte order. */
#define PC_SAVE_GOOD 1                  /* at least one copy in the cart's format and none that fails the cart's check, or a file in the older layout that is not blank and holds no copy */
#define PC_SAVE_NOT_A_SAVE 2            /* a copy that fails the cart's check, or no copy at all: damaged, something else, or a save in the other byte order */
#define PC_SAVE_BLANK 3                 /* every byte 0x00 or every byte 0xFF, as a new cart */
int pc_save_check(const uint8_t *firmware_bytes, size_t file_bytes);

/* What pc_save_load returns when it hands back no save. Only the first of them means a new cart. A file that is there and could not be read may be a good save that a backup or a cloud
   client holds for a moment, so taking it as blank would write a blank cart's save over it as soon as the game saves. */
#define PC_SAVE_NO_FILE (-1L)           /* the folder is there, and no file has that name */
#define PC_SAVE_UNREADABLE (-2L)        /* anything else that stopped the read: the file held by another program, the folder or the drive gone */
#define PC_SAVE_WRONG_SIZE (-3L)        /* read to its end, and it is neither of the two sizes a save can be: empty, cut short, or not a save */

/* Reads the file into firmware layout and returns its size. A file in the older layout reads as far as it goes, and the rest of the 8 KB is 0, as the cart's RAM powers up in the whole-core
   simulation that gave such a file. On any of the answers above firmware_bytes is left as it was. */
long pc_save_load(const char *path, uint8_t *firmware_bytes);

/* Writes the firmware's bytes to the file through a temporary name and a rename, so a kill mid-write leaves the old file whole. The rename happens only after the whole temporary file has
   reached the disk: a full disk leaves the old file whole too. Returns 0, or -1 on failure. */
int pc_save_store(const char *path, const uint8_t *firmware_bytes);

/* pc_save_store for a player's save: first, once a run, the copies above, path.bak, or path.bak.kept and path.failed_check. If a copy cannot be made nothing is replaced, the newest bytes go to
   path.tmp, and it returns -1, so the next try can still make the copy. */
int pc_save_replace(const char *path, const uint8_t *firmware_bytes);

/* path.bak is there. */
int pc_save_backup_there(const char *path);

/* What the check made of the last file loaded: PC_SAVE_GOOD, PC_SAVE_NOT_A_SAVE or PC_SAVE_BLANK. pc_save_loaded_check also takes path.bak, when it is there and can be read: a file that
   checks good on its own but has a place blank that holds a whole copy in path.bak does not check. pc_save_loaded_why: a sentence saying why it did not check, when there is more to say than
   that a copy failed, else empty. */
int pc_save_loaded_check(void);
const char *pc_save_loaded_why(void);

/* The number of the first save, 1 to 4, whose place is blank in fw but holds a whole copy in backup_fw, both 8 KB in the firmware's byte order; 0 when there is none. */
int pc_save_blanked(const uint8_t *fw, const uint8_t *backup_fw);

/* After a failed store or replace. pc_save_newest_in_temp: the newest bytes are whole in path.tmp. pc_save_copy_failed: it was a copy that failed: of path + pc_save_copy_source, to path +
   pc_save_copy_ending. */
int pc_save_newest_in_temp(void);
int pc_save_copy_failed(void);
const char *pc_save_copy_ending(void);
const char *pc_save_copy_source(void);

/* The save already holds the newest bytes again, so what a failed store left in path.tmp is out of date: it goes. */
void pc_save_forget_temp(const char *path);

#endif /* PC_SAVE_H */
