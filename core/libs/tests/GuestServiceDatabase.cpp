#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libScePosixForWebKit/GuestResolver.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <thread>
#include <array>
#include <barrier>
#include <stdexcept>
#include <string>

extern "C" GuestResolver::ServiceEntry* APS5_VABI getservbyname_nid_postfix(const char*, const char*);

void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Service database check failed at line %u\n", location.line());
        std::abort();
    }
}

bool NetworkPort(const GuestResolver::ServiceEntry* entry, std::uint16_t port) {
    if (entry == nullptr) return false;
    unsigned char bytes[4];
    std::memcpy(bytes, &entry->port, sizeof(bytes));
    return bytes[0] == port >> 8 && bytes[1] == (port & 0xff) && bytes[2] == 0 && bytes[3] == 0;
}

int main() {
    auto* http = getservbyname_nid_postfix("http", "tcp");
    Require(http && std::strcmp(http->name, "http") == 0 && std::strcmp(http->protocol, "tcp") == 0);
    Require(NetworkPort(http, 80));
    Require(http->aliases && std::strcmp(http->aliases[0], "www") == 0);
    Require(std::strcmp(http->aliases[1], "www-http") == 0 && http->aliases[2] == nullptr);

    auto* https = getservbyname_nid_postfix("https", nullptr);
    Require(https && std::strcmp(https->name, "https") == 0 && std::strcmp(https->protocol, "tcp") == 0);
    Require(NetworkPort(https, 443) && https->aliases && https->aliases[0] == nullptr);

    auto* domain = getservbyname_nid_postfix("domain", "udp");
    Require(domain && std::strcmp(domain->name, "domain") == 0 && std::strcmp(domain->protocol, "udp") == 0);
    Require(NetworkPort(domain, 53));

    auto* www = getservbyname_nid_postfix("www", "sctp");
    Require(www && std::strcmp(www->name, "http") == 0 && std::strcmp(www->protocol, "sctp") == 0);
    Require(NetworkPort(www, 80));

    auto* mail = getservbyname_nid_postfix("mail", nullptr);
    Require(mail && std::strcmp(mail->name, "smtp") == 0 && NetworkPort(mail, 25));

    errno = 0;
    Require(getservbyname_nid_postfix("no-such-service", nullptr) == nullptr && errno == 0);
    Require(getservbyname_nid_postfix("domain", "sctp") == nullptr);
    Require(getservbyname_nid_postfix("ntp", "TCP") == nullptr);
    Require(getservbyname_nid_postfix("HTTP", "tcp") == nullptr);

    auto* ntp = getservbyname_nid_postfix("ntp", "udp");
    Require(ntp && NetworkPort(ntp, 123));
    std::thread([] {
        auto* ssh = getservbyname_nid_postfix("ssh", "tcp");
        Require(ssh && std::strcmp(ssh->name, "ssh") == 0 && NetworkPort(ssh, 22));
    }).join();
    Require(std::strcmp(ntp->name, "ntp") == 0 && std::strcmp(ntp->protocol, "udp") == 0 && NetworkPort(ntp, 123));
    const std::pair<const char*, std::uint16_t> expected[]{
        {"ftp-data", 20}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23},
        {"smtp", 25}, {"domain", 53}, {"tftp", 69}, {"http", 80},
        {"pop3", 110}, {"ntp", 123}, {"imap", 143}, {"https", 443},
        {"rtsp", 554}, {"submission", 587}, {"imaps", 993}, {"pop3s", 995}};
    for (const auto& [name, port] : expected) {
        for (const char* protocol : {"tcp", "udp"}) {
            const auto* entry = getservbyname_nid_postfix(name, protocol);
            Require(entry && std::strcmp(entry->name, name) == 0 && NetworkPort(entry, port));
            Require(std::strcmp(entry->protocol, protocol) == 0);
        }
    }
    auto* imap = getservbyname_nid_postfix("imap4", "udp");
    Require(imap && std::strcmp(imap->name, "imap") == 0 && NetworkPort(imap, 143));
    Require(std::strcmp(imap->aliases[0], "imap2") == 0 && std::strcmp(imap->aliases[1], "imap4") == 0 && imap->aliases[2] == nullptr);
    Require(NetworkPort(getservbyname_nid_postfix("spop3", nullptr), 995));
    for (const char* missing : {"", "0", "65536", "HTTPS", " http", "http "}) {
        errno = 73;
        Require(getservbyname_nid_postfix(missing, nullptr) == nullptr && errno == 73);
    }
    bool rejected = false;
    try { getservbyname_nid_postfix(nullptr, nullptr); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected);

    std::barrier ready(4);
    std::array<std::thread, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            const auto [name, port] = expected[index];
            for (int round = 0; round < 100; ++round) {
                auto* entry = getservbyname_nid_postfix(name, "tcp");
                Require(entry && NetworkPort(entry, port));
                ready.arrive_and_wait();
                Require(std::strcmp(entry->name, name) == 0 && NetworkPort(entry, port));
                Require(entry->aliases && entry->aliases[0] == nullptr);
                ready.arrive_and_wait();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    return 0;
}
