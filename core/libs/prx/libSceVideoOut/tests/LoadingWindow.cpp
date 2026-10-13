#define SDL_MAIN_HANDLED
#include "prx/libSceVideoOut/include/LoadingWindow.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

void Require(bool condition) { if (!condition) throw std::runtime_error("loading window regression"); }

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("anyps5-window-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto base = directory / "status";
#ifdef _WIN32
    _wputenv_s(L"ANYPS5_LOADING_STATUS", base.c_str());
#else
    setenv("ANYPS5_LOADING_STATUS", base.c_str(), 1);
#endif
    SDL_SetMainReady();
    Require(SDL_Init(SDL_INIT_VIDEO) == 0);
    auto* window = SDL_CreateWindow("Loading window regression", 20, 20, 640, 480, SDL_WINDOW_HIDDEN);
    Require(window != nullptr);
    LoadingWindow loading;
    loading.Start(window);
    for (unsigned i = 0; i < 7; ++i) loading.Presented();
    bool observed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(base.string() + ".window");
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        if (json.find("\"frames\":7") != std::string::npos && json.find("\"width\":640") != std::string::npos && json.find("\"height\":480") != std::string::npos) {
            Require(json.find("\"focused\":false") != std::string::npos);
            observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    loading.Stop();
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::filesystem::remove_all(directory);
    Require(observed);
}
