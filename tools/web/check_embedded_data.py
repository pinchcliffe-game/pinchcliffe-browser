#!/usr/bin/env python3
"""Check an engine binary for known original content, using a local executable.

No reference payload is stored here. This supplements the release file allowlist;
it is a regression check for recovered content, not a legal clearance assessment.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct


class Image:
    def __init__(self, data: bytes):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("Expected the player's PE executable")
        pe = struct.unpack_from("<I", data, 60)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("Invalid PE signature")
        self.base = struct.unpack_from("<I", data, pe + 52)[0]
        count = struct.unpack_from("<H", data, pe + 6)[0]
        start = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
        self.sections = [struct.unpack_from("<4I", data, start + 40*i + 8)
                         for i in range(count)]

    def read(self, address: int, size: int) -> bytes:
        for _, rva, length, offset in self.sections:
            delta = address - self.base - rva
            if 0 <= delta <= length and size <= length - delta:
                result = self.data[offset + delta:offset + delta + size]
                if len(result) == size:
                    return result
        raise ValueError(f"Content range {address:#x} is not file-backed")


def content_blocks(image: Image):
    # Original data locations, never the values stored at those locations.
    for name, address, size in (
        ("Bee Maze maps", 0x4cd7e0, 5*352*4),
        ("Pipeline maps", 0x49ece8, 5*80),
        ("Mahjong layout", 0x4e6b10, 0x141),
        ("Puzzle cells", 0x49f26c, 72*12),
        ("Bouquet placements", 0x49f7b0, 22*5*8),
        ("Math Lab levels", 0x5120b0, 23*36),
        ("Bugzzz levels", 0x4d7210, 9*48),
        ("Hopscotch levels", 0x49e788, 16*24),
        ("Audio Pairs placements", 0x4cb720, 20*8),
        ("Mountain outlines", 0x4dbf60, 48*4),
    ):
        yield name, image.read(address, size)
    # Earlier builds encoded these maps as decimal strings.
    maps = struct.unpack("<1760I", image.read(0x4cd7e0, 1760*4))
    yield "Bee Maze text maps", "".join(map(str, maps)).encode("ascii")
    yield "Pipeline text maps", bytes(v + ord("0") for v in image.read(0x49ece8, 400))
    for address in (0x4e6fa8, 0x4ecfb8, 0x4effc0, 0x4f2fc8,
                    0x4f5fd0, 0x4fbfe0, 0x4fefe8):
        vertices, faces = struct.unpack("<hh", image.read(address, 4))
        if not (1 <= vertices <= 256 and 1 <= faces <= 256):
            raise ValueError("Unexpected original mesh counts")
        yield f"Puzzle mesh {address:#x}", image.read(address + 8, vertices*24)
        yield f"Puzzle faces {address:#x}", image.read(address + 0x1808, faces*24)


def find_embedded(engine: bytes, blocks) -> list[str]:
    found = []
    for name, payload in blocks:
        # Avoid matching generic zero-fill or short mathematical constants.
        for offset in range(0, len(payload) - 63, 16):
            signature = payload[offset:offset + 64]
            if len(set(signature)) >= 4 and signature in engine:
                found.append(name)
                break
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    try:
        found = find_embedded(args.engine.read_bytes(),
                              content_blocks(Image(args.executable.read_bytes())))
        if found:
            raise ValueError("Embedded original content: " + ", ".join(found))
    except (ValueError, struct.error) as error:
        parser.exit(1, f"Publication refused: {error}\n")
    print("No known original meshes, maps, levels or placement payloads in engine")


if __name__ == "__main__":
    main()
