#pragma once

#include "SDL.h"
#include <atomic>
#include <thread>

class LoadingWindow {
public:
    void Start(SDL_Window* window);
    void Stop() { worker = {}; }
    void Presented() { frames.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<unsigned long long> frames{0};
    std::jthread worker;
};
