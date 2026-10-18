/****************************************************************************
 * Experimental native macOS process snapshot format and writer.            *
 ****************************************************************************/

#ifndef DMTCP_MACH_SNAPSHOT_H
#define DMTCP_MACH_SNAPSHOT_H

#include <cstddef>
#include <cstdint>

namespace dmtcp {
namespace MachSnapshot {

inline constexpr char kSnapshotPathEnv[] = "DMTCP_MACOS_SNAPSHOT_FILE";
inline constexpr char kImageMagic[16] = "DMTCP-MACH-SNAP";
inline constexpr uint32_t kImageVersion = 1;
inline constexpr uint32_t kEndianMarker = 0x12345678;
inline constexpr uint32_t kThreadStateWords = 68;
inline constexpr uint32_t kNeonStateWords = 132;

enum ImageFlags : uint32_t {
  kImageThreadsQuiesced = 1U << 0,
  kImageHasUnreadableData = 1U << 1,
  kImageMissingBackingIdentity = 1U << 2,
};

enum RegionFlags : uint32_t {
  kRegionDataCaptured = 1U << 0,
  kRegionDataUnreadable = 1U << 1,
  kRegionSnapshotScratch = 1U << 2,
  kRegionHasBackingPath = 1U << 3,
  kRegionBackingUnavailable = 1U << 4,
};

struct ImageHeader {
  char magic[16];
  uint32_t version;
  uint32_t headerSize;
  uint32_t endianMarker;
  uint32_t pointerSize;
  uint32_t pageSize;
  uint32_t flags;
  uint64_t regionCount;
  uint64_t threadCount;
  uint64_t capturedBytes;
  uint64_t threadTableOffset;
};

struct RegionRecord {
  uint64_t address;
  uint64_t size;
  uint64_t objectOffset;
  uint64_t dataSize;
  uint64_t dataPageCount;
  uint64_t backingDevice;
  uint64_t backingInode;
  uint64_t backingFileSize;
  int64_t backingMtimeSec;
  int64_t backingMtimeNsec;
  int32_t protection;
  int32_t maxProtection;
  int32_t inheritance;
  uint32_t userTag;
  uint32_t shareMode;
  uint32_t externalPager;
  uint32_t machFlags;
  uint32_t flags;
  uint32_t backingPathLength;
};

struct PageRecord {
  uint64_t regionOffset;
  uint32_t size;
  uint32_t disposition;
};

struct ThreadRecord {
  uint64_t threadId;
  uint32_t generalFlavor;
  uint32_t generalStateWordCount;
  uint32_t neonFlavor;
  uint32_t neonStateWordCount;
  uint32_t flags;
  uint32_t reserved;
  uint32_t generalState[kThreadStateWords];
  uint32_t neonState[kNeonStateWords];
};

enum ThreadFlags : uint32_t {
  kSnapshotThread = 1U << 0,
  kCoordinatorThread = 1U << 1,
};

static_assert(sizeof(ImageHeader) == 72);
static_assert(sizeof(RegionRecord) == 120);
static_assert(sizeof(PageRecord) == 16);
static_assert(sizeof(ThreadRecord) == 832);

// Records a helper thread that a restorer must recreate rather than resume.
void markCurrentThread(uint32_t role);

// Writes a native-endian, arm64 snapshot of the current process.  Other
// threads are suspended while memory and register state are copied.  The
// caller remains running and is marked in the thread table for a future
// restorer to exclude from application state.
bool write(const char *path, char *error, size_t errorCapacity);

} // namespace MachSnapshot
} // namespace dmtcp

#endif // DMTCP_MACH_SNAPSHOT_H
