#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int APS5_VABI sceSaveDataInitialize3(const void*);
extern "C" int APS5_VABI sceSaveDataTerminate();
extern "C" int APS5_VABI sceSaveDataMount3(const SaveDataMount3*, SaveDataMountResult*);
extern "C" int APS5_VABI sceSaveDataUmount2(std::uint32_t, const SaveDataMountPoint*);
extern "C" int APS5_VABI sceSaveDataSaveIcon(const SaveDataMountPoint*, const SaveDataIcon*);
extern "C" int APS5_VABI sceSaveDataLoadIcon(const SaveDataMountPoint*, SaveDataIcon*);
extern "C" int APS5_VABI sceSaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2*, SaveDataMemorySetupResult*);
extern "C" int APS5_VABI sceSaveDataGetSaveDataMemory2(SaveDataMemoryGet2*);
extern "C" int APS5_VABI sceSaveDataSetSaveDataMemory2(const SaveDataMemorySet2*);

namespace {

constexpr int SaveDataErrorParameter = -2137063424;
constexpr int SaveDataErrorNotMounted = -2137063420;
constexpr int SaveDataErrorNotFound = -2137063416;
constexpr int SaveDataErrorBadMounted = -2137063411;
constexpr int SaveDataErrorInternal = -2137063413;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "savedata icon check failed: %s\n", what.c_str());
    ++failures;
}

std::vector<char> PngBytes(char seed, std::size_t size) {
    return std::vector<char>(size, seed);
}

SaveDataIcon IconOf(std::vector<char>& storage) {
    SaveDataIcon icon{};
    icon.buf = storage.data();
    icon.buf_size = storage.size();
    icon.data_size = storage.size();
    return icon;
}

