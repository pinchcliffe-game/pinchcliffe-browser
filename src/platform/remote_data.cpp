#include "platform/remote_data.h"

#include <stdexcept>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// The page can supply only slices of user-selected local Files/Blobs. There
// is deliberately no network fallback if a file is missing or unreadable.
// clang-format off
EM_JS(int, fgp_read_local_range, (const char *path, double offset, double size,
                                  std::uint8_t *destination), {
    try {
        if (!Module.fgpReadLocal) return -1;
        const bytes = Module.fgpReadLocal(UTF8ToString(path), offset, size);
        if (bytes.length !== size) return -1;
        HEAPU8.set(bytes, destination);
        return size;
    } catch (error) {
        console.error('Local game file read failed', error);
        return -1;
    }
});
// clang-format on
#endif

namespace fgp {
namespace fs = std::filesystem;

bool isLocalBrowserPackage(const fs::path &archive) {
#ifdef __EMSCRIPTEN__
    std::error_code error;
    return fs::is_regular_file(fs::path(archive.string() + ".idx"), error);
#else
    (void)archive;
    return false;
#endif
}

void readLocalBrowserRange(const fs::path &archive, std::size_t offset,
                           std::size_t size, std::uint8_t *destination) {
#ifdef __EMSCRIPTEN__
    constexpr std::string_view mount = "/src/data/game/";
    const std::string path = archive.generic_string();
    if (path.rfind(mount, 0) != 0) {
        throw std::runtime_error("Not under the browser data mount: " + path);
    }
    const std::string relative = path.substr(mount.size());
    const int read = fgp_read_local_range(relative.c_str(), static_cast<double>(offset),
                                         static_cast<double>(size), destination);
    if (read < 0 || static_cast<std::size_t>(read) != size) {
        throw std::runtime_error("Cannot read local game file: " + relative);
    }
#else
    (void)offset;
    (void)size;
    (void)destination;
    throw std::runtime_error("Local browser packages only exist in the browser build: " +
                             archive.string());
#endif
}

double sceneDataProgress(const std::string &scene) {
    (void)scene;
    return 1.0; // Local files are available immediately; no server prefetch.
}

} // namespace fgp
