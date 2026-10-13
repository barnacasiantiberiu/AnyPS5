#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "prx/libc/include/general/VabiMacros.hpp"
#include "tests/ModuleChecks.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
int APS5_VABI sceNetInit_nid_postfix();
int APS5_VABI sceNetShowNetstat();
int APS5_VABI sceNetShowIfconfig();
int APS5_VABI sceNetSocket(const char*, int, int, int);
int APS5_VABI sceNetSocketClose(int);
int APS5_VABI sceNetBind_nid_postfix(int, const void*, std::uint32_t);
int APS5_VABI sceNetListen(int, int);
int APS5_VABI sceNetGetsockname(int, void*, std::uint32_t*);
int APS5_VABI sceNetSetsockopt(int, int, int, const void*, std::uint32_t);
int APS5_VABI sceNetConnect(int, const void*, std::uint32_t);
int APS5_VABI sceNetAccept(int, void*, std::uint32_t*);
std::int64_t APS5_VABI sceNetSend(int, const void*, std::size_t, int);
std::int64_t APS5_VABI sceNetRecv(int, void*, std::size_t, int);
}

using ModuleTest::Require;

template<class TFunction>
std::string Capture(TFunction function) {
    auto* file = std::tmpfile();
    Require(file != nullptr && std::fflush(stdout) == 0);
#ifdef _WIN32
    const int output = _fileno(stdout);
    const int saved = _dup(output);
    Require(saved >= 0 && _dup2(_fileno(file), output) == 0);
#else
    const int output = fileno(stdout);
    const int saved = dup(output);
    Require(saved >= 0 && dup2(fileno(file), output) == output);
#endif
    const int result = function();
    Require(std::fflush(stdout) == 0);
#ifdef _WIN32
    Require(_dup2(saved, output) == 0 && _close(saved) == 0);
#else
    Require(dup2(saved, output) == output && close(saved) == 0);
#endif
    Require(result == 0 && std::fseek(file, 0, SEEK_SET) == 0);
    std::string text;
    std::array<char, 256> bytes{};
    for (std::size_t count; (count = std::fread(bytes.data(), 1, bytes.size(), file)) != 0;) text.append(bytes.data(), count);
    Require(std::ferror(file) == 0 && std::fclose(file) == 0);
    return text;
}

void Netstat() {
    Require(Capture(sceNetShowNetstat).find("sockets=0") != std::string::npos);
    const int listener = sceNetSocket("diagnostic-listener", 2, 1, 0);
    Require(listener >= 0);
    const std::string row = "socket=" + std::to_string(listener) + " family=2 type=1";
    auto text = Capture(sceNetShowNetstat);
    Require(text.find(row + " nonblock=0 bound=0 listening=0") != std::string::npos);
    std::array<std::uint8_t, 16> address{16, 2, 0, 0, 127, 0, 0, 1};
    Require(sceNetBind_nid_postfix(listener, address.data(), address.size()) == 0);
    Require(sceNetListen(listener, 1) == 0);
    std::uint32_t size = address.size();
    Require(sceNetGetsockname(listener, address.data(), &size) == 0 && size == address.size());
    const auto savedAddress = address;
    const int nonblocking = 1;
    Require(sceNetSetsockopt(listener, 0xffff, 0x1200, &nonblocking, sizeof(nonblocking)) == 0);
    text = Capture(sceNetShowNetstat);
    Require(text.find("sockets=1") != std::string::npos);
    Require(text.find(row + " nonblock=1 bound=1 listening=1") != std::string::npos);
    const unsigned port = unsigned(address[2]) * 256 + address[3];
    Require(port != 0 && text.find("local=127.0.0.1:" + std::to_string(port)) != std::string::npos);
    Require(sceNetGetsockname(listener, address.data(), &size) == 0 && address == savedAddress);
    const int client = sceNetSocket("diagnostic-client", 2, 1, 0);
    Require(client >= 0 && sceNetConnect(client, address.data(), size) == 0);
    const int accepted = sceNetAccept(listener, nullptr, nullptr);
    Require(accepted >= 0);
    text = Capture(sceNetShowNetstat);
    Require(text.find("sockets=3") != std::string::npos);
    Require(text.find("peer=127.0.0.1:" + std::to_string(port)) != std::string::npos);
    const char payload[] = "diagnostics preserve transport";
    Require(sceNetSend(client, payload, sizeof(payload), 0) == sizeof(payload));
    std::array<char, sizeof(payload)> received{};
    Require(sceNetRecv(accepted, received.data(), received.size(), 0) == sizeof(payload));
    Require(std::memcmp(payload, received.data(), sizeof(payload)) == 0);
    for (int descriptor : {client, accepted, listener}) Require(sceNetSocketClose(descriptor) == 0);
    Require(Capture(sceNetShowNetstat).find("sockets=0") != std::string::npos);
    const int datagram = sceNetSocket("diagnostic-udp", 2, 2, 0);
    Require(datagram >= 0);
    Require(Capture(sceNetShowNetstat).find("socket=" + std::to_string(datagram) + " family=2 type=2") != std::string::npos);
    Require(sceNetSocketClose(datagram) == 0);
    const int ipv6 = sceNetSocket("diagnostic-ipv6", 28, 2, 0);
    Require(ipv6 >= 0);
    std::array<std::uint8_t, 28> address6{28, 28};
    address6[23] = 1;
    Require(sceNetBind_nid_postfix(ipv6, address6.data(), address6.size()) == 0);
    text = Capture(sceNetShowNetstat);
    Require(text.find("socket=" + std::to_string(ipv6) + " family=28 type=2") != std::string::npos);
    Require(text.find("local=[::1]:") != std::string::npos);
    Require(sceNetSocketClose(ipv6) == 0);
    Require(Capture(sceNetShowNetstat).find("sockets=0") != std::string::npos);
}

void Ifconfig() {
    for (int iteration = 0; iteration != 2; ++iteration) {
        const auto text = Capture(sceNetShowIfconfig);
        Require(text.starts_with("sceNet ifconfig:"));
        Require(text.find("up=1 loopback=1 family=2 address=127.0.0.1 prefix=8") != std::string::npos);
    }
}

int main(int argc, char** argv) {
    Require(argc == 2 && sceNetInit_nid_postfix() == 0);
    if (std::strcmp(argv[1], "netstat") == 0) Netstat();
    else {
        Require(std::strcmp(argv[1], "ifconfig") == 0);
        Ifconfig();
    }
}
