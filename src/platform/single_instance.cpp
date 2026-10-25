#include "platform/single_instance.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <libproc.h>
#include <signal.h>
#include <sys/types.h>
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

#include <tlhelp32.h>
#endif

#ifdef __linux__
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#endif

namespace fgp {
#ifdef __linux__
namespace fs = std::filesystem;
#endif

void closeOlderNativeInstances() {
    // Tools that run several headless copies side by side (for example
    // tools/web/record_scene_reads.py) opt out of closing older copies.
    if (const char *allow = std::getenv("FGP_ALLOW_MULTIPLE_INSTANCES");
        allow && *allow && std::string(allow) != "0") {
        return;
    }
#ifdef __APPLE__
    char currentPathBuffer[PROC_PIDPATHINFO_MAXSIZE]{};
    const pid_t currentPid = getpid();
    if (proc_pidpath(currentPid, currentPathBuffer,
                     sizeof(currentPathBuffer)) <= 0) {
        return;
    }

    proc_bsdinfo currentInfo{};
    if (proc_pidinfo(currentPid, PROC_PIDTBSDINFO, 0, &currentInfo,
                     sizeof(currentInfo)) != sizeof(currentInfo)) {
        return;
    }

    const int estimatedCount = proc_listallpids(nullptr, 0);
    if (estimatedCount <= 0) {
        return;
    }
    std::vector<pid_t> pids(static_cast<std::size_t>(estimatedCount) + 64U);
    const int count = proc_listallpids(
        pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (count <= 0) {
        return;
    }

    const std::string currentPath(currentPathBuffer);
    std::vector<pid_t> olderInstances;
    for (int index = 0; index < count; ++index) {
        const pid_t candidate = pids[static_cast<std::size_t>(index)];
        if (candidate <= 0 || candidate == currentPid) {
            continue;
        }

        proc_bsdinfo candidateInfo{};
        if (proc_pidinfo(candidate, PROC_PIDTBSDINFO, 0, &candidateInfo,
                         sizeof(candidateInfo)) != sizeof(candidateInfo) ||
            candidateInfo.pbi_uid != currentInfo.pbi_uid) {
            continue;
        }
        const bool startedEarlier =
            candidateInfo.pbi_start_tvsec < currentInfo.pbi_start_tvsec ||
            (candidateInfo.pbi_start_tvsec == currentInfo.pbi_start_tvsec &&
             (candidateInfo.pbi_start_tvusec < currentInfo.pbi_start_tvusec ||
              (candidateInfo.pbi_start_tvusec == currentInfo.pbi_start_tvusec &&
               candidate < currentPid)));
        if (!startedEarlier) {
            continue;
        }

        char candidatePathBuffer[PROC_PIDPATHINFO_MAXSIZE]{};
        if (proc_pidpath(candidate, candidatePathBuffer,
                         sizeof(candidatePathBuffer)) <= 0 ||
            currentPath != candidatePathBuffer) {
            continue;
        }
        olderInstances.push_back(candidate);
    }

    for (const pid_t pid : olderInstances) {
        kill(pid, SIGTERM);
    }
    if (!olderInstances.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (const pid_t pid : olderInstances) {
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
        }
    }
    if (!olderInstances.empty()) {
        std::cout << "Closed " << olderInstances.size()
                  << " older FGP Native instance(s)\n";
    }
#endif
#ifdef _WIN32
    // Same policy as the macOS branch: the newest launch of this exact
    // executable survives; earlier copies of the same binary are closed.
    std::vector<wchar_t> currentPathBuffer(32768);
    const DWORD currentPathLength = GetModuleFileNameW(
        nullptr, currentPathBuffer.data(),
        static_cast<DWORD>(currentPathBuffer.size()));
    if (currentPathLength == 0 ||
        currentPathLength >= currentPathBuffer.size()) {
        return;
    }

    const DWORD currentPid = GetCurrentProcessId();
    FILETIME currentCreation{}, unusedExit{}, unusedKernel{}, unusedUser{};
    if (!GetProcessTimes(GetCurrentProcess(), &currentCreation, &unusedExit,
                         &unusedKernel, &unusedUser)) {
        return;
    }
    const auto fileTimeValue = [](const FILETIME &time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
               time.dwLowDateTime;
    };
    const std::uint64_t currentStart = fileTimeValue(currentCreation);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    struct OlderInstance {
        HANDLE handle;
        DWORD pid;
    };
    std::vector<OlderInstance> olderInstances;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == currentPid || entry.th32ProcessID == 0) {
            continue;
        }
        HANDLE candidate = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE, entry.th32ProcessID);
        if (!candidate) {
            continue;
        }