void TestMountedIconRoundTrip() {
    SaveDataMount3 mount{};
    mount.user_id = 1;
    SceSaveDataDirName dir{};
    std::strcpy(dir.data, "iconsave");
    mount.dir_name = &dir;
    mount.mount_mode = 0x22;
    SaveDataMountResult mounted{};
    Check(sceSaveDataMount3(&mount, &mounted) == 0, "rdwr mount succeeds");

    SaveDataMountPoint point{};
    std::memcpy(point.data, mounted.mount_point.data, sizeof(point.data));

    std::vector<char> sink(64);
    SaveDataIcon loader = IconOf(sink);
    Check(sceSaveDataLoadIcon(&point, &loader) == SaveDataErrorNotFound, "load without an icon returns NOT_FOUND");

    const std::vector<char> bytes = PngBytes('i', 100);
    std::vector<char> source = bytes;
    SaveDataIcon saver = IconOf(source);
    Check(sceSaveDataSaveIcon(&point, &saver) == 0, "save icon succeeds");
    Check(std::filesystem::is_regular_file(std::filesystem::path("_sd/iconsave/sce_sys/icon0.png")),
          "icon lands at sce_sys/icon0.png inside the save directory");

    std::vector<char> loaded(256);
    SaveDataIcon reader = IconOf(loaded);
    Check(sceSaveDataLoadIcon(&point, &reader) == 0, "load icon succeeds");
    Check(reader.data_size == bytes.size(), "load reports the full file size");
    Check(std::memcmp(loaded.data(), bytes.data(), bytes.size()) == 0, "loaded bytes match the saved bytes");

    std::vector<char> narrow(10);
    SaveDataIcon narrow_reader = IconOf(narrow);
    Check(sceSaveDataLoadIcon(&point, &narrow_reader) == 0, "load with a small buffer succeeds");
    Check(narrow_reader.data_size == bytes.size(), "small-buffer load still reports the full size");
    Check(std::memcmp(narrow.data(), bytes.data(), narrow.size()) == 0, "small-buffer load fills buf_size bytes");

    std::array<char, 24> guarded;
    guarded.fill('G');
    SaveDataIcon guardedReader{};
    guardedReader.buf = guarded.data() + 8;
    guardedReader.buf_size = 8;
    Check(sceSaveDataLoadIcon(&point, &guardedReader) == 0, "guarded load succeeds");
    Check(guardedReader.data_size == bytes.size(), "guarded load reports full size");
    Check(std::all_of(guarded.begin(), guarded.begin() + 8, [](char c) { return c == 'G'; }) &&
          std::all_of(guarded.end() - 8, guarded.end(), [](char c) { return c == 'G'; }), "load respects both buffer guards");
    Check(std::memcmp(guarded.data() + 8, bytes.data(), 8) == 0, "guarded load fills permitted bytes");
    guardedReader.buf_size = 0;
    const auto before = guarded;
    Check(sceSaveDataLoadIcon(&point, &guardedReader) == 0 && guardedReader.data_size == bytes.size(), "zero capacity reports full size");
    Check(guarded == before, "zero capacity writes no bytes");

    Check(sceSaveDataUmount2(0, &point) == 0, "umount succeeds");
    mount.mount_mode = 1;
    Check(sceSaveDataMount3(&mount, &mounted) == 0, "existing save remounts read-only");
    point = mounted.mount_point;
    Check(sceSaveDataLoadIcon(&point, &reader) == 0 && reader.data_size == bytes.size(), "icon persists across remount");
    Check(sceSaveDataSaveIcon(&point, &saver) == SaveDataErrorBadMounted, "existing read-only icon cannot be replaced");
    Check(sceSaveDataUmount2(0, &point) == 0, "read-only remount closes");
    mount.mount_mode = 2;
    Check(sceSaveDataMount3(&mount, &mounted) == 0, "reused mount slot becomes writable again");
    point = mounted.mount_point;
    saver.data_size = 3;
    Check(sceSaveDataSaveIcon(&point, &saver) == 0, "short replacement succeeds");
    Check(sceSaveDataLoadIcon(&point, &reader) == 0 && reader.data_size == 3, "replacement truncates old icon");
    saver.data_size = source.size() + 100;
    Check(sceSaveDataSaveIcon(&point, &saver) == 0, "save bounds data size by capacity");
    Check(sceSaveDataLoadIcon(&point, &reader) == 0 && reader.data_size == source.size(), "save consumes only input capacity");
    const auto blocked = std::filesystem::path("_sd/iconsave/sce_sys/icon0.png.tmp");
    Check(std::filesystem::create_directory(blocked), "create blocked replacement path");
    std::ofstream(blocked / "keep") << "block";
    Check(sceSaveDataSaveIcon(&point, &saver) == SaveDataErrorInternal, "failed replacement reports INTERNAL");
    Check(sceSaveDataLoadIcon(&point, &reader) == 0 && reader.data_size == bytes.size(), "failed replacement preserves original size");
    Check(std::memcmp(loaded.data(), bytes.data(), bytes.size()) == 0, "failed replacement preserves original bytes");
    Check(sceSaveDataUmount2(0, &point) == 0, "replacement test closes mount");
}

void TestParameterChecks() {
    SaveDataMount3 mount{};
    mount.user_id = 1;
    SceSaveDataDirName dir{};
    std::strcpy(dir.data, "iconargs");
    mount.dir_name = &dir;
    mount.mount_mode = 0x22;
    SaveDataMountResult mounted{};
    Check(sceSaveDataMount3(&mount, &mounted) == 0, "mount for argument checks succeeds");
    SaveDataMountPoint point{};
    std::memcpy(point.data, mounted.mount_point.data, sizeof(point.data));

    const std::vector<char> bytes = PngBytes('a', 8);
    std::vector<char> source = bytes;
    SaveDataIcon saver = IconOf(source);
    Check(sceSaveDataSaveIcon(&point, nullptr) == SaveDataErrorParameter, "save with a null icon returns PARAMETER");
    saver.buf = nullptr;
    Check(sceSaveDataSaveIcon(&point, &saver) == SaveDataErrorParameter, "save with a null buffer returns PARAMETER");
    SaveDataMountPoint missing{};
    std::strcpy(missing.data, "/savedata15");
    Check(sceSaveDataSaveIcon(&missing, nullptr) == SaveDataErrorNotMounted, "save on an unmounted point returns NOT_MOUNTED");

    SaveDataIcon reader = IconOf(source);
    Check(sceSaveDataLoadIcon(&point, nullptr) == SaveDataErrorParameter, "load with a null icon returns PARAMETER");
    Check(sceSaveDataLoadIcon(&point, &reader) == SaveDataErrorNotFound, "load after failed saves still returns NOT_FOUND");

    Check(sceSaveDataUmount2(0, &point) == 0, "umount after argument checks succeeds");
}

