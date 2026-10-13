#pragma once

#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

struct AppContentAddcontInfo {
    NpUnifiedEntitlementLabel entitlement_label;
    std::uint32_t status;
};

struct AppContentAddcontDownloadProgress {
    std::uint64_t dataSize;
    std::uint64_t downloadedSize;
};

static_assert(sizeof(AppContentAddcontInfo) == 24);
static_assert(sizeof(AppContentAddcontDownloadProgress) == 16);

extern "C" {
int APS5_VABI sceAppContentAddcontMount(std::uint32_t, const NpUnifiedEntitlementLabel*, AppContentMountPoint*);
int APS5_VABI sceAppContentAddcontUnmount(const AppContentMountPoint*);
int APS5_VABI sceAppContentGetAddcontInfo(std::uint32_t, const NpUnifiedEntitlementLabel*, AppContentAddcontInfo*);
int APS5_VABI sceAppContentGetAddcontInfoList(std::uint32_t, AppContentAddcontInfo*, std::uint32_t, std::uint32_t*);
int APS5_VABI sceAppContentAddcontEnqueueDownload(std::uint32_t, const NpUnifiedEntitlementLabel*);
int APS5_VABI sceAppContentGetAddcontDownloadProgress(std::uint32_t, const NpUnifiedEntitlementLabel*, AppContentAddcontDownloadProgress*);
}
