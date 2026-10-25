#!/usr/bin/env python3
"""Prepare an engine-only browser site, without reading any original game data.

Also remove obsolete generated bundles and asset links from incremental builds.
Only these known build outputs are removed; symlinks are unlinked, never followed.
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

STATIC_FILES = ("local-files.js", "local-files-worker.js", "disc-image.js", "leaderboard.html")
LEGACY_OUTPUTS = ("vfs", "data", "FGPNativePOC.data", "preload.json")


def prepare(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name in LEGACY_OUTPUTS:
        path = out / name
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
    for name in STATIC_FILES:
        shutil.copy2(Path(__file__).with_name(name), out / name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.out)
    print("browser site: engine only; players select their own local game files")


if __name__ == "__main__":
    main()
