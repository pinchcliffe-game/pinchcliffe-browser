#include "platform/console.h"

#include <cstdio>
#include <iostream>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fgp {

#ifdef _WIN32

namespace {

bool streamIsCaptured(DWORD stream) {
    const HANDLE handle = GetStdHandle(stream);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

bool standardOutputIsCaptured() { return streamIsCaptured(STD_OUTPUT_HANDLE); }

void rebindStandardStreams() {
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    // The packaged text and the game directory are printed as UTF-8.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"FGP Native — developer console");
    // The iostreams were bound to the old handles before this point.
    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}

} // namespace

bool openDeveloperConsole(bool force) {
    // A scripted run redirects stdout to a log; taking that away would defeat
    // the capture, so only an explicit request overrides it.
    if (standardOutputIsCaptured() && !force) {
        return false;
    }
    const bool alreadyAttached = GetConsoleWindow() != nullptr;
    if (alreadyAttached && !force) {
        // Started from a terminal: that window is the developer console.
        return false;
    }
    if (alreadyAttached) {
        // A process may own only one console, so release the inherited one
        // before asking for a window of our own.
        FreeConsole();
    }
    if (!AllocConsole()) {
        return false;
    }
    rebindStandardStreams();
    return true;
}

bool fatalErrorNeedsDialog(bool sdlStarted) {
    if (streamIsCaptured(STD_ERROR_HANDLE)) {
        return false;
    }
    if (sdlStarted || GetConsoleWindow() == nullptr) {
        return true;
    }
    // One attached process means the console is this process's own: Windows
    // made it for a double-click, or openDeveloperConsole did, and it closes
    // with the process, taking the message with it.
    DWORD processes[2]{};
    return GetConsoleProcessList(processes, 2) <= 1;
}

#else

// Only Windows creates consoles on demand. Elsewhere the diagnostics stay on
// the terminal that launched the application, which already is a window of
// its own; spawning a terminal emulator would depend on which one is
// installed, so it is deliberately not attempted.
bool openDeveloperConsole(bool) { return false; }

#ifdef __EMSCRIPTEN__
// The page has no stderr a player sees; SDL's box is an alert() there.
bool fatalErrorNeedsDialog(bool sdlStarted) { return sdlStarted; }
#else
bool fatalErrorNeedsDialog(bool sdlStarted) {
    struct stat status {};
    if (fstat(STDERR_FILENO, &status) == 0 &&
        (S_ISREG(status.st_mode) || S_ISFIFO(status.st_mode) ||
         S_ISSOCK(status.st_mode))) {
        return false;
    }
    return sdlStarted || isatty(STDERR_FILENO) == 0;
}
#endif

#endif

} // namespace fgp
