# Repository rules

- Keep this repository source-only. Never commit original game files, extracted content, original-derived icons, game screenshots, or saves.
- Read original content from player-selected local files. Do not add compiled fallback content or remote asset downloads.
- Keep filenames, format addresses, and engine logic separate from game-content payloads. Update the publication checks when changing import or asset loading.
- Use a `codex/` branch for changes, review the diff, run the relevant build and tests, open a pull request, and merge after checks pass.
- Native scripted runs must use `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software FGP_ALLOW_MULTIPLE_INSTANCES=1`.
- Do not claim reference parity without an original-versus-native capture. Do not claim legal clearance from a technical content audit.
