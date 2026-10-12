#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
extern "C" {
int APS5_VABI wait_nid_postfix(int*);
int APS5_VABI waitpid_nid_postfix(int, int*, int);
int APS5_VABI getpid_nid_postfix(void);
int* APS5_VABI __error_nid_postfix();
}
constexpr int noChild = 10;
constexpr int invalidArgument = 22;
constexpr int untouched = 0x5a5a5a5a;
static void Require(bool value) { if (!value) std::abort(); }
static void RequireFailure(int pid, int options, int error) {
    int status = untouched;
    *__error_nid_postfix() = 0;
    Require(waitpid_nid_postfix(pid, &status, options) == -1);
    Require(*__error_nid_postfix() == error);
    Require(status == untouched);
    *__error_nid_postfix() = 0;
    Require(waitpid_nid_postfix(pid, nullptr, options) == -1);
    Require(*__error_nid_postfix() == error);
}
int main() {
    const int self = getpid_nid_postfix();
    for (int pid : {-1, 0, 1, self, self + 1, -self}) {
        for (int options : {0, 1, 16})
            RequireFailure(pid, options, noChild);
        for (int options : {0x40, 0x41, 0x80, 0x100, 0x40000000, static_cast<int>(0x80000040u), static_cast<int>(0xffffffffu)})
            RequireFailure(pid, options, invalidArgument);
    }
    for (int options : {2, 4, 32, static_cast<int>(0x80000000u)}) {
        int value = untouched;
        bool rejected = false;
        try { waitpid_nid_postfix(-1, &value, options); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && value == untouched);
    }
#ifdef _WIN32
    RequireFailure(-1, 8, noChild);
#endif
    int status = untouched;
    *__error_nid_postfix() = invalidArgument;
    Require(wait_nid_postfix(&status) == -1);
    Require(*__error_nid_postfix() == noChild);
    Require(status == untouched);
    *__error_nid_postfix() = invalidArgument;
    Require(wait_nid_postfix(nullptr) == -1);
    Require(*__error_nid_postfix() == noChild);
}
