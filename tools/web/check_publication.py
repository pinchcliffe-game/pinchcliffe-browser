#!/usr/bin/env python3
"""Fail closed if a staged browser release could publish original game files."""
from __future__ import annotations

import argparse
from pathlib import Path
from html.parser import HTMLParser

from serve import PUBLIC_FILES

REQUIRED = PUBLIC_FILES - {"REVISION", "index.html"}


class ImportPage(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.picker = False
        self.reader = False

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        self.picker |= tag == "input" and values.get("id") == "game-folder"
        self.reader |= tag == "script" and values.get("src") == "local-files.js"


def check(site: Path) -> None:
    found = {path.name for path in site.iterdir()}
    if found - PUBLIC_FILES:
        raise ValueError(f"Unexpected public files: {sorted(found - PUBLIC_FILES)}")
    if REQUIRED - found:
        raise ValueError(f"Missing engine files: {sorted(REQUIRED - found)}")
    for path in site.iterdir():
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Not a regular release file: {path.name}")
    script = (site / "FGPNativePOC.js").read_text(encoding="utf-8")
    if "FGPNativePOC.data" in script or "loadPackage(" in script:
        raise ValueError("Engine still contains an Emscripten file preload")
    for name in ("FGPNativePOC.html", "index.html"):
        if (site / name).exists():
            page = ImportPage()
            page.feed((site / name).read_text(encoding="utf-8"))
            if not page.picker or not page.reader:
                raise ValueError(f"Missing local import flow: {name}")
    if (site / "FGPNativePOC.wasm").read_bytes()[:4] != b"\0asm":
        raise ValueError("Invalid engine WebAssembly")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("site", type=Path)
    args = parser.parse_args()
    try:
        check(args.site)
    except ValueError as error:
        parser.exit(1, f"Publication refused: {error}\n")
    print("Publication checked: engine-only files, local importer, no preload bundle")


if __name__ == "__main__":
    main()
