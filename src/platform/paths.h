#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace fgp {

// Resolves the original-game installation directory, first match wins: the
// first argument; FGP_GAME_DIR; the source-tree layouts (src/data/game,
// src/data/extracted, gamedata/) under the working directory or its parent;
// the executable's directory and its ancestors, as an installation, with the
// data in game/, or with the source-tree layouts; and on Windows the original
// game's own installation, from its registry key. Without a match it returns
// the working directory, and the caller reports the missing packages.
// argv must be UTF-8 (see platform/command_line.h).
std::filesystem::path findGameDirectory(int argc, char **argv);

// The directory holding the running executable, when the platform says.
std::optional<std::filesystem::path> executableDirectory();

// Where profiles are kept when FGP_PROFILE_DIR does not say. The original
// keeps them in <game>/Profiles, and so does this when that folder (or, before
// it exists, the game folder) accepts new files. An installation the player
// cannot write to, such as one under C:\Program Files, gets
// <userDataDirectory>/Profiles instead, which starts as a copy of the
// installation's own profiles.
std::filesystem::path chooseProfileDirectory(
    const std::filesystem::path &gameDirectory);

// This program's per-user data folder: %LOCALAPPDATA%\FGP Native on Windows,
// ~/Library/Application Support/FGP Native on macOS, and
// $XDG_DATA_HOME/fgp-native (or ~/.local/share/fgp-native) elsewhere.
std::optional<std::filesystem::path> userDataDirectory();

// The directory an installation of the original game recorded in the
// registry ('Resource Path'), on Windows; nothing elsewhere.
std::optional<std::filesystem::path> originalInstallDirectory();

// A path from UTF-8 text. A path built from a plain char string is read in the
// process code page on Windows, which is not UTF-8 unless the manifest made it
// so, so text known to be UTF-8 goes through here.
std::filesystem::path pathFromUtf8(std::string_view text);

// The environment variable as a path, or nothing when it is unset or empty.
// On Windows it is read from the wide environment, so it does not depend on
// the code page either.
std::optional<std::filesystem::path> environmentPath(const char *name);

} // namespace fgp