        std::vector<wchar_t> candidatePathBuffer(32768);
        DWORD candidatePathLength =
            static_cast<DWORD>(candidatePathBuffer.size());
        FILETIME candidateCreation{};
        const bool samePath =
            QueryFullProcessImageNameW(candidate, 0, candidatePathBuffer.data(),
                                       &candidatePathLength) &&
            _wcsicmp(candidatePathBuffer.data(), currentPathBuffer.data()) == 0;
        const bool startedEarlier =
            samePath &&
            GetProcessTimes(candidate, &candidateCreation, &unusedExit,
                            &unusedKernel, &unusedUser) &&
            (fileTimeValue(candidateCreation) < currentStart ||
             (fileTimeValue(candidateCreation) == currentStart &&
              entry.th32ProcessID < currentPid));
        if (startedEarlier) {
            olderInstances.push_back({candidate, entry.th32ProcessID});
        } else {
            CloseHandle(candidate);
        }
    }
    CloseHandle(snapshot);

    if (olderInstances.empty()) {
        return;
    }

    // Mirror the macOS SIGTERM-then-SIGKILL sequence: request a normal close
    // first so the older instance can flush its output, then terminate it.
    const auto postCloseToInstanceWindows = [](HWND window,
                                               LPARAM parameter) -> BOOL {
        const auto &instances =
            *reinterpret_cast<const std::vector<OlderInstance> *>(parameter);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(window, &windowPid);
        for (const OlderInstance &instance : instances) {
            if (instance.pid == windowPid) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                break;
            }
        }
        return TRUE;
    };
    EnumWindows(postCloseToInstanceWindows,
                reinterpret_cast<LPARAM>(&olderInstances));

    for (const OlderInstance &instance : olderInstances) {
        if (WaitForSingleObject(instance.handle, 250) != WAIT_OBJECT_0) {
            TerminateProcess(instance.handle, 0);
            WaitForSingleObject(instance.handle, 250);
        }
        CloseHandle(instance.handle);
    }
    std::cout << "Closed " << olderInstances.size()
              << " older FGP Native instance(s)\n";
#endif
#ifdef __linux__
    // Same policy as the other platforms: the newest launch of this exact
    // executable survives; earlier copies of the same binary are closed.
    std::error_code linkError;
    const fs::path currentPath = fs::read_symlink("/proc/self/exe", linkError);
    if (linkError || currentPath.empty()) {
        return;
    }

    const pid_t currentPid = getpid();
    const auto startTicks = [](pid_t pid) -> std::optional<unsigned long long> {
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string content((std::istreambuf_iterator<char>(stat)),
                            std::istreambuf_iterator<char>());
        // The comm field may contain spaces; fields resume after the last ')'.
        const std::size_t comm = content.rfind(')');
        if (comm == std::string::npos) {
            return std::nullopt;
        }
        std::istringstream fields(content.substr(comm + 1));
        std::string token;
        for (int index = 1; index <= 20; ++index) {
            if (!(fields >> token)) {
                return std::nullopt;
            }
        }
        return std::stoull(token);
    };
    const auto currentStart = startTicks(currentPid);
    if (!currentStart) {
        return;
    }

    std::vector<pid_t> olderInstances;
    for (const auto &entry : fs::directory_iterator("/proc", linkError)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() ||
            name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        const pid_t candidate = static_cast<pid_t>(std::stol(name));
        if (candidate == currentPid) {
            continue;
        }
        std::error_code candidateError;
        const fs::path candidatePath =
            fs::read_symlink(entry.path() / "exe", candidateError);
        if (candidateError || candidatePath != currentPath) {
            continue;
        }
        const auto candidateStart = startTicks(candidate);
        if (!candidateStart) {
            continue;
        }
        const bool startedEarlier =
            *candidateStart < *currentStart ||
            (*candidateStart == *currentStart && candidate < currentPid);
        if (startedEarlier) {
            olderInstances.push_back(candidate);
        }
    }

    for (const pid_t pid : olderInstances) {
        kill(pid, SIGTERM);
    }
    if (!olderInstances.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (const pid_t pid : olderInstances) {
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
        }
    }
    if (!olderInstances.empty()) {
        std::cout << "Closed " << olderInstances.size()
                  << " older FGP Native instance(s)\n";
    }
#endif
}

} // namespace fgp
