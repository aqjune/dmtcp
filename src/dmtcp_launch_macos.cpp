/****************************************************************************
 * Registration-only dmtcp_launch implementation for macOS.                 *
 ****************************************************************************/

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "config.h"
#include "constants.h"
#include "coordinatorapi_registration.h"
#include "mach_snapshot.h"

namespace {

const char *usage =
  "Usage: dmtcp_launch [OPTIONS] -- COMMAND [ARGS...]\n"
  "Launch a user-owned macOS process and register it with an existing "
  "DMTCP coordinator.\n\n"
  "This experimental mode does not checkpoint or restart the process.\n\n"
  "Options:\n"
  "  --join-coordinator       Require an existing coordinator (default)\n"
  "  -h, --coord-host HOST    Coordinator host (default: 127.0.0.1)\n"
  "  -p, --coord-port PORT    Coordinator port (default: 7779)\n"
  "  --snapshot-file FILE     Write experimental Mach snapshot on SIGUSR2\n"
  "  -q, --quiet              Suppress launcher notices\n"
  "  --help                   Show this help\n"
  "  --version                Show the DMTCP version\n";

[[noreturn]] void
fail(const std::string &message)
{
  fprintf(stderr, "dmtcp_launch: %s\n", message.c_str());
  exit(2);
}

std::string
executablePath()
{
  uint32_t capacity = 0;
  _NSGetExecutablePath(nullptr, &capacity);
  std::vector<char> path(capacity + 1);
  if (_NSGetExecutablePath(path.data(), &capacity) != 0) {
    fail("cannot determine launcher path");
  }
  path.back() = '\0';

  char resolved[PATH_MAX];
  if (realpath(path.data(), resolved) == nullptr) {
    fail("cannot resolve launcher path: " + std::string(strerror(errno)));
  }
  return resolved;
}

std::filesystem::path
resolveCommandPath(const char *command)
{
  auto resolve = [](const std::filesystem::path &candidate) {
    char resolved[PATH_MAX];
    if (access(candidate.c_str(), X_OK) == 0 &&
        realpath(candidate.c_str(), resolved) != nullptr) {
      return std::filesystem::path(resolved);
    }
    return std::filesystem::path();
  };

  if (strchr(command, '/') != nullptr) {
    return resolve(command);
  }

  const char *pathEnvironment = getenv("PATH");
  const std::string_view searchPath = pathEnvironment == nullptr
                                      ? "/usr/bin:/bin"
                                      : pathEnvironment;
  size_t begin = 0;
  while (begin <= searchPath.size()) {
    const size_t end = searchPath.find(':', begin);
    const std::string_view entry = searchPath.substr(
      begin, end == std::string_view::npos ? end : end - begin);
    const std::filesystem::path directory = entry.empty() ? "." : entry;
    const std::filesystem::path resolved = resolve(directory / command);
    if (!resolved.empty()) {
      return resolved;
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return {};
}

bool
isPlatformProtected(const std::filesystem::path &target)
{
  const std::string path = target.string();
  static constexpr const char *protectedPrefixes[] = {
    "/System/", "/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/"
  };
  for (const char *prefix : protectedPrefixes) {
    if (path.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

bool
hasSafeOwnershipAndMode(const std::filesystem::path &target,
                        std::string *reason)
{
  struct stat metadata = {};
  if (stat(target.c_str(), &metadata) == -1) {
    *reason = "cannot inspect target: " + std::string(strerror(errno));
    return false;
  }
  if (!S_ISREG(metadata.st_mode)) {
    *reason = "target is not a regular file";
    return false;
  }
  if (geteuid() != getuid() || metadata.st_uid != geteuid()) {
    *reason = "target must be owned by the invoking user";
    return false;
  }
  if ((metadata.st_mode & (S_ISUID | S_ISGID)) != 0) {
    *reason = "set-user-ID and set-group-ID targets are unsupported";
    return false;
  }
  return true;
}

bool
hasInjectableCodeSignature(const std::filesystem::path &target,
                           std::string *reason)
{
  CFURLRef targetUrl = CFURLCreateFromFileSystemRepresentation(
    kCFAllocatorDefault,
    reinterpret_cast<const UInt8 *>(target.c_str()),
    target.string().size(), false);
  if (targetUrl == nullptr) {
    *reason = "cannot create code-signing URL";
    return false;
  }

  SecStaticCodeRef staticCode = nullptr;
  OSStatus result = SecStaticCodeCreateWithPath(targetUrl, kSecCSDefaultFlags,
                                                &staticCode);
  CFRelease(targetUrl);
  if (result != errSecSuccess) {
    *reason = "cannot inspect target code signature";
    return false;
  }

  CFDictionaryRef information = nullptr;
  result = SecCodeCopySigningInformation(staticCode,
                                         kSecCSSigningInformation,
                                         &information);
  CFRelease(staticCode);
  if (result == errSecCSUnsigned) {
    return true;
  }
  if (result != errSecSuccess || information == nullptr) {
    *reason = "cannot read target code-signing information";
    return false;
  }

  uint32_t flags = 0;
  CFNumberRef flagNumber = static_cast<CFNumberRef>(
    CFDictionaryGetValue(information, kSecCodeInfoFlags));
  const bool hasFlags = flagNumber != nullptr &&
    CFNumberGetValue(flagNumber, kCFNumberSInt32Type, &flags);
  CFRelease(information);
  if (!hasFlags) {
    *reason = "target code signature has no readable flags";
    return false;
  }

  constexpr uint32_t incompatibleFlags =
    kSecCodeSignatureForceHard |
    kSecCodeSignatureForceKill |
    kSecCodeSignatureRestrict |
    kSecCodeSignatureEnforcement |
    kSecCodeSignatureLibraryValidation |
    kSecCodeSignatureRuntime;
  if ((flags & incompatibleFlags) != 0) {
    *reason = "target uses hardened or library-validation code signing";
    return false;
  }
  return true;
}

bool
isMachOExecutable(const std::filesystem::path &target, bool *isScript)
{
  *isScript = false;
  int fd = open(target.c_str(), O_RDONLY);
  if (fd == -1) {
    return false;
  }

  uint32_t magic = 0;
  const ssize_t bytes = read(fd, &magic, sizeof(magic));
  close(fd);
  if (bytes >= 2 && (magic & 0xffffU) ==
                    (static_cast<uint32_t>('#') |
                     (static_cast<uint32_t>('!') << 8))) {
    *isScript = true;
    return false;
  }
  if (bytes != sizeof(magic)) {
    return false;
  }
  return magic == MH_MAGIC || magic == MH_CIGAM || magic == MH_MAGIC_64 ||
         magic == MH_CIGAM_64 || magic == FAT_MAGIC || magic == FAT_CIGAM ||
         magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
}

bool
readRange(const unsigned char *data,
          size_t size,
          uint64_t offset,
          void *destination,
          size_t length)
{
  if (offset > size || length > size - static_cast<size_t>(offset)) {
    return false;
  }
  memcpy(destination, data + offset, length);
  return true;
}

uint32_t
convert32(uint32_t value, bool swapped)
{
  return swapped ? __builtin_bswap32(value) : value;
}

uint64_t
convert64(uint64_t value, bool swapped)
{
  return swapped ? __builtin_bswap64(value) : value;
}

bool
thinMachOIsInjectable(const unsigned char *data,
                      size_t fileSize,
                      uint64_t sliceOffset,
                      uint64_t sliceSize,
                      std::string *reason)
{
  if (sliceOffset > fileSize || sliceSize > fileSize - sliceOffset) {
    *reason = "invalid Mach-O slice bounds";
    return false;
  }

  uint32_t magic = 0;
  if (sliceSize < sizeof(magic) ||
      !readRange(data, fileSize, sliceOffset, &magic, sizeof(magic))) {
    *reason = "truncated Mach-O image";
    return false;
  }

  const bool is64Bit = magic == MH_MAGIC_64 || magic == MH_CIGAM_64;
  const bool swapped = magic == MH_CIGAM || magic == MH_CIGAM_64;
  if (magic != MH_MAGIC && magic != MH_CIGAM &&
      magic != MH_MAGIC_64 && magic != MH_CIGAM_64) {
    *reason = "invalid Mach-O slice";
    return false;
  }

  uint32_t commandCount = 0;
  uint32_t commandBytes = 0;
  size_t headerSize = 0;
  if (is64Bit) {
    struct mach_header_64 header = {};
    if (!readRange(data, fileSize, sliceOffset, &header, sizeof(header))) {
      *reason = "truncated Mach-O header";
      return false;
    }
    commandCount = convert32(header.ncmds, swapped);
    commandBytes = convert32(header.sizeofcmds, swapped);
    headerSize = sizeof(header);
  } else {
    struct mach_header header = {};
    if (!readRange(data, fileSize, sliceOffset, &header, sizeof(header))) {
      *reason = "truncated Mach-O header";
      return false;
    }
    commandCount = convert32(header.ncmds, swapped);
    commandBytes = convert32(header.sizeofcmds, swapped);
    headerSize = sizeof(header);
  }

  if (headerSize > sliceSize || commandBytes > sliceSize - headerSize) {
    *reason = "invalid Mach-O load-command bounds";
    return false;
  }

  uint64_t commandOffset = sliceOffset + headerSize;
  const uint64_t commandEnd = commandOffset + commandBytes;
  for (uint32_t index = 0; index < commandCount; ++index) {
    struct load_command command = {};
    if (commandOffset > commandEnd ||
        !readRange(data, fileSize, commandOffset, &command,
                   sizeof(command))) {
      *reason = "truncated Mach-O load command";
      return false;
    }
    const uint32_t type = convert32(command.cmd, swapped);
    const uint32_t length = convert32(command.cmdsize, swapped);
    if (length < sizeof(command) || length > commandEnd - commandOffset) {
      *reason = "invalid Mach-O load-command size";
      return false;
    }

    if (type == LC_SEGMENT_64 && length >= sizeof(struct segment_command_64)) {
      struct segment_command_64 segment = {};
      if (!readRange(data, fileSize, commandOffset, &segment,
                     sizeof(segment))) {
        *reason = "truncated Mach-O segment";
        return false;
      }
      if (strncmp(segment.segname, "__RESTRICT",
                  sizeof(segment.segname)) == 0) {
        *reason = "target contains a dyld-restricted Mach-O segment";
        return false;
      }
    } else if (type == LC_SEGMENT &&
               length >= sizeof(struct segment_command)) {
      struct segment_command segment = {};
      if (!readRange(data, fileSize, commandOffset, &segment,
                     sizeof(segment))) {
        *reason = "truncated Mach-O segment";
        return false;
      }
      if (strncmp(segment.segname, "__RESTRICT",
                  sizeof(segment.segname)) == 0) {
        *reason = "target contains a dyld-restricted Mach-O segment";
        return false;
      }
    }
    commandOffset += length;
  }

  if (commandOffset != commandEnd) {
    *reason = "Mach-O load-command count does not match its size";
    return false;
  }
  return true;
}

bool
hasInjectableMachOLayout(const std::filesystem::path &target,
                         std::string *reason)
{
  int fd = open(target.c_str(), O_RDONLY);
  if (fd == -1) {
    *reason = "cannot open target for Mach-O inspection";
    return false;
  }

  struct stat metadata = {};
  if (fstat(fd, &metadata) == -1 || metadata.st_size < 0) {
    *reason = "cannot inspect target size";
    close(fd);
    return false;
  }
  const size_t fileSize = static_cast<size_t>(metadata.st_size);
  void *mapping = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) {
    *reason = "cannot map target for Mach-O inspection";
    return false;
  }

  const unsigned char *data = static_cast<const unsigned char *>(mapping);
  uint32_t magic = 0;
  bool injectable = readRange(data, fileSize, 0, &magic, sizeof(magic));
  if (!injectable) {
    *reason = "truncated Mach-O file";
  } else if (magic == FAT_MAGIC || magic == FAT_CIGAM ||
             magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
    const bool swapped = magic == FAT_CIGAM || magic == FAT_CIGAM_64;
    const bool uses64BitOffsets = magic == FAT_MAGIC_64 ||
                                  magic == FAT_CIGAM_64;
    struct fat_header header = {};
    injectable = readRange(data, fileSize, 0, &header, sizeof(header));
    uint32_t architectureCount = injectable
                                 ? convert32(header.nfat_arch, swapped)
                                 : 0;
    if (injectable && architectureCount == 0) {
      *reason = "universal Mach-O contains no architectures";
      injectable = false;
    }
    uint64_t architectureOffset = sizeof(header);
    for (uint32_t index = 0; injectable && index < architectureCount; ++index) {
      uint64_t sliceOffset = 0;
      uint64_t sliceSize = 0;
      if (uses64BitOffsets) {
        struct fat_arch_64 architecture = {};
        injectable = readRange(data, fileSize, architectureOffset,
                               &architecture, sizeof(architecture));
        if (injectable) {
          sliceOffset = convert64(architecture.offset, swapped);
          sliceSize = convert64(architecture.size, swapped);
        }
        architectureOffset += sizeof(architecture);
      } else {
        struct fat_arch architecture = {};
        injectable = readRange(data, fileSize, architectureOffset,
                               &architecture, sizeof(architecture));
        if (injectable) {
          sliceOffset = convert32(architecture.offset, swapped);
          sliceSize = convert32(architecture.size, swapped);
        }
        architectureOffset += sizeof(architecture);
      }
      if (injectable) {
        injectable = thinMachOIsInjectable(data, fileSize, sliceOffset,
                                           sliceSize, reason);
      }
    }
    if (!injectable && reason->empty()) {
      *reason = "invalid universal Mach-O header";
    }
  } else {
    injectable = thinMachOIsInjectable(data, fileSize, 0, fileSize, reason);
  }

  munmap(mapping, fileSize);
  return injectable;
}

int
parsePort(const char *text)
{
  char *end = nullptr;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 1 ||
      value > 65535) {
    fail("invalid coordinator port '" + std::string(text) + "'");
  }
  return static_cast<int>(value);
}

const char *
optionValue(int argc, char **argv, int *index, const char *option)
{
  if (*index + 1 >= argc) {
    fail(std::string(option) + " requires a value");
  }
  ++*index;
  return argv[*index];
}

void
setEnvironment(const char *name, const std::string &value)
{
  if (setenv(name, value.c_str(), 1) == -1) {
    fail("cannot set " + std::string(name) + ": " + strerror(errno));
  }
}

} // namespace

int
main(int argc, char **argv)
{
  std::string host = DEFAULT_HOST;
  std::string snapshotPath;
  int port = DEFAULT_PORT;
  bool quiet = false;
  bool snapshotRequested = false;
  int commandIndex = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--") {
      commandIndex = i + 1;
      break;
    }
    if (argument.empty() || argument[0] != '-') {
      commandIndex = i;
      break;
    }

    if (argument == "--help") {
      fputs(usage, stdout);
      return 0;
    }
    if (argument == "--version") {
      printf("dmtcp_launch (DMTCP) %s\n", PACKAGE_VERSION);
      return 0;
    }
    if (argument == "--join-coordinator") {
      continue;
    }
    if (argument == "-q" || argument == "--quiet") {
      quiet = true;
      continue;
    }
    if (argument == "-h" || argument == "--coord-host") {
      host = optionValue(argc, argv, &i, argument.c_str());
      continue;
    }
    if (argument.rfind("--coord-host=", 0) == 0) {
      host = argument.substr(strlen("--coord-host="));
      continue;
    }
    if (argument == "-p" || argument == "--coord-port") {
      port = parsePort(optionValue(argc, argv, &i, argument.c_str()));
      continue;
    }
    if (argument.rfind("--coord-port=", 0) == 0) {
      port = parsePort(argument.c_str() + strlen("--coord-port="));
      continue;
    }
    if (argument == "--snapshot-file") {
      snapshotRequested = true;
      snapshotPath = optionValue(argc, argv, &i, argument.c_str());
      continue;
    }
    if (argument.rfind("--snapshot-file=", 0) == 0) {
      snapshotRequested = true;
      snapshotPath = argument.substr(strlen("--snapshot-file="));
      continue;
    }
    if (argument == "--new-coordinator" ||
        argument == "--any-coordinator") {
      fail(argument + " is unavailable in macOS registration-only mode");
    }
    fail("unsupported macOS option '" + argument + "'");
  }

  if (commandIndex == -1 || commandIndex >= argc) {
    fputs(usage, stderr);
    return 2;
  }
  if (host.empty()) {
    fail("coordinator host must not be empty");
  }
  if (snapshotRequested && snapshotPath.empty()) {
    fail("snapshot path must not be empty");
  }

  const std::filesystem::path target = resolveCommandPath(argv[commandIndex]);
  if (target.empty()) {
    fail("cannot find executable '" + std::string(argv[commandIndex]) + "'");
  }
  if (isPlatformProtected(target)) {
    fail("cannot inject into platform-protected executable '" +
         target.string() + "'");
  }
  std::string eligibilityError;
  if (!hasSafeOwnershipAndMode(target, &eligibilityError)) {
    fail(eligibilityError + ": '" + target.string() + "'");
  }
  if (!hasInjectableCodeSignature(target, &eligibilityError)) {
    fail(eligibilityError + ": '" + target.string() + "'");
  }
  bool isScript = false;
  if (!isMachOExecutable(target, &isScript)) {
    fail(isScript
           ? "scripts are unavailable in macOS registration-only mode"
           : "target is not a Mach-O executable: '" + target.string() + "'");
  }
  if (!hasInjectableMachOLayout(target, &eligibilityError)) {
    fail(eligibilityError + ": '" + target.string() + "'");
  }

  const std::filesystem::path launcher = executablePath();
  const std::filesystem::path dylib =
    launcher.parent_path().parent_path() / "lib" / "dmtcp" /
    "libdmtcp-register.dylib";
  if (access(dylib.c_str(), R_OK) == -1) {
    fail("registration library is not readable at '" + dylib.string() +
         "'");
  }

  std::string insertedLibraries = dylib.string();
  const char *existingLibraries = getenv("DYLD_INSERT_LIBRARIES");
  if (existingLibraries != nullptr && existingLibraries[0] != '\0') {
    insertedLibraries += ":";
    insertedLibraries += existingLibraries;
  }

  setEnvironment(ENV_VAR_NAME_HOST, host);
  setEnvironment(ENV_VAR_NAME_PORT, std::to_string(port));
  setEnvironment(dmtcp::Registration::kEnabledEnv, "1");
  if (snapshotRequested) {
    sigset_t snapshotSignalSet;
    sigemptyset(&snapshotSignalSet);
    sigaddset(&snapshotSignalSet, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &snapshotSignalSet, nullptr) == -1) {
      fail("cannot reserve SIGUSR2 for snapshot capture: " +
           std::string(strerror(errno)));
    }

    std::error_code pathError;
    const std::filesystem::path absoluteSnapshot =
      std::filesystem::absolute(snapshotPath, pathError);
    if (pathError) {
      fail("cannot resolve snapshot path '" + snapshotPath + "'");
    }
    snapshotPath = absoluteSnapshot.lexically_normal().string();
    setEnvironment(dmtcp::MachSnapshot::kSnapshotPathEnv, snapshotPath);
  } else if (unsetenv(dmtcp::MachSnapshot::kSnapshotPathEnv) == -1) {
    fail("cannot clear inherited snapshot configuration: " +
         std::string(strerror(errno)));
  }
  setEnvironment("DYLD_INSERT_LIBRARIES", insertedLibraries);

  if (!quiet) {
    fprintf(stderr,
            "dmtcp_launch: registering '%s' with %s:%d; "
            "checkpoint/restart is unavailable on macOS\n",
            argv[commandIndex], host.c_str(), port);
    if (!snapshotPath.empty()) {
      fprintf(stderr,
              "dmtcp_launch: send SIGUSR2 to write experimental snapshot %s\n",
              snapshotPath.c_str());
    }
  }

  execv(target.c_str(), &argv[commandIndex]);
  fail("cannot execute '" + target.string() + "': " + strerror(errno));
}
