#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_SHAREDMEMORY_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_SHAREDMEMORY_HPP

#include <cstddef>
#include <cstdint>
struct FileStat;
#ifndef _WIN32
#include <sys/uio.h>
#endif

namespace File {

bool IsSharedMemoryDescriptor_nid_no_patch(int descriptor);
bool QuerySharedMemoryAccess_nid_no_patch(int descriptor, bool* readable, bool* writable);
int SharedMemoryStatus_nid_no_patch(int descriptor, FileStat* result);
int ChmodSharedMemory_nid_no_patch(int descriptor, int mode);
std::int64_t ReadSharedMemory_nid_no_patch(int descriptor, void* buffer, std::size_t size);
std::int64_t PreadSharedMemory_nid_no_patch(int descriptor, void* buffer, std::size_t size, std::int64_t offset);
std::int64_t WriteSharedMemory_nid_no_patch(int descriptor, const void* buffer, std::size_t size);
std::int64_t PwriteSharedMemory_nid_no_patch(int descriptor, const void* buffer, std::size_t size, std::int64_t offset);
std::int64_t SeekSharedMemory_nid_no_patch(int descriptor, std::int64_t offset, int whence);
int TruncateSharedMemory_nid_no_patch(int descriptor, std::int64_t length);
#ifndef _WIN32
std::int64_t WritevSharedMemory_nid_no_patch(int descriptor, const struct iovec* vectors, int count);
std::int64_t PwritevSharedMemory_nid_no_patch(int descriptor, const struct iovec* vectors, int count, std::int64_t offset);
#endif

}

#endif
