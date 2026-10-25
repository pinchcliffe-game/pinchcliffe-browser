#include "platform/command_line.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

namespace fgp {

#ifdef _WIN32

namespace {

std::string utf8FromWide(const wchar_t *text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr,
                        nullptr);
    return result;
}

} // namespace

Utf8Arguments::Utf8Arguments(int argc, char **argv) {
    int count = 0;
    if (LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int index = 0; index < count; ++index) {
            storage_.push_back(utf8FromWide(wide[index]));
        }
        LocalFree(wide);
    } else {
        storage_.assign(argv, argv + argc);
    }
    for (std::string &argument : storage_) {
        pointers_.push_back(argument.data());
    }
    pointers_.push_back(nullptr);
}

#else

Utf8Arguments::Utf8Arguments(int argc, char **argv)
    : storage_(argv, argv + argc) {
    for (std::string &argument : storage_) {
        pointers_.push_back(argument.data());
    }
    pointers_.push_back(nullptr);
}

#endif

} // namespace fgp
