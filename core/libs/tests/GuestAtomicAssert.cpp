#include "ModuleChecks.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" void APS5_VABI _ZSt14_Atomic_assertPKcS0__nid_postfix(const char*, const char*);

namespace {

using ModuleTest::Require;

constexpr int AssertionSignalExit = 86;

struct AssertionCase {
    const char* message;
    const char* location;
    const char* diagnostic;
};

constexpr std::array Cases{
    AssertionCase{"invalid atomic memory order", "atomic-order:53",
                  "[libc] guest assertion failed: invalid atomic memory order (atomic-order:53)\n"},
    AssertionCase{"%n%s%08x", "%p%n",
                  "[libc] guest assertion failed: %n%s%08x (%p%n)\n"},
    AssertionCase{"", "", "[libc] guest assertion failed:  ()\n"},
    AssertionCase{nullptr, "atomic-order:9", "[libc] guest assertion failed: ? (atomic-order:9)\n"},
    AssertionCase{"invalid order", nullptr, "[libc] guest assertion failed: invalid order (?)\n"},
    AssertionCase{nullptr, nullptr, "[libc] guest assertion failed: ? (?)\n"}
};

void assertionSignal(int signal) {
    std::_Exit(signal == SIGABRT ? AssertionSignalExit : 87);
}

int runAssertion(std::size_t index) {
    if (std::signal(SIGABRT, assertionSignal) == SIG_ERR) return 88;
    try {
        _ZSt14_Atomic_assertPKcS0__nid_postfix(Cases[index].message, Cases[index].location);
    } catch (...) {
        return 89;
    }
    return 90;
}

struct ChildResult {
    int exitCode;
    std::string output;
};

#ifdef _WIN32

ChildResult runChild(std::size_t index) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    Require(CreatePipe(&readPipe, &writePipe, &attributes, 4096) != 0);
    Require(SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0) != 0);
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Require(length > 0 && length < executable.size());
    std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" " + std::to_wstring(index);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = writePipe;
    PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != 0;
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        Require(false);
    }
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, 3000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 91);
        WaitForSingleObject(child.hProcess, 1000);
        CloseHandle(child.hProcess);
        CloseHandle(readPipe);
        Require(false);
    }
    DWORD exitCode = 0;
    Require(GetExitCodeProcess(child.hProcess, &exitCode) != 0);
    CloseHandle(child.hProcess);
    ChildResult result{static_cast<int>(exitCode), {}};
    std::array<char, 4096> buffer{};
    DWORD bytes = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr) != 0 && bytes != 0) {
        result.output.append(buffer.data(), bytes);
    }
    CloseHandle(readPipe);
    return result;
}

#else

ChildResult runChild(std::size_t index) {
    std::array<int, 2> pipeDescriptors{};
    Require(pipe(pipeDescriptors.data()) == 0);
    const pid_t child = fork();
    Require(child >= 0);
    if (child == 0) {
        close(pipeDescriptors[0]);
        if (dup2(pipeDescriptors[1], STDERR_FILENO) == -1) std::_Exit(92);
        close(pipeDescriptors[1]);
        std::_Exit(runAssertion(index));
    }
    close(pipeDescriptors[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    if (waited != child) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) == -1 && errno == EINTR) {}
        close(pipeDescriptors[0]);
        Require(false);
    }
    ChildResult result{WIFEXITED(status) ? WEXITSTATUS(status) : -1, {}};
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto bytes = read(pipeDescriptors[0], buffer.data(), buffer.size());
        if (bytes > 0) result.output.append(buffer.data(), static_cast<std::size_t>(bytes));
        else if (bytes < 0 && errno == EINTR) continue;
        else break;
    }
    close(pipeDescriptors[0]);
    return result;
}

#endif

}

int main(int argc, char** argv) {
    if (argc == 2) {
#ifdef _WIN32
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
        for (std::size_t index = 0; index < Cases.size(); ++index) {
            if (std::to_string(index) == argv[1]) return runAssertion(index);
        }
        return 93;
    }
    Require(argc == 1);
    for (std::size_t index = 0; index < Cases.size(); ++index) {
        const auto result = runChild(index);
        if (result.exitCode != AssertionSignalExit) {
            std::fprintf(stderr, "Atomic assertion case %zu exited with %d instead of SIGABRT\n", index, result.exitCode);
        }
        Require(result.exitCode == AssertionSignalExit);
        std::string output = result.output;
        for (std::size_t position = 0; (position = output.find("\r\n", position)) != std::string::npos;) {
            output.erase(position, 1);
        }
        Require(output == Cases[index].diagnostic);
    }
}
