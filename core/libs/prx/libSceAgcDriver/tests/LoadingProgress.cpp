#include "prx/libSceAgcDriver/Execution/include/LoadingProgress.hpp"
#include <barrier>
#include <iterator>
#include <stdexcept>
#include <vector>

void Require(bool condition) { if (!condition) throw std::runtime_error("loading progress regression"); }

int main() {
    using namespace AnyPS5::Loading;
    Tracker tracker;
    std::barrier entered(5), release(5);
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&, i] {
        Scope step(tracker, static_cast<Stage>(i));
        entered.arrive_and_wait();
        release.arrive_and_wait();
    });
    entered.arrive_and_wait();
    const auto active = tracker.Read();
    Require(active.completed == 0 && active.failed == 0);
    for (const auto count : active.active) Require(count == 1);
    release.arrive_and_wait();
    threads.clear();
    try {
        Scope step(tracker, Stage::Shader);
        throw std::runtime_error("expected failure");
    } catch (const std::runtime_error&) {}
    {
        Scope step(tracker, Stage::Library);
        step.Failed();
    }
    const auto finished = tracker.Read();
    Require(finished.completed == 4 && finished.failed == 2);
    for (const auto count : finished.active) Require(count == 0);
    Require(finished.lastCompletedMs <= finished.elapsedMs);
    const auto directory = std::filesystem::temp_directory_path() / ("anyps5-loading-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto path = directory / "status.work";
    WriteSnapshot(path, active);
    {
        std::ifstream input(path);
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        Require(json.find("\"active\":[1,1,1,1]") != std::string::npos);
        Require(json.find("\"completed\":0") != std::string::npos);
    }
    WriteSnapshot(path, finished);
    {
        std::ifstream input(path);
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        Require(json.find("\"active\":[0,0,0,0]") != std::string::npos);
        Require(json.find("\"completed\":4,\"failed\":2") != std::string::npos);
    }
    const auto base = directory / "live";
#ifdef _WIN32
    _wputenv_s(L"ANYPS5_LOADING_STATUS", base.c_str());
#else
    setenv("ANYPS5_LOADING_STATUS", base.c_str(), 1);
#endif
    const auto observe = [&](const std::string& expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            std::ifstream input(base.string() + ".work");
            const std::string json((std::istreambuf_iterator<char>(input)), {});
            if (json.find(expected) != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    {
        Scope step(Stage::Compute);
        Require(observe("\"active\":[0,0,0,1]"));
    }
    Require(observe("\"completed\":1,\"failed\":0"));
    std::filesystem::remove_all(directory);
}