void TestReadOnlyMount() {
    SaveDataMount3 mount{};
    mount.user_id = 1;
    SceSaveDataDirName dir{};
    std::strcpy(dir.data, "iconrdonly");
    mount.dir_name = &dir;
    mount.mount_mode = 0x21;
    SaveDataMountResult mounted{};
    Check(sceSaveDataMount3(&mount, &mounted) == 0, "rdonly mount succeeds");
    SaveDataMountPoint point{};
    std::memcpy(point.data, mounted.mount_point.data, sizeof(point.data));

    const std::vector<char> bytes = PngBytes('r', 8);
    std::vector<char> source = bytes;
    SaveDataIcon saver = IconOf(source);
    Check(sceSaveDataSaveIcon(&point, &saver) == SaveDataErrorBadMounted, "save on a read-only mount returns BAD_MOUNTED");

    std::vector<char> loaded(64);
    SaveDataIcon reader = IconOf(loaded);
    Check(sceSaveDataLoadIcon(&point, &reader) == SaveDataErrorNotFound, "a rejected save writes no icon");

    Check(sceSaveDataUmount2(0, &point) == 0, "rdonly umount succeeds");
}

void TestSaveDataMemoryIcon() {
    const std::int32_t user = 1;
    const std::uint32_t slot = 2;
    const std::vector<char> seeded = PngBytes('s', 24);

    SaveDataMemorySetup2 setup{};
    setup.user_id = user;
    setup.slot_id = slot;
    setup.memory_size = 1024;
    std::vector<char> icon_storage = seeded;
    SaveDataIcon init_icon = IconOf(icon_storage);
    setup.init_icon = &init_icon;
    SaveDataMemorySetupResult setup_result{};
    Check(sceSaveDataSetupSaveDataMemory2(&setup, &setup_result) == 0, "setup with an init icon succeeds");
    Check(setup_result.existed_memory_size == 0, "a fresh memory slot reports no existing size");
    Check(std::filesystem::is_regular_file(std::filesystem::path("_sd_mem/u1/slot2.icon")),
          "the init icon lands in the memory slot sidecar");

    std::vector<char> loaded(64);
    SaveDataMemoryGet2 get{};
    get.user_id = user;
    get.slot_id = slot;
    SaveDataIcon reader = IconOf(loaded);
    get.icon = &reader;
    Check(sceSaveDataGetSaveDataMemory2(&get) == 0, "get with an icon succeeds");
    Check(reader.data_size == seeded.size(), "get reports the seeded icon size");
    Check(std::memcmp(loaded.data(), seeded.data(), seeded.size()) == 0, "get returns the seeded icon bytes");

    SaveDataMemoryGet2 probe{};
    probe.user_id = user;
    probe.slot_id = slot;
    SaveDataIcon probe_icon{};
    probe.icon = &probe_icon;
    Check(sceSaveDataGetSaveDataMemory2(&probe) == 0, "get with a null icon buffer succeeds");
    Check(probe_icon.data_size == 0, "get with a null icon buffer reports zero");

    const std::vector<char> replaced = PngBytes('n', 48);
    std::vector<char> next_storage = replaced;
    SaveDataIcon next_icon = IconOf(next_storage);
    SaveDataMemorySet2 set{};
    set.user_id = user;
    set.slot_id = slot;
    set.icon = &next_icon;
    set.data_num = 0;
    Check(sceSaveDataSetSaveDataMemory2(&set) == 0, "set with an icon succeeds");
    reader = IconOf(loaded);
    get.icon = &reader;
    Check(sceSaveDataGetSaveDataMemory2(&get) == 0, "get after overwriting succeeds");
    Check(reader.data_size == replaced.size(), "get reports the replaced icon size");
    Check(std::memcmp(loaded.data(), replaced.data(), replaced.size()) == 0, "get returns the replaced bytes");

    SaveDataIcon empty_icon{};
    set.icon = &empty_icon;
    Check(sceSaveDataSetSaveDataMemory2(&set) == 0, "set with an empty icon succeeds");
    Check(!std::filesystem::exists(std::filesystem::path("_sd_mem/u1/slot2.icon")),
          "an empty icon set drops the sidecar file");
    reader = IconOf(loaded);
    get.icon = &reader;
    Check(sceSaveDataGetSaveDataMemory2(&get) == 0, "get after dropping the icon succeeds");
    Check(reader.data_size == 0, "get after dropping the icon reports zero");
}

