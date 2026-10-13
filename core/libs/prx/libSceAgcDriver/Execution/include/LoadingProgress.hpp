#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace AnyPS5::Loading {

enum class Stage : unsigned { Shader, Graphics, Library, Compute, Count };

struct Snapshot {
    std::array<unsigned, static_cast<unsigned>(Stage::Count)> active{};
    unsigned long long completed = 0;
    unsigned long long failed = 0;
    long long elapsedMs = 0;
    long long lastCompletedMs = 0;
};

class Tracker {
public:
    void Begin(Stage stage) {
        std::lock_guard lock(mutex);
        ++state.active.at(static_cast<unsigned>(stage));
    }
    void End(Stage stage, bool failed) {
        std::lock_guard lock(mutex);
        --state.active.at(static_cast<unsigned>(stage));
        if (failed) ++state.failed;
        else ++state.completed;
        state.lastCompletedMs = elapsed();
    }
    Snapshot Read() {
        std::lock_guard lock(mutex);
        auto result = state;
        result.elapsedMs = elapsed();
        return result;
    }
private:
    long long elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::mutex mutex;
    Snapshot state;
};

inline long long Timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::filesystem::path StatusPath(const char* suffix) {
#ifdef _WIN32
    const auto* wide = _wgetenv(L"ANYPS5_LOADING_STATUS");
    if (wide == nullptr || *wide == L'\0') return {};
    auto path = std::filesystem::path(wide);
    path += suffix;
    return path;
#else
    const auto* base = std::getenv("ANYPS5_LOADING_STATUS");
    return base != nullptr && *base != '\0' ? std::filesystem::u8path(std::string(base) + suffix) : std::filesystem::path{};
#endif
}

inline void WriteSnapshot(const std::filesystem::path& path, const Snapshot& state) {
    std::ofstream output(path, std::ios::trunc);
    output << "{\"version\":1,\"updatedAt\":" << Timestamp()
           << ",\"elapsedMs\":" << state.elapsedMs << ",\"lastCompletedMs\":" << state.lastCompletedMs
           << ",\"completed\":" << state.completed << ",\"failed\":" << state.failed << ",\"active\":[";
    for (unsigned i = 0; i < state.active.size(); ++i) output << (i == 0 ? "" : ",") << state.active[i];
    output << "]}";
}

class Reporter {
public:
    Reporter() : path(StatusPath(".work")) {
        try {
            if (!path.empty()) worker = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    try { WriteSnapshot(path, tracker.Read()); } catch (...) {}
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            });
        } catch (...) { path.clear(); }
    }
    bool Enabled() const { return !path.empty(); }
    Tracker tracker;
private:
    std::filesystem::path path;
    std::jthread worker;
};

inline Reporter& SharedReporter() {
    static Reporter reporter;
    return reporter;
}

class Scope {
public:
    explicit Scope(Stage stage) : stage(stage), exceptions(std::uncaught_exceptions()) {
        auto& reporter = SharedReporter();
        if (reporter.Enabled()) {
            tracker = &reporter.tracker;
            tracker->Begin(stage);
        }
    }
    Scope(Tracker& tracker, Stage stage) : tracker(&tracker), stage(stage), exceptions(std::uncaught_exceptions()) { tracker.Begin(stage); }
    ~Scope() { if (tracker != nullptr) tracker->End(stage, failed || std::uncaught_exceptions() > exceptions); }
    void Failed() { failed = true; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Tracker* tracker = nullptr;
    Stage stage;
    int exceptions;
    bool failed = false;
};

}
