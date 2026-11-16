# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The program image the translator works from: an in-memory copy of the ROM.

Patches and overlays change this copy only. It is never written to disk.

An overlay describes code that the program copies into RAM at boot. The translator decodes those
bytes at the RAM address, reading them from their ROM source.
"""
import hashlib


class Image:
    def __init__(self, rom):
        self.bytes = bytearray(rom)
        self.size = len(rom)
        self.overlays = []      # (ram_base, source_address, length, why)
        self.patched = []       # (address, length, why)

    def r8(self, a):
        return self.bytes[self._off(a)]

    def r16(self, a):
        o = self._off(a)
        if o + 2 > self.size:
            raise IndexError(a)
        return (self.bytes[o] << 8) | self.bytes[o + 1]

    def r32(self, a):
        return (self.r16(a) << 16) | self.r16(a + 2)

    def _off(self, a):
        for base, src, length, _ in self.overlays:
            if base <= a < base + length:
                return src + (a - base)
        if a >= self.size:
            raise IndexError(a)
        return a

    def source_of(self, a):
        """For an address inside an overlay: where its bytes come from."""
        return self._off(a)

    def in_rom(self, a):
        return 0 <= a < self.size

    def in_overlay(self, a):
        return any(base <= a < base + length for base, _, length, _ in self.overlays)

    def readable(self, a):
        return self.in_rom(a) or self.in_overlay(a)

    def add_overlay(self, ram_base, source, length, why):
        for base, _, n, _ in self.overlays:
            if ram_base < base + n and base < ram_base + length:
                return False
        self.overlays.append((ram_base, source, length, why))
        return True

    def patch(self, address, data, why):
        """Change the in-memory copy. Used for the patch source and for the local test patches."""
        if address + len(data) > self.size:
            raise ValueError('patch outside the image')
        self.bytes[address:address + len(data)] = data
        self.patched.append((address, len(data), why))

    def window_sha256(self, address, length):
        return hashlib.sha256(bytes(self.bytes[address:address + length])).hexdigest()
