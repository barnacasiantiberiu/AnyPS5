#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
extern "C" {
int APS5_VABI getsid_nid_postfix(int);
int APS5_VABI getpid_nid_postfix();
int* APS5_VABI __error_nid_postfix();
}
static void Require(bool value) { if (!value) std::abort(); }
int main() {
    const int self = getpid_nid_postfix();
    *__error_nid_postfix() = 73;
#ifdef _WIN32
    Require(self == static_cast<int>(GetCurrentProcessId()));
    Require(getsid_nid_postfix(0) == self);
#else
    Require(getsid_nid_postfix(0) == ::getsid(0));
#endif
    Require(getsid_nid_postfix(self) == getsid_nid_postfix(0));
    Require(*__error_nid_postfix() == 73);
    Require(getsid_nid_postfix(-1) == -1);
    Require(*__error_nid_postfix() == 3);
}
