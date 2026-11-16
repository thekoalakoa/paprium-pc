# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The gate: the tool works on one ROM only, known by the SHA-256 of the whole file.

The expected hash lives in the local configuration, never in this repository. The gate runs
before anything else, and on a mismatch nothing at all is written.
"""
import hashlib
import json
import os


class GateRefused(Exception):
    pass


REFUSAL = 'This is not the ROM this tool knows. Nothing was written.'


def load_config(path=None):
    """Read the local configuration. See config.example.json for its fields."""
    path = path or os.environ.get('PAPRIUM_PC_CONFIG')
    if not path:
        raise GateRefused('No local configuration: pass --config or set PAPRIUM_PC_CONFIG.')
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def check_rom(cfg):
    """Return the ROM bytes when the file matches the configured hash. Raise GateRefused otherwise."""
    want = (cfg.get('rom_sha256') or '').strip().lower()
    if len(want) != 64:
        raise GateRefused('The local configuration has no ROM hash. Nothing was written.')
    try:
        with open(cfg['rom_path'], 'rb') as f:
            rom = f.read()
    except (KeyError, OSError):
        raise GateRefused('Cannot read the ROM file named in the local configuration. Nothing was written.')
    if hashlib.sha256(rom).hexdigest() != want:
        raise GateRefused(REFUSAL)
    return rom


def out_dir(cfg):
    """The folder for everything the tool writes. It must exist already and lie outside the repository."""
    out = os.path.abspath(cfg['out_dir'])
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.path.normcase(out + os.sep).startswith(os.path.normcase(repo + os.sep)):
        raise GateRefused('out_dir must lie outside the repository.')
    os.makedirs(out, exist_ok=True)
    return out
