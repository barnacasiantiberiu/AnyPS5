#include "prx/libc/include/general/VabiMacros.hpp"
#include "tests/ModuleChecks.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceAudioInInit();
int APS5_VABI sceAudioInOpen(int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioInHqOpen(int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioInAsyncOpen(int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioInInput(int, void*);
int APS5_VABI sceAudioInGetSilentState(int);
int APS5_VABI sceAudioInIsPadEmbeddedMic(int);
int APS5_VABI sceAudioInClose(int);
}
using ModuleTest::Require;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
constexpr int InvalidHandle = static_cast<int>(0x80260101u);
constexpr int Busy = static_cast<int>(0x8026010au);
void Environment(const char* key, const std::string& value) {
#ifdef _WIN32
    Require(_putenv_s(key, value.c_str()) == 0);
#else
    Require((value.empty() ? unsetenv(key) : setenv(key, value.c_str(), 1)) == 0);
#endif
}
struct Recording {
    fs::path path = fs::current_path() / ("audio-modes-" + std::to_string(Clock::now().time_since_epoch().count()) + ".raw");
    std::vector<std::uint8_t> bytes;
    explicit Recording(std::uint32_t format) {
        const unsigned channels = format == 2 || format == 0x12 ? 2 : 1;
        const unsigned sampleBytes = format >= 0x10 ? 4 : 2;
        bytes.resize(1024 * channels * sampleBytes);
        for (unsigned i = 0; i < 1024 * channels; ++i) {
            if (sampleBytes == 4) {
                const float value = static_cast<float>(static_cast<int>(i % 100) - 50) / 64.0f;
                std::memcpy(bytes.data() + i * 4, &value, 4);
            } else {
                const auto value = static_cast<std::int16_t>(i * 9 - 4096);
                std::memcpy(bytes.data() + i * 2, &value, 2);
            }
        }
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        Require(out.good());
        Environment("SDL_AUDIODRIVER", "disk");
        Environment("SDL_DISKAUDIOFILEIN", path.string());
        Environment("SDL_DISKAUDIODELAY", "");
    }
    ~Recording() {
        Require(path.parent_path() == fs::current_path() && path.filename().string().starts_with("audio-modes-"));
        Require(fs::remove(path));
    }
};
void Stream(bool hq, std::uint32_t grain = 384, std::uint32_t frequency = 48000) {
    for (const std::uint32_t format : {1u, 2u, 0x10u, 0x11u, 0x12u}) {
        Recording recording(format);
        const unsigned frameBytes = (format >= 0x10 ? 4 : 2) * (format == 2 || format == 0x12 ? 2 : 1);
        const int port = hq ? sceAudioInHqOpen(1, 1, 0, 128, 48000, format) :
                             sceAudioInAsyncOpen(1, 1, 0, grain, frequency, format);
        Require(port > 0 && sceAudioInGetSilentState(port) == 0);
        Require(sceAudioInIsPadEmbeddedMic(port) == 0);
        std::vector<std::uint8_t> collected;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (collected.size() < recording.bytes.size() && Clock::now() < deadline) {
            std::vector<std::uint8_t> block(16 + 384 * frameBytes + 16, 0xcd);
            const int frames = sceAudioInInput(port, block.data() + 16);
            Require(frames >= 0 && frames <= 384);
            Require(std::all_of(block.begin(), block.begin() + 16, [](auto b) { return b == 0xcd; }));
            Require(std::all_of(block.begin() + 16 + frames * frameBytes, block.end(), [](auto b) { return b == 0xcd; }));
            const auto bytes = std::min<std::size_t>(frames * frameBytes, recording.bytes.size() - collected.size());
            collected.insert(collected.end(), block.begin() + 16, block.begin() + 16 + bytes);
            if (!hq && frames == 0) std::this_thread::yield();
        }
        if (collected != recording.bytes) {
            const auto mismatch = std::mismatch(collected.begin(), collected.end(), recording.bytes.begin(), recording.bytes.end());
            std::fprintf(stderr, "format=%u collected=%zu expected=%zu mismatch=%zu actual=%u expectedByte=%u\n", format,
                collected.size(), recording.bytes.size(), static_cast<std::size_t>(mismatch.first - collected.begin()),
                mismatch.first == collected.end() ? 999 : *mismatch.first, mismatch.second == recording.bytes.end() ? 999 : *mismatch.second);
        }
        Require(collected == recording.bytes);
        Require(sceAudioInInput(port, nullptr) == 0);
        Require(sceAudioInClose(port) == 0);
        Require(sceAudioInInput(port, nullptr) == InvalidHandle);
        Require(sceAudioInClose(port) == InvalidHandle);
    }
}
void Empty() {
    Environment("SDL_AUDIODRIVER", "none");
    std::array<std::uint8_t, 384 * 8 + 16> bytes;
    bytes.fill(0xb6);
    for (const auto length : {1u, 127u, 129u, 256u, 384u}) {
        const int port = sceAudioInAsyncOpen(1, 0, 0, length, 16000, 0x12);
        Require(port > 0 && sceAudioInGetSilentState(port) == 1);
        for (int i = 0; i < 200; ++i) Require(sceAudioInInput(port, bytes.data()) == 0);
        Require(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0xb6; }));
        Require(sceAudioInClose(port) == 0);
    }
    const int hq = sceAudioInHqOpen(1, 0, 0, 128, 48000, 0x10);
    Require(hq > 0);
    const auto start = Clock::now();
    for (int i = 0; i < 4; ++i) Require(sceAudioInInput(hq, bytes.data()) == 128);
    Require(Clock::now() - start >= std::chrono::microseconds(4 * 2666));
    Require(std::all_of(bytes.begin(), bytes.begin() + 512, [](auto b) { return b == 0; }));
    Require(std::all_of(bytes.begin() + 512, bytes.end(), [](auto b) { return b == 0xb6; }));
    Require(sceAudioInClose(hq) == 0);
    const int sync = sceAudioInOpen(1, 0, 0, 128, 16000, 0x10);
    Require(sync == hq);
    bytes.fill(0xb6);
    Require(sceAudioInInput(sync, bytes.data()) == 128);
    Require(std::all_of(bytes.begin(), bytes.begin() + 512, [](auto b) { return b == 0; }));
    Require(bytes[512] == 0xb6);
    Require(sceAudioInClose(sync) == 0);
}
void BusyPort() {
    Environment("SDL_AUDIODRIVER", "none");
    const int port = sceAudioInHqOpen(1, 0, 0, 128, 48000, 1);
    Require(port > 0);
    std::atomic<bool> stop{false}, finished{false};
    std::thread reader([&] {
        std::array<std::int16_t, 385> samples{};
        while (!stop) {
            const int count = sceAudioInInput(port, samples.data());
            Require(count == 128 || count == Busy);
        }
        finished = true;
    });
    bool observed = false;
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (!observed && Clock::now() < deadline) {
        observed = sceAudioInInput(port, nullptr) == Busy;
        if (!observed) std::this_thread::yield();
    }
    stop = true;
    reader.join();
    Require(observed && finished);
    Require(sceAudioInGetSilentState(port) == 1);
    Require(sceAudioInClose(port) == 0);
}
int main(int argc, char** argv) {
    Require(argc == 2 && sceAudioInInit() == 0);
    const std::string mode(argv[1]);
    if (mode == "hq") Stream(true);
    else if (mode == "async") Stream(false);
    else if (mode == "async_small") Stream(false, 1, 16000);
    else if (mode == "empty") Empty();
    else if (mode == "busy") BusyPort();
    else Require(false);
}
