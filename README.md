# Pinchcliffe browser engine

An independent C++ reimplementation of *Flåklypa Grand Prix Gullutgave* (2002), with a browser importer for player-supplied game files.

This repository contains the engine and importer. It contains no original discs, executable, artwork, audio, movies, fonts, icons, or extracted game-content bundles. Recovered meshes, maps, level tables and other content are read from the player's executable at runtime. Asset identifiers, file-format addresses and engine behavior remain in the source.

## What you need to play

The current importer supports either:

- Both original disc images (the volumes `FGPGULL1` and `FGPGULL2`, as ISO or raw MODE1/2352 BIN files), **plus your own `FGP.exe` from update 1.2**.
- A compatible installed game folder containing the 1.2 `FGP.exe` and the game packages.

**The two CDs alone are not sufficient yet.** No update executable or download link to game content is supplied here. Select files in the browser; they are read locally and are not uploaded. Profiles are stored in the browser. An optional leaderboard sends score metadata to the server, not game files.

## Build and run in the browser

Install CMake, Ninja, Python 3 and Emscripten. With Emscripten's environment active:

```sh
sh tools/web/build-ffmpeg-wasm.sh
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release -DFGP_DEVELOPMENT_MODE=OFF -DFGP_OPEN_ALL_GAMES=ON
cmake --build build-web --parallel
python3 tools/web/serve.py build-web --bind 127.0.0.1 --port 8765
```

Open **http://localhost:8765/**, choose the disc images and executable (or the installed folder), and start the game. The server serves only an explicit list of engine files, even if a build directory contains other files. Keep original game files outside a public web root.

The FFmpeg build script downloads FFmpeg separately. SDL2 is supplied by Emscripten. Their upstream licenses apply; review their distribution requirements when distributing compiled builds.

## Native build and tests

The native build needs a C++20 compiler, CMake, SDL2, FFmpeg 5.1 or newer, pkg-config, Python 3 and Node.js. On macOS/Linux:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

These repository tests use synthetic data and require no original game files. Windows uses the SDL2/FFmpeg dependency directories described in `CMakeLists.txt`; this source export has not yet been validated on Windows.

Playwright browser integration tests are optional:

```sh
# Install Playwright separately, then run against the local server.
node tests/test_web_browser.cjs http://127.0.0.1:8765
```

Set `FGP_TEST_GAME_DIR`, or `FGP_TEST_DISC1`, `FGP_TEST_DISC2` and `FGP_TEST_EXE`, to exercise a real local import. `FGP_TEST_SCENE=puzzle` selects a particular scene in a development build. Tests block HTTP after import and reject unexpected uploads.

## Publication checks

Stage only `FGPNativePOC.html`, `FGPNativePOC.js`, `FGPNativePOC.wasm`, `local-files.js`, `local-files-worker.js`, `disc-image.js` and `leaderboard.html` in a clean directory, then run:

```sh
python3 tools/web/check_publication.py path/to/staged-site
python3 tools/web/check_embedded_data.py path/to/staged-site/FGPNativePOC.wasm --executable /path/to/your/FGP.exe
```

The second check uses your local executable to look for known embedded original meshes, maps, levels and placement data. It stores no original payloads in this repository. These are technical checks, not a guarantee of legal clearance. This is an unofficial project, with no affiliation or endorsement claimed.

Do not commit or attach original game files, extracted assets, save profiles, or screenshots containing original artwork to this repository.
