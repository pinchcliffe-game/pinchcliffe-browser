#!/usr/bin/env python3
"""Serve the engine-only browser build and shared leaderboard locally.

Only the authored site files and compiled engine are served. Old data bundles,
package links and arbitrary files in an incremental build directory are denied.

    python3 tools/web/serve.py build-web --port 8765
"""

from __future__ import annotations

import argparse
import functools
import http.server
import json
import os
import re
import tempfile
import threading
import time
from urllib.parse import unquote, urlsplit
from datetime import datetime
from pathlib import Path

PUBLIC_FILES = frozenset({
    "FGPNativePOC.html", "index.html", "FGPNativePOC.js", "FGPNativePOC.wasm",
    "local-files.js", "local-files-worker.js", "disc-image.js", "leaderboard.html", "REVISION",
})

RANGE = re.compile(r"bytes=(\d*)-(\d*)$")
SCENE = re.compile(r"^[a-z0-9]{1,24}$")
MAX_SCORE = 100_000_000
MAX_PLAYER = 32
MAX_TITLE = 64
KEEP_PER_GAME = 50
MAX_BODY = 4096
DEFAULT_SCORES = Path.home() / ".pinchcliffe" / "leaderboard.json"


class Leaderboard:
    """Best scores per game, in one JSON file shared by every device.

    {"games": {scene: {"title": str, "entries": [{"player", "score", "date"}]}}}
    with each game's entries sorted best first (ties: earlier first).
    """

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.lock = threading.Lock()
        self.data = {"games": {}}
        if self.path.is_file():
            try:
                loaded = json.loads(self.path.read_text(encoding="utf-8"))
                if isinstance(loaded.get("games"), dict):
                    self.data = loaded
            except (OSError, ValueError):
                # A damaged file is kept aside rather than overwritten blindly.
                self.path.replace(self.path.with_suffix(".damaged.json"))

    def snapshot(self) -> dict:
        with self.lock:
            return json.loads(json.dumps(self.data))

    @staticmethod
    def validate(body: object) -> tuple[str, str, str, int]:
        """(scene, title, player, score), or ValueError with a reason."""
        if not isinstance(body, dict):
            raise ValueError("expected a JSON object")
        scene = body.get("scene")
        if not isinstance(scene, str) or not SCENE.match(scene):
            raise ValueError("scene must be 1-24 lowercase letters or digits")
        score = body.get("score")
        if isinstance(score, bool) or not isinstance(score, int) or not 0 <= score <= MAX_SCORE:
            raise ValueError(f"score must be an integer from 0 to {MAX_SCORE}")
        clean = lambda value: "".join(ch for ch in value if ch.isprintable()).strip()  # noqa: E731
        player = body.get("player", "")
        title = body.get("title", scene)
        if not isinstance(player, str) or not isinstance(title, str):
            raise ValueError("player and title must be strings")
        player, title = clean(player) or "Anonym", clean(title) or scene
        if len(player) > MAX_PLAYER:
            raise ValueError(f"player is longer than {MAX_PLAYER} characters")
        if len(title) > MAX_TITLE:
            raise ValueError(f"title is longer than {MAX_TITLE} characters")
        return scene, title, player, score

    def submit(self, body: object) -> dict:
        scene, title, player, score = self.validate(body)
        entry = {"player": player, "score": score,
                 "date": datetime.now().astimezone().isoformat(timespec="seconds")}
        with self.lock:
            game = self.data["games"].setdefault(scene, {"title": title, "entries": []})
            game["title"] = title
            game["entries"].append(entry)
            game["entries"].sort(key=lambda item: (-item["score"], item["date"]))
            del game["entries"][KEEP_PER_GAME:]
            self._save()
        return entry

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        handle, temporary = tempfile.mkstemp(dir=self.path.parent, prefix=".leaderboard-")
        try:
            with os.fdopen(handle, "w", encoding="utf-8") as stream:
                json.dump(self.data, stream, ensure_ascii=False, indent=1)
            os.replace(temporary, self.path)
        except BaseException:
            Path(temporary).unlink(missing_ok=True)
            raise


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".data": "application/octet-stream",
        ".bin": "application/octet-stream",
    }

    def send_json(self, status: int, payload: object) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - http.server API
        if self.path.split("?", 1)[0] == "/api/scores":
            return self.send_json(200, self.server.leaderboard.snapshot())
        return super().do_GET()

    def do_POST(self):  # noqa: N802 - http.server API
        if self.path.split("?", 1)[0] != "/api/scores":
            return self.send_json(404, {"error": "not found"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_BODY:
            return self.send_json(413 if length > MAX_BODY else 400,
                                  {"error": f"body must be 1-{MAX_BODY} bytes"})
        try:
            body = json.loads(self.rfile.read(length).decode("utf-8"))
            entry = self.server.leaderboard.submit(body)
        except (UnicodeDecodeError, ValueError) as error:
            return self.send_json(400, {"error": str(error)})
        return self.send_json(201, entry)

    def send_head(self):  # noqa: D401 - http.server API
        requested = unquote(urlsplit(self.path).path)
        if requested == "/":
            requested = "/index.html" if (Path(self.directory) / "index.html").is_file() else "/FGPNativePOC.html"
        if requested[1:] not in PUBLIC_FILES or not requested.startswith("/"):
            self.send_error(404, "Not found")
            return None
        path = str(Path(self.directory) / requested[1:])
        if Path(path).is_symlink():
            self.send_error(404, "Not found")
            return None
        self.path = requested
        header = self.headers.get("Range")
        if not header or not os.path.isfile(path):
            return super().send_head()
        match = RANGE.match(header.strip())
        size = os.path.getsize(path)
        if not match or (not match.group(1) and not match.group(2)):
            self.send_error(416, "Unsupported range")
            return None
        if match.group(1):
            start = int(match.group(1))
            end = int(match.group(2)) if match.group(2) else size - 1
        else:
            start = max(size - int(match.group(2)), 0)
            end = size - 1
        end = min(end, size - 1)
        if start > end or start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None
        stream = open(path, "rb")
        stream.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self._remaining = end - start + 1
        return stream

    def copyfile(self, source, outputfile):
        remaining = getattr(self, "_remaining", None)
        if remaining is None:
            return super().copyfile(source, outputfile)
        rate = self.server.throttle  # bytes per second, 0 = unlimited
        step = min(1 << 20, max(rate // 10, 16384)) if rate else 1 << 20
        while remaining > 0:
            chunk = source.read(min(remaining, step))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)
            if rate:
                time.sleep(len(chunk) / rate)
        self._remaining = None

    def log_message(self, format, *args):  # quieter: ranges are many
        if os.environ.get("FGP_SERVE_VERBOSE"):
            super().log_message(format, *args)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", nargs="?", default="build-web")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--throttle-kib", type=int, default=0,
                        help="limit ranged responses to this many KiB/s, to try "
                             "the loading screen at phone-like speeds")
    parser.add_argument("--scores-file", type=Path, default=DEFAULT_SCORES,
                        help=f"leaderboard storage (default {DEFAULT_SCORES})")
    args = parser.parse_args()
    handler = functools.partial(RangeHandler, directory=args.directory)
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        server.throttle = args.throttle_kib * 1024
        server.leaderboard = Leaderboard(args.scores_file)
        print(f"Serving {args.directory} on http://{args.bind}:{args.port}/FGPNativePOC.html"
              f" (leaderboard: /leaderboard.html, {args.scores_file})")
        server.serve_forever()


if __name__ == "__main__":
    main()
