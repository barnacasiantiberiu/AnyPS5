#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYFORKHANDLES_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYFORKHANDLES_HPP

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>

class MemoryForkHandles {
public:
    void Add(HANDLE handle) { entries.push_back({handle}); }

    bool Prepare() {
        std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
            return reinterpret_cast<std::uintptr_t>(left.handle) < reinterpret_cast<std::uintptr_t>(right.handle);
        });
        entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
            return left.handle == right.handle;
        }), entries.end());
        for (auto& entry : entries) {
            if (!GetHandleInformation(entry.handle, &entry.flags)) return false;
            if ((entry.flags & HANDLE_FLAG_INHERIT) != 0) continue;
            if (!SetHandleInformation(entry.handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) return false;
            entry.changed = true;
        }
        return true;
    }

    bool Restore() noexcept {
        bool success = true;
        for (const auto& entry : entries) {
            if (entry.changed && !SetHandleInformation(entry.handle, HANDLE_FLAG_INHERIT, entry.flags & HANDLE_FLAG_INHERIT)) success = false;
        }
        entries.clear();
        return success;
    }

private:
    struct Entry {
        HANDLE handle;
        DWORD flags = 0;
        bool changed = false;
    };
    std::vector<Entry> entries;
};
#endif

#endif
