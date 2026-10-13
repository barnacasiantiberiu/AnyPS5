#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int StatusNone = 0;
constexpr int StatusInitialized = 1;
constexpr int StatusRunning = 2;
constexpr int StatusFinished = 3;
constexpr int ErrorNotInitialized = static_cast<int>(0x80b80003u);
constexpr int ErrorAlreadyInitialized = static_cast<int>(0x80b80004u);
constexpr int ErrorNotFinished = static_cast<int>(0x80b80005u);
constexpr int ErrorBusy = static_cast<int>(0x80b80008u);
constexpr int ErrorInvalidParameter = static_cast<int>(0x80b8000au);
constexpr int ErrorNotRunning = static_cast<int>(0x80b8000bu);
constexpr int ErrorNullArgument = static_cast<int>(0x80b8000du);

struct CommerceParameter {
    std::uint64_t baseSize;
    std::uint8_t baseReserved[40];
    std::uint32_t size;
    std::int32_t userId;
    std::int32_t mode;
    std::uint32_t serviceLabel;
    const char* const* targets;
    std::uint32_t targetCount;
    std::uint32_t padding;
    std::uint64_t features;
    std::uint64_t userData;
    std::uint8_t reserved[32];
};
static_assert(sizeof(CommerceParameter) == 0x80 && offsetof(CommerceParameter, size) == 0x30);
static_assert(offsetof(CommerceParameter, mode) == 0x38 && offsetof(CommerceParameter, userData) == 0x58);

struct CommerceResult {
    std::int32_t result;
    std::uint8_t authorized;
    std::uint8_t padding[3];
    std::uint64_t userData;
    std::uint8_t reserved[32];
};
static_assert(sizeof(CommerceResult) == 0x30 && offsetof(CommerceResult, userData) == 8);

struct Dialog {
    std::mutex mutex;
    int status = StatusNone;
    std::uint64_t userData = 0;
    bool hasRequest = false;
};

Dialog& State() {
    static Dialog dialog;
    return dialog;
}

int Open(const void* parameter, std::uint32_t requiredSize) {
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    if (dialog.status == StatusNone) return ErrorNotInitialized;
    if (dialog.status == StatusRunning) return ErrorBusy;
    if (!parameter) return ErrorNullArgument;
    std::uint64_t baseSize;
    std::memcpy(&baseSize, parameter, sizeof(baseSize));
    if (baseSize < 0x30) return ErrorInvalidParameter;
    std::uint32_t size;
    std::memcpy(&size, static_cast<const std::uint8_t*>(parameter) + offsetof(CommerceParameter, size), sizeof(size));
    if (size < requiredSize) return ErrorInvalidParameter;
    CommerceParameter request;
    std::memcpy(&request, parameter, sizeof(request));
    if (request.mode < 0 || request.mode > 5) return ErrorInvalidParameter;
    dialog.userData = request.userData;
    dialog.hasRequest = true;
    dialog.status = StatusRunning;
    return 0;
}

}

extern "C" {

int APS5_VABI sceNpCommerceDialogInitialize() {
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    if (dialog.status != StatusNone) return ErrorAlreadyInitialized;
    dialog.status = StatusInitialized;
    dialog.userData = 0;
    dialog.hasRequest = false;
    return 0;
}

int APS5_VABI sceNpCommerceDialogOpen(const void* param) {
    return Open(param, 0x80);
}

int APS5_VABI sceNpCommerceDialogOpen2(const void* param) {
    return Open(param, 0x88);
}

int APS5_VABI sceNpCommerceDialogUpdateStatus() {
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    if (dialog.status == StatusRunning) dialog.status = StatusFinished;
    return dialog.status;
}

int APS5_VABI sceNpCommerceDialogGetStatus() {
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    return dialog.status;
}

int APS5_VABI sceNpCommerceDialogGetResult(void* result) {
    if (!result) return ErrorNullArgument;
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    if (dialog.status != StatusFinished || !dialog.hasRequest) return ErrorNotFinished;
    const CommerceResult value{1, 0, {}, dialog.userData, {}};
    std::memcpy(result, &value, sizeof(value));
    return 0;
}

int APS5_VABI sceNpCommerceDialogClose() {
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    if (dialog.status == StatusNone) return ErrorNotInitialized;
    if (!dialog.hasRequest) return ErrorNotRunning;
    dialog.status = StatusFinished;
    return 0;
}

int APS5_VABI sceNpCommerceDialogTerminate() {
    auto& dialog = State();
    std::lock_guard lock(dialog.mutex);
    if (dialog.status == StatusNone) return ErrorNotInitialized;
    dialog.status = StatusNone;
    dialog.userData = 0;
    dialog.hasRequest = false;
    return 0;
}

int APS5_VABI sceNpCommerceHidePsStoreIcon() {
    return 0;
}

int APS5_VABI sceNpCommerceShowPsStoreIcon(int pos) {
    (void)pos;
    return 0;
}

int APS5_VABI sceNpCommerceSetPsStoreIconLayout(int layout) {
    (void)layout;
    return 0;
}

}
