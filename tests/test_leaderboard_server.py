#!/usr/bin/env python3
"""The shared leaderboard API in tools/web/serve.py, end to end over HTTP.

Starts the real server on a free local port with a temporary scores file, and
checks what the browser build (src/platform/leaderboard.cpp) and
tools/web/leaderboard.html rely on: accepted scores come back sorted best
first, bad input is refused without touching the file, each game keeps its
best 50, and the file survives a restart. Needs nothing but the standard
library.
"""

from __future__ import annotations

import functools
import http.server
import json
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "web"))

import serve  # noqa: E402

FAILURES: list[str] = []


def expect(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def request(base: str, method: str, body: bytes | None = None,
            headers: dict | None = None) -> tuple[int, object]:
    call = urllib.request.Request(base + "/api/scores", data=body, method=method,
                                  headers=headers or {})
    try:
        with urllib.request.urlopen(call, timeout=10) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"null")


def post(base: str, payload: object) -> tuple[int, object]:
    return request(base, "POST", json.dumps(payload).encode("utf-8"),
                   {"Content-Type": "application/json"})


def start(directory: Path, scores: Path) -> tuple[http.server.ThreadingHTTPServer, str]:
    handler = functools.partial(serve.RangeHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.throttle = 0
    server.leaderboard = serve.Leaderboard(scores)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        scores = root / "state" / "leaderboard.json"
        server, base = start(root, scores)
        try:
            status, body = request(base, "GET")
            expect(status == 200 and body == {"games": {}},
                   f"an empty board must read as no games, got {status} {body}")

            status, entry = post(base, {"scene": "whackamole", "title": "Mullvadjakt",
                                        "player": "Åse", "score": 120})
            expect(status == 201 and entry.get("player") == "Åse"
                   and entry.get("score") == 120 and entry.get("date"),
                   f"a valid score must be stored and echoed, got {status} {entry}")
            post(base, {"scene": "whackamole", "title": "Mullvadjakt",
                        "player": "Sondre", "score": 300})
            post(base, {"scene": "whackamole", "title": "Mullvadjakt",
                        "player": "", "score": 50})
            status, body = request(base, "GET")
            game = body.get("games", {}).get("whackamole", {})
            expect(game.get("title") == "Mullvadjakt",
                   f"the game's title must be kept, got {game.get('title')!r}")
            expect([item["score"] for item in game.get("entries", [])] == [300, 120, 50],
                   f"entries must be sorted best first, got {game.get('entries')}")
            expect(game["entries"][-1]["player"] == "Anonym",
                   "an empty player name must become Anonym")

            refused = [
                ({"scene": "Whack-a-Mole", "score": 1}, "scene with capitals and dashes"),
                ({"scene": "", "score": 1}, "empty scene"),
                ({"scene": "whackamole", "score": -1}, "negative score"),
                ({"scene": "whackamole", "score": 100_000_001}, "score above the cap"),
                ({"scene": "whackamole", "score": "12"}, "score as a string"),
                ({"scene": "whackamole", "score": 1.5}, "fractional score"),
                ({"scene": "whackamole", "score": True}, "boolean score"),
                ({"scene": "whackamole", "score": 1, "player": "x" * 33}, "33-char player"),
                ({"scene": "whackamole", "score": 1, "title": "x" * 65}, "65-char title"),
                (["whackamole", 1], "a JSON array"),
            ]
            for payload, what in refused:
                status, body = post(base, payload)
                expect(status == 400 and "error" in body,
                       f"{what} must be refused with 400, got {status} {body}")
            status, _ = request(base, "POST", b"{not json", {"Content-Type": "application/json"})
            expect(status == 400, f"malformed JSON must be refused, got {status}")
            status, _ = request(base, "POST", b"x" * (serve.MAX_BODY + 1),
                                {"Content-Type": "application/json"})
            expect(status == 413, f"an oversized body must be refused, got {status}")
            status, body = request(base, "GET")
            expect(len(body["games"]["whackamole"]["entries"]) == 3,
                   "refused submissions must not change the board")

            for score in range(55):
                post(base, {"scene": "pipeline", "title": "Rørlegging",
                            "player": f"p{score}", "score": score})
            status, body = request(base, "GET")
            kept = [item["score"] for item in body["games"]["pipeline"]["entries"]]
            expect(len(kept) == serve.KEEP_PER_GAME and kept[0] == 54 and kept[-1] == 5,
                   f"each game must keep only its best {serve.KEEP_PER_GAME}, got "
                   f"{len(kept)} from {kept[:1]} to {kept[-1:]}")

            stored = json.loads(scores.read_text(encoding="utf-8"))
            expect(stored == body, "the file must hold exactly what GET returns")
            leftovers = [p.name for p in scores.parent.iterdir() if p.name != scores.name]
            expect(not leftovers, f"atomic saves must leave no temporary files: {leftovers}")
        finally:
            server.shutdown()
            server.server_close()

        server, base = start(root, scores)
        try:
            status, body = request(base, "GET")
            expect(body["games"]["whackamole"]["entries"][0]["player"] == "Sondre",
                   "scores must survive a restart")
        finally:
            server.shutdown()
            server.server_close()

    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL {failure}")
        return 1
    print("leaderboard API: storing, sorting, refusing, capping and persisting checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
