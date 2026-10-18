/****************************************************************************
 * Experimental native macOS process snapshot writer.                       *
 ****************************************************************************/

#include "mach_snapshot.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#if defined(__arm64__)
# include <mach/arm/thread_status.h>
#endif
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_info.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include <libproc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dmtcp {
namespace MachSnapshot {
namespace {

constexpr size_t kCopyBufferSize = 1024U * 1024U;
constexpr size_t kMaxTrackedThreads = 4096;
constexpr unsigned int kMaxSuspendPasses = 8;

std::atomic<uint64_t> coordinatorThreadId{0};
std::atomic<uint64_t> snapshotThreadId{0};

#if defined(__arm64__)
static_assert(ARM_THREAD_STATE64_COUNT == kThreadStateWords);
static_assert(sizeof(arm_thread_state64_t) ==
              kThreadStateWords * sizeof(uint32_t));
static_assert(ARM_NEON_STATE64_COUNT == kNeonStateWords);
static_assert(sizeof(arm_neon_state64_t) ==
              kNeonStateWords * sizeof(uint32_t));
#endif

bool
writeAll(int fd, const void *buffer, size_t length)
{
  const char *cursor = static_cast<const char *>(buffer);
  while (length > 0) {
    const ssize_t written = ::write(fd, cursor, length);
    if (written == -1 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    cursor += written;
    length -= static_cast<size_t>(written);
  }
  return true;
}

bool
pwriteAll(int fd, const void *buffer, size_t length, off_t offset)
{
  const char *cursor = static_cast<const char *>(buffer);
  while (length > 0) {
    const ssize_t written = pwrite(fd, cursor, length, offset);
    if (written == -1 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    cursor += written;
    length -= static_cast<size_t>(written);
    offset += written;
  }
  return true;
}

bool
rangesOverlap(uint64_t firstAddress,
              uint64_t firstSize,
              uint64_t secondAddress,
              uint64_t secondSize)
{
  if (firstSize == 0 || secondSize == 0) {
    return false;
  }
  if (firstAddress <= secondAddress) {
    return secondAddress - firstAddress < firstSize;
  }
  return firstAddress - secondAddress < secondSize;
}

void
setError(char *error, size_t capacity, const char *message)
{
  if (error != nullptr && capacity > 0) {
    snprintf(error, capacity, "%s", message);
  }
}

} // namespace

void
markCurrentThread(uint32_t role)
{
  uint64_t threadId = 0;
  if (pthread_threadid_np(nullptr, &threadId) != 0) {
    return;
  }
  if ((role & kCoordinatorThread) != 0) {
    coordinatorThreadId.store(threadId);
  }
  if ((role & kSnapshotThread) != 0) {
    snapshotThreadId.store(threadId);
  }
}

bool
write(const char *path, char *error, size_t errorCapacity)
{
#if !defined(__arm64__)
  setError(error, errorCapacity,
           "Mach snapshots currently support native arm64 only");
  return false;
#else
  if (path == nullptr || path[0] == '\0') {
    setError(error, errorCapacity, "snapshot path is empty");
    return false;
  }

  char temporaryPath[PATH_MAX];
  const int pathLength = snprintf(temporaryPath, sizeof(temporaryPath),
                                  "%s.tmp.XXXXXX", path);
  if (pathLength < 0 || static_cast<size_t>(pathLength) >=
                          sizeof(temporaryPath)) {
    setError(error, errorCapacity, "snapshot path is too long");
    return false;
  }

  int imageFd = mkstemp(temporaryPath);
  if (imageFd == -1 || fchmod(imageFd, 0600) == -1 ||
      fcntl(imageFd, F_SETFD, FD_CLOEXEC) == -1) {
    if (imageFd != -1) {
      close(imageFd);
      unlink(temporaryPath);
    }
    setError(error, errorCapacity, "cannot create private snapshot image");
    return false;
  }

  void *copyBuffer = mmap(nullptr, kCopyBufferSize,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
  if (copyBuffer == MAP_FAILED) {
    close(imageFd);
    unlink(temporaryPath);
    setError(error, errorCapacity, "cannot allocate snapshot copy buffer");
    return false;
  }

  const size_t threadWorkspaceSize = kMaxTrackedThreads * sizeof(thread_t);
  auto *trackedThreads = static_cast<thread_t *>(
    mmap(nullptr, threadWorkspaceSize, PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANON, -1, 0));
  if (trackedThreads == MAP_FAILED) {
    munmap(copyBuffer, kCopyBufferSize);
    close(imageFd);
    unlink(temporaryPath);
    setError(error, errorCapacity, "cannot allocate thread workspace");
    return false;
  }

  const mach_port_t currentThread = pthread_mach_thread_np(pthread_self());
  const uint64_t currentStackEnd = reinterpret_cast<uint64_t>(
    pthread_get_stackaddr_np(pthread_self()));
  const uint64_t currentStackSize = pthread_get_stacksize_np(pthread_self());
  const uint64_t currentStackStart = currentStackEnd - currentStackSize;
  const uint64_t copyBufferAddress = reinterpret_cast<uint64_t>(copyBuffer);
  const uint64_t threadWorkspaceAddress =
    reinterpret_cast<uint64_t>(trackedThreads);

  ImageHeader header = {};
  memcpy(header.magic, kImageMagic, sizeof(header.magic));
  header.version = kImageVersion;
  header.headerSize = sizeof(header);
  header.endianMarker = kEndianMarker;
  header.pointerSize = sizeof(void *);
  header.pageSize = static_cast<uint32_t>(getpagesize());

  const char *failure = nullptr;
  if (!writeAll(imageFd, &header, sizeof(header))) {
    failure = "cannot write snapshot header";
  }

  // Resolve and fault the imported capture path before arbitrary target
  // threads can be stopped while holding dyld or libc internal locks.
  if (failure == nullptr) {
    mach_vm_address_t warmAddress = copyBufferAddress;
    mach_vm_size_t warmSize = 0;
    natural_t warmDepth = 0;
    vm_region_submap_info_data_64_t warmInfo = {};
    mach_msg_type_number_t warmInfoCount = VM_REGION_SUBMAP_INFO_COUNT_64;
    mach_vm_region_recurse(
      mach_task_self(), &warmAddress, &warmSize, &warmDepth,
      reinterpret_cast<vm_region_recurse_info_t>(&warmInfo), &warmInfoCount);
    vm_page_disposition_t warmDisposition = 0;
    mach_vm_size_t warmDispositionCount = 1;
    mach_vm_page_range_query(
      mach_task_self(), copyBufferAddress, header.pageSize,
      reinterpret_cast<mach_vm_address_t>(&warmDisposition),
      &warmDispositionCount);
    mach_vm_size_t warmReadSize = 0;
    mach_vm_read_overwrite(mach_task_self(), copyBufferAddress,
                           header.pageSize, copyBufferAddress, &warmReadSize);
    thread_identifier_info_data_t warmIdentifier = {};
    mach_msg_type_number_t warmIdentifierCount = THREAD_IDENTIFIER_INFO_COUNT;
    thread_info(currentThread, THREAD_IDENTIFIER_INFO,
                reinterpret_cast<thread_info_t>(&warmIdentifier),
                &warmIdentifierCount);
    uint32_t warmGeneral[kThreadStateWords] = {};
    mach_msg_type_number_t warmGeneralCount = ARM_THREAD_STATE64_COUNT;
    thread_get_state(currentThread, ARM_THREAD_STATE64,
                     reinterpret_cast<thread_state_t>(warmGeneral),
                     &warmGeneralCount);
    uint32_t warmNeon[kNeonStateWords] = {};
    mach_msg_type_number_t warmNeonCount = ARM_NEON_STATE64_COUNT;
    thread_get_state(currentThread, ARM_NEON_STATE64,
                     reinterpret_cast<thread_state_t>(warmNeon),
                     &warmNeonCount);
    char warmPath[PROC_PIDPATHINFO_MAXSIZE] = {};
    proc_regionfilename(getpid(), copyBufferAddress,
                        warmPath, sizeof(warmPath));
    struct stat warmMetadata = {};
    fstat(imageFd, &warmMetadata);
    stat(temporaryPath, &warmMetadata);
    lseek(imageFd, 0, SEEK_CUR);
    pwriteAll(imageFd, &header, sizeof(header), 0);
    fsync(imageFd);
    thread_suspend(MACH_PORT_NULL);
    thread_resume(MACH_PORT_NULL);
    vm_deallocate(mach_task_self(), 0, 0);
  }

  size_t trackedThreadCount = 0;
  bool suspensionStable = false;
  for (unsigned int pass = 0;
       failure == nullptr && pass < kMaxSuspendPasses;
       ++pass) {
    thread_act_array_t passThreads = nullptr;
    mach_msg_type_number_t passThreadCount = 0;
    if (task_threads(mach_task_self(), &passThreads,
                     &passThreadCount) != KERN_SUCCESS) {
      failure = "cannot enumerate process threads";
      break;
    }

    bool addedThread = false;
    for (mach_msg_type_number_t index = 0;
         index < passThreadCount;
         ++index) {
      const thread_t thread = passThreads[index];
      if (thread == currentThread) {
        mach_port_deallocate(mach_task_self(), thread);
        continue;
      }

      bool alreadyTracked = false;
      for (size_t tracked = 0; tracked < trackedThreadCount; ++tracked) {
        if (trackedThreads[tracked] == thread) {
          alreadyTracked = true;
          break;
        }
      }
      if (alreadyTracked) {
        mach_port_deallocate(mach_task_self(), thread);
        continue;
      }
      if (trackedThreadCount == kMaxTrackedThreads) {
        mach_port_deallocate(mach_task_self(), thread);
        failure = "process has too many threads to snapshot";
        continue;
      }
      if (thread_suspend(thread) != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), thread);
        failure = "cannot suspend every process thread";
        continue;
      }
      trackedThreads[trackedThreadCount++] = thread;
      addedThread = true;
    }
    vm_deallocate(mach_task_self(),
                  reinterpret_cast<vm_address_t>(passThreads),
                  passThreadCount * sizeof(thread_t));
    if (failure == nullptr && !addedThread) {
      suspensionStable = true;
      break;
    }
  }
  if (failure == nullptr && !suspensionStable) {
    failure = "process thread set did not become stable";
  }
  if (suspensionStable) {
    header.flags |= kImageThreadsQuiesced;
  }

  kern_return_t machResult = KERN_SUCCESS;
  mach_vm_address_t address = 0;
  natural_t depth = 0;
  while (failure == nullptr) {
    mach_vm_size_t regionSize = 0;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t infoCount = VM_REGION_SUBMAP_INFO_COUNT_64;
    machResult = mach_vm_region_recurse(
      mach_task_self(), &address, &regionSize, &depth,
      reinterpret_cast<vm_region_recurse_info_t>(&info), &infoCount);
    if (machResult == KERN_INVALID_ADDRESS) {
      break;
    }
    if (machResult != KERN_SUCCESS || regionSize == 0) {
      failure = "cannot enumerate process memory regions";
      break;
    }
    if (info.is_submap) {
      ++depth;
      continue;
    }

    RegionRecord record = {};
    record.address = address;
    record.size = regionSize;
    record.objectOffset = info.offset;
    record.protection = info.protection;
    record.maxProtection = info.max_protection;
    record.inheritance = info.inheritance;
    record.userTag = info.user_tag;
    record.shareMode = info.share_mode;
    record.externalPager = info.external_pager;
    record.machFlags = info.flags;

    char backingPath[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (info.external_pager != 0) {
      struct proc_regionwithpathinfo pathInfo = {};
      const int pathResult = proc_pidinfo(
        getpid(), PROC_PIDREGIONPATHINFO, address,
        &pathInfo, sizeof(pathInfo));
      bool verifiedBacking = false;
      if (pathResult == sizeof(pathInfo) && pathInfo.prp_vip.vip_path[0] != '\0') {
        snprintf(backingPath, sizeof(backingPath), "%s",
                 pathInfo.prp_vip.vip_path);
        record.backingPathLength =
          static_cast<uint32_t>(strnlen(backingPath,
                                        sizeof(backingPath) - 1) + 1);
        const struct vinfo_stat &vnode = pathInfo.prp_vip.vip_vi.vi_stat;
        record.backingDevice = vnode.vst_dev;
        record.backingInode = vnode.vst_ino;
        record.backingFileSize = vnode.vst_size;
        record.backingMtimeSec = vnode.vst_mtime;
        record.backingMtimeNsec = vnode.vst_mtimensec;

        struct stat currentBacking = {};
        verifiedBacking = stat(backingPath, &currentBacking) == 0 &&
          static_cast<uint64_t>(currentBacking.st_dev) ==
            record.backingDevice &&
          currentBacking.st_ino == record.backingInode &&
          static_cast<uint64_t>(currentBacking.st_size) ==
            record.backingFileSize &&
          currentBacking.st_mtimespec.tv_sec == record.backingMtimeSec &&
          currentBacking.st_mtimespec.tv_nsec == record.backingMtimeNsec;
      }
      if (verifiedBacking) {
        record.flags |= kRegionHasBackingPath;
      } else {
        record.flags |= kRegionBackingUnavailable;
        header.flags |= kImageMissingBackingIdentity;
      }
    }

    const off_t recordOffset = lseek(imageFd, 0, SEEK_CUR);
    if (recordOffset == -1 || !writeAll(imageFd, &record, sizeof(record)) ||
        (record.backingPathLength > 0 &&
         !writeAll(imageFd, backingPath, record.backingPathLength))) {
      failure = "cannot write memory-region record";
      break;
    }

    const bool isSnapshotScratch =
      rangesOverlap(address, regionSize, copyBufferAddress, kCopyBufferSize) ||
      rangesOverlap(address, regionSize, threadWorkspaceAddress,
                    threadWorkspaceSize) ||
      rangesOverlap(address, regionSize, currentStackStart, currentStackSize);
    const bool captureData = !isSnapshotScratch &&
      (info.protection & VM_PROT_READ) != 0 &&
      (info.protection & VM_PROT_WRITE) != 0 &&
      info.user_tag != VM_MEMORY_GUARD;

    if (isSnapshotScratch) {
      record.flags |= kRegionSnapshotScratch;
    } else if (captureData) {
      const mach_vm_size_t pageSize = header.pageSize;
      auto *dispositions = reinterpret_cast<vm_page_disposition_t *>(
        static_cast<char *>(copyBuffer) + pageSize);
      const mach_vm_size_t dispositionCapacity =
        (kCopyBufferSize - pageSize) / sizeof(*dispositions);
      mach_vm_size_t queried = 0;
      while (queried < regionSize) {
        const mach_vm_size_t remaining = regionSize - queried;
        const mach_vm_size_t requestedPages =
          std::min<mach_vm_size_t>(
            dispositionCapacity,
            remaining / pageSize + (remaining % pageSize != 0));
        const mach_vm_size_t querySize =
          std::min<mach_vm_size_t>(remaining, requestedPages * pageSize);
        mach_vm_size_t dispositionCount = requestedPages;
        machResult = mach_vm_page_range_query(
          mach_task_self(), address + queried, querySize,
          reinterpret_cast<mach_vm_address_t>(dispositions),
          &dispositionCount);
        if (machResult != KERN_SUCCESS ||
            dispositionCount != requestedPages) {
          record.flags |= kRegionDataUnreadable;
          header.flags |= kImageHasUnreadableData;
          break;
        }

        for (mach_vm_size_t page = 0; page < requestedPages; ++page) {
          const vm_page_disposition_t disposition = dispositions[page];
          const bool hasPageState =
            (disposition & (VM_PAGE_QUERY_PAGE_PRESENT |
                            VM_PAGE_QUERY_PAGE_PAGED_OUT)) != 0;
          const bool hasPrivateChanges =
            (disposition & (VM_PAGE_QUERY_PAGE_DIRTY |
                            VM_PAGE_QUERY_PAGE_COPIED)) != 0;
          if (!hasPageState ||
              (info.external_pager &&
               (record.flags & kRegionHasBackingPath) != 0 &&
               !hasPrivateChanges)) {
            continue;
          }

          const mach_vm_size_t pageOffset = queried + page * pageSize;
          const mach_vm_size_t request = std::min<mach_vm_size_t>(
            pageSize, regionSize - pageOffset);
          mach_vm_size_t received = 0;
          machResult = mach_vm_read_overwrite(
            mach_task_self(), address + pageOffset, request,
            reinterpret_cast<mach_vm_address_t>(copyBuffer), &received);
          if (machResult != KERN_SUCCESS || received != request) {
            record.flags |= kRegionDataUnreadable;
            header.flags |= kImageHasUnreadableData;
            continue;
          }

          const PageRecord pageRecord = {
            pageOffset,
            static_cast<uint32_t>(received),
            static_cast<uint32_t>(disposition),
          };
          if (!writeAll(imageFd, &pageRecord, sizeof(pageRecord)) ||
              !writeAll(imageFd, copyBuffer,
                        static_cast<size_t>(received))) {
            failure = "cannot write memory-page contents";
            break;
          }
          ++record.dataPageCount;
          record.dataSize += received;
          header.capturedBytes += received;
        }
        if (failure != nullptr) {
          break;
        }
        queried += querySize;
      }
      if (record.dataPageCount > 0) {
        record.flags |= kRegionDataCaptured;
      }
    }

    if (failure == nullptr &&
        !pwriteAll(imageFd, &record, sizeof(record), recordOffset)) {
      failure = "cannot update memory-region record";
      break;
    }
    ++header.regionCount;

    if (address > UINT64_MAX - regionSize) {
      break;
    }
    address += regionSize;
  }

  if (failure == nullptr) {
    const off_t threadTableOffset = lseek(imageFd, 0, SEEK_CUR);
    if (threadTableOffset == -1) {
      failure = "cannot locate snapshot thread table";
    } else {
      header.threadTableOffset = static_cast<uint64_t>(threadTableOffset);
    }
  }

  const size_t capturedThreadCount = trackedThreadCount + 1;
  for (size_t index = 0;
       failure == nullptr && index < capturedThreadCount;
       ++index) {
    const thread_t thread = index == 0 ? currentThread
                                       : trackedThreads[index - 1];
    ThreadRecord record = {};
    record.generalFlavor = ARM_THREAD_STATE64;
    record.generalStateWordCount = ARM_THREAD_STATE64_COUNT;
    record.neonFlavor = ARM_NEON_STATE64;
    record.neonStateWordCount = ARM_NEON_STATE64_COUNT;

    thread_identifier_info_data_t identifier = {};
    mach_msg_type_number_t identifierCount = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(thread, THREAD_IDENTIFIER_INFO,
                    reinterpret_cast<thread_info_t>(&identifier),
                    &identifierCount) != KERN_SUCCESS) {
      failure = "cannot read thread identifier";
      break;
    }
    record.threadId = identifier.thread_id;
    if (record.threadId == snapshotThreadId.load() || thread == currentThread) {
      record.flags |= kSnapshotThread;
    }
    if (record.threadId == coordinatorThreadId.load()) {
      record.flags |= kCoordinatorThread;
    }

    mach_msg_type_number_t generalCount = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(
          thread, ARM_THREAD_STATE64,
          reinterpret_cast<thread_state_t>(record.generalState),
          &generalCount) != KERN_SUCCESS ||
        generalCount != ARM_THREAD_STATE64_COUNT) {
      failure = "cannot read arm64 general thread state";
      break;
    }
    mach_msg_type_number_t neonCount = ARM_NEON_STATE64_COUNT;
    if (thread_get_state(
          thread, ARM_NEON_STATE64,
          reinterpret_cast<thread_state_t>(record.neonState),
          &neonCount) != KERN_SUCCESS || neonCount != ARM_NEON_STATE64_COUNT) {
      failure = "cannot read arm64 NEON thread state";
      break;
    }
    if (!writeAll(imageFd, &record, sizeof(record))) {
      failure = "cannot write thread-state record";
      break;
    }
    ++header.threadCount;
  }

  if (failure == nullptr &&
      !pwriteAll(imageFd, &header, sizeof(header), 0)) {
    failure = "cannot finalize snapshot header";
  }
  if (failure == nullptr && fsync(imageFd) == -1) {
    failure = "cannot flush snapshot image";
  }

  bool resumeFailed = false;
  for (size_t index = 0; index < trackedThreadCount; ++index) {
    if (thread_resume(trackedThreads[index]) != KERN_SUCCESS) {
      resumeFailed = true;
    }
    mach_port_deallocate(mach_task_self(), trackedThreads[index]);
  }
  if (failure == nullptr && resumeFailed) {
    failure = "cannot resume every process thread";
  }
  munmap(trackedThreads, threadWorkspaceSize);
  munmap(copyBuffer, kCopyBufferSize);
  close(imageFd);

  if (failure == nullptr && rename(temporaryPath, path) == -1) {
    failure = "cannot publish completed snapshot image";
  }
  if (failure != nullptr) {
    unlink(temporaryPath);
    setError(error, errorCapacity, failure);
    return false;
  }
  if (error != nullptr && errorCapacity > 0) {
    error[0] = '\0';
  }
  return true;
#endif
}

} // namespace MachSnapshot
} // namespace dmtcp
