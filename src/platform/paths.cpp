#include "platform/paths.h"

#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#endif

#ifdef __APPLE__
#include <cstdint>
#include <mach-o/dyld.h>
#endif

namespace fgp {
namespace fs = std::filesystem;

fs::path pathFromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::optional<fs::path> environmentPath(const char *name) {
#ifdef _WIN32
    const std::wstring wideName(name, name + std::char_traits<char>::length(name));
    const DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    if (length == 0 || length >= size) {
        return std::nullopt;
    }
    value.resize(length);
    return fs::path(value);
#else
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return fs::path(value);
#endif
}

fs::path findGameDirectory(int argc, char **argv) {
    if (argc >= 2 && std::string_view(argv[1]).rfind("--", 0) != 0) {
        return pathFromUtf8(argv[1]);
    }
    if (std::optional<fs::path> environment = environmentPath("FGP_GAME_DIR")) {
        return *environment;
    }
    // An installation counts as usable when its menu and common packages are
    // present in either form: packed archives or directories of loose files.
    const auto usable = [](const fs::path &root) {
        const auto hasPackage = [&](std::string_view name) {
            const fs::path layer = root / "Data";
            std::error_code error;
            return fs::is_regular_file(layer / (std::string(name) + ".bin"),
                                       error) ||
                   fs::is_directory(layer / name, error);
        };
        return hasPackage("menu") && hasPackage("common");
    };
    // The installation that ships with the source tree, under src/data/game.
    // The parent directory is checked too so running from inside build/ works,
    // and the historical gamedata/ location is still accepted.
    const fs::path sourceTreeLayouts[] = {
        fs::path("src") / "data" / "game",
        fs::path("src") / "data" / "extracted",
        fs::path("gamedata"),
    };
    for (const fs::path &base :
         {fs::current_path(), fs::current_path().parent_path()}) {
        for (const fs::path &relative : sourceTreeLayouts) {
            if (usable(base / relative)) {
                return base / relative;
            }
        }
    }
    // Then the same layouts relative to the executable, so a build finds its
    // data whatever the working directory (a macOS bundle starts in /), and a
    // distributed folder works: the data in game/ beside the program, or the
    // program dropped into an installation of the original. The ancestors
    // reach a checkout from build/FGPNativePOC.app/Contents/MacOS and a folder
    // beside the bundle from inside it.
    if (const std::optional<fs::path> executable = executableDirectory()) {
        fs::path base = *executable;
        for (int level = 0; level < 5 && !base.empty(); ++level) {
            for (const fs::path &candidate : {base, base / "game"}) {
                if (usable(candidate)) {
                    return candidate;
                }
            }
            for (const fs::path &relative : sourceTreeLayouts) {
                if (usable(base / relative)) {
                    return base / relative;
                }
            }
            if (base == base.parent_path()) {
                break;
            }
            base = base.parent_path();
        }
    }
    // Last, an installation of the original, which records its directory in
    // the registry the way FGP.exe reads it back (0x00410093 pushes
    // 'Resource Path', 0x00410098 reads it from HKEY_LOCAL_MACHINE).
    if (const std::optional<fs::path> installed = originalInstallDirectory();
        installed && usable(*installed)) {
        return *installed;
    }
    return fs::current_path();
}

std::optional<fs::path> executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code error;
    const fs::path resolved = fs::weakly_canonical(buffer, error);
    return (error ? fs::path(buffer) : resolved).parent_path();
#elif defined(__linux__)
    std::error_code error;
    const fs::path resolved = fs::read_symlink("/proc/self/exe", error);
    if (error) {
        return std::nullopt;
    }
    return resolved.parent_path();
#else
    return std::nullopt;
#endif
}

std::optional<fs::path> originalInstallDirectory() {
#ifdef _WIN32
    // HKLM\SOFTWARE\CapricornuS AS\Flåklypa Grand Prix\1.00.000 in the
    // 32-bit view, as tools/configure_fgp_registry.py writes it.
    const wchar_t *key =
        L"SOFTWARE\\CapricornuS AS\\Fl\u00E5klypa Grand Prix\\1.00.000";
    DWORD size = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Resource Path", flags, nullptr,
                     nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Resource Path", flags, nullptr,
                     value.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(std::wcslen(value.c_str()));
    if (value.empty()) {
        return std::nullopt;
    }
    return fs::path(value);
#else
    return std::nullopt;
#endif
}

namespace {

// Whether a new file can be made in the directory: the only test that agrees
// with ACLs, read-only media and Program Files alike.
bool directoryAcceptsFiles(const fs::path &directory) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        return false;
    }
    const fs::path probe = directory / ".fgp-native-write-probe";
    bool accepted = false;
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        accepted = static_cast<bool>(output);
    }
    fs::remove(probe, error);
    return accepted;
}

} // namespace

std::optional<fs::path> userDataDirectory() {
#if defined(_WIN32)
    if (const std::optional<fs::path> local = environmentPath("LOCALAPPDATA")) {
        return *local / "FGP Native";
    }
#elif defined(__APPLE__)
    if (const std::optional<fs::path> home = environmentPath("HOME")) {
        return *home / "Library" / "Application Support" / "FGP Native";
    }
#elif !defined(__EMSCRIPTEN__)
    if (const std::optional<fs::path> data = environmentPath("XDG_DATA_HOME")) {
        return *data / "fgp-native";
    }
    if (const std::optional<fs::path> home = environmentPath("HOME")) {
        return *home / ".local" / "share" / "fgp-native";
    }
#endif
    return std::nullopt;
}

fs::path chooseProfileDirectory(const fs::path &gameDirectory) {
    const fs::path installed = gameDirectory / "Profiles";
    std::error_code error;
    const bool exists = fs::is_directory(installed, error);
    if (directoryAcceptsFiles(exists ? installed : gameDirectory)) {
        return installed;
    }
    const std::optional<fs::path> userData = userDataDirectory();
    if (!userData) {
        return installed;
    }
    const fs::path personal = *userData / "Profiles";
    if (!fs::exists(personal, error)) {
        fs::create_directories(personal, error);
        if (error) {
            return installed;
        }
        if (exists) {
            for (const auto &item : fs::directory_iterator(installed, error)) {
                if (item.is_regular_file(error) &&
                    item.path().extension() == ".pfl") {
                    std::error_code copyError;
                    fs::copy_file(item.path(),
                                  personal / item.path().filename(),
                                  fs::copy_options::skip_existing, copyError);
                }
            }
        }
    }
    return personal;
}

} // namespace fgp