void TestIconIoFailures() {
    std::vector<char> bytes(12, 'F');
    auto icon = IconOf(bytes);
    SaveDataMemorySetup2 setup{};
    setup.user_id = 8;
    setup.slot_id = 5;
    setup.memory_size = 32;
    setup.init_icon = &icon;
    SaveDataMemorySetupResult result{};
    icon.buf = nullptr;
    Check(sceSaveDataSetupSaveDataMemory2(&setup, &result) == SaveDataErrorParameter, "nonempty initial icon requires input buffer");
    Check(!std::filesystem::exists("_sd_mem/u8/slot5.bin"), "invalid initial icon creates no memory");
    icon.buf = bytes.data();
    const auto blocked = std::filesystem::path("_sd_mem/u8/slot5.icon");
    Check(std::filesystem::create_directories(blocked), "create blocked memory icon path");
    std::ofstream(blocked / "keep") << "block";
    Check(sceSaveDataSetupSaveDataMemory2(&setup, &result) == SaveDataErrorInternal, "setup propagates icon write failure");
    Check(!std::filesystem::exists("_sd_mem/u8/slot5.bin"), "failed initial icon does not publish memory blob");
    Check(std::filesystem::remove(blocked / "keep") && std::filesystem::remove(blocked), "unblock memory icon path");
    Check(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0, "setup can retry after failed icon write");
    std::vector<char> loaded(20, 'X');
    auto reader = IconOf(loaded);
    SaveDataMemoryGet2 get{};
    get.user_id = setup.user_id;
    get.slot_id = setup.slot_id;
    get.icon = &reader;
    Check(sceSaveDataGetSaveDataMemory2(&get) == 0 && reader.data_size == bytes.size(), "retried setup stores icon");
    Check(std::memcmp(loaded.data(), bytes.data(), bytes.size()) == 0, "retried icon bytes match");
    Check(std::all_of(loaded.begin() + bytes.size(), loaded.end(), [](char c) { return c == 'X'; }), "memory icon leaves tail untouched");
    SaveDataMemorySet2 invalid{};
    invalid.user_id = setup.user_id;
    invalid.slot_id = setup.slot_id;
    invalid.icon = &icon;
    icon.buf = nullptr;
    Check(sceSaveDataSetSaveDataMemory2(&invalid) == SaveDataErrorParameter, "nonempty replacement requires input buffer");
    Check(sceSaveDataGetSaveDataMemory2(&get) == 0 && reader.data_size == bytes.size(), "invalid replacement preserves existing icon");
    Check(std::filesystem::remove(blocked) && std::filesystem::create_directory(blocked), "block deletion with directory");
    std::ofstream(blocked / "keep") << "block";
    SaveDataIcon empty{};
    SaveDataMemorySet2 set{};
    set.user_id = setup.user_id;
    set.slot_id = setup.slot_id;
    set.icon = &empty;
    Check(sceSaveDataSetSaveDataMemory2(&set) == SaveDataErrorInternal, "failed icon removal reports INTERNAL");
    Check(std::filesystem::exists(blocked / "keep"), "failed removal preserves data");
}

}

int main() {
    const auto previous = std::filesystem::current_path();
    const auto temporary = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto root = temporary /
        ("anyps5-savedata-icon-" + std::to_string(std::random_device{}()));
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("test directory already exists");
    std::filesystem::current_path(root);
    int status = 0;
    try {
        Check(sceSaveDataInitialize3(nullptr) == 0, "initialize succeeds");
        TestMountedIconRoundTrip();
        TestParameterChecks();
        TestReadOnlyMount();
        TestSaveDataMemoryIcon();
        TestIconIoFailures();
        Check(sceSaveDataTerminate() == 0, "terminate succeeds");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        status = 1;
    }
    std::filesystem::current_path(previous);
    std::error_code error;
    if (root.parent_path() != temporary || !root.filename().string().starts_with("anyps5-savedata-icon-"))
        throw std::runtime_error("unsafe test cleanup path");
    std::filesystem::remove_all(root, error);
    Check(!error, "temporary test directory removed");
    if (failures != 0) return 1;
    std::printf("savedata icon tests passed\n");
    return status;
}
