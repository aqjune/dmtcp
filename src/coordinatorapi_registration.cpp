/****************************************************************************
 * Coordinator registration for processes without a checkpoint runtime.    *
 ****************************************************************************/

#include "coordinatorapi_registration.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "dmtcpmessagetypes.h"
#include "workerstate.h"

namespace dmtcp {
namespace Registration {
namespace {

bool
writeAll(int fd, const void *buffer, size_t length)
{
  const char *cursor = static_cast<const char *>(buffer);
  while (length > 0) {
    const ssize_t written = write(fd, cursor, length);
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
readAll(int fd, void *buffer, size_t length)
{
  char *cursor = static_cast<char *>(buffer);
  while (length > 0) {
    const ssize_t received = read(fd, cursor, length);
    if (received == -1 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      return false;
    }
    cursor += received;
    length -= static_cast<size_t>(received);
  }
  return true;
}

int
openSocket(const char *host, int port, std::string *error)
{
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char portText[16];
  snprintf(portText, sizeof(portText), "%d", port);

  struct addrinfo *addresses = nullptr;
  const int lookupResult = getaddrinfo(host, portText, &hints, &addresses);
  if (lookupResult != 0) {
    *error = "cannot resolve coordinator host '" + std::string(host) +
             "': " + gai_strerror(lookupResult);
    return -1;
  }

  int fd = -1;
  int savedErrno = ECONNREFUSED;
  for (const struct addrinfo *address = addresses;
       address != nullptr;
       address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype,
                address->ai_protocol);
    if (fd == -1) {
      savedErrno = errno;
      continue;
    }
    if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    savedErrno = errno;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);

  if (fd == -1) {
    *error = "cannot connect to coordinator at " + std::string(host) + ":" +
             portText + ": " + strerror(savedErrno);
  }
  return fd;
}

} // namespace

int
connectToCoordinator(const char *host,
                     int port,
                     const char *programName,
                     std::string *error)
{
  if (host == nullptr || host[0] == '\0' || port < 1 || port > 65535 ||
      programName == nullptr || error == nullptr) {
    if (error != nullptr) {
      *error = "invalid coordinator registration parameters";
    }
    return -1;
  }

  int fd = openSocket(host, port, error);
  if (fd == -1) {
    return -1;
  }

  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname)) == -1) {
    *error = "cannot read local hostname: " + std::string(strerror(errno));
    close(fd);
    return -1;
  }
  hostname[sizeof(hostname) - 1] = '\0';

  WorkerState::setCurrentState(WorkerState::RUNNING);
  DmtcpMessage hello(DMT_REGISTER_ONLY_WORKER);
  hello.realPid = getpid();

  const size_t hostnameLength = strlen(hostname) + 1;
  const size_t programLength = strlen(programName) + 1;
  if (hostnameLength + programLength > DMTCP_MAX_MESSAGE_EXTRA_BYTES) {
    *error = "process identity exceeds coordinator protocol limit";
    close(fd);
    return -1;
  }
  std::vector<char> processInfo(hostnameLength + programLength);
  memcpy(processInfo.data(), hostname, hostnameLength);
  memcpy(processInfo.data() + hostnameLength, programName, programLength);
  hello.extraBytes = static_cast<uint32_t>(processInfo.size());

  if (!writeAll(fd, &hello, sizeof(hello)) ||
      !writeAll(fd, processInfo.data(), processInfo.size())) {
    *error = "cannot send registration handshake: " +
             std::string(strerror(errno));
    close(fd);
    return -1;
  }

  DmtcpMessage reply;
  reply.poison();
  if (!readAll(fd, &reply, sizeof(reply)) || !reply.isValid() ||
      reply.type != DMT_ACCEPT) {
    *error = "coordinator rejected registration-only worker";
    close(fd);
    return -1;
  }

  const int descriptorFlags = fcntl(fd, F_GETFD);
  if (descriptorFlags == -1 ||
      fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == -1) {
    *error = "cannot protect coordinator descriptor across exec: " +
             std::string(strerror(errno));
    close(fd);
    return -1;
  }

  return fd;
}

void
monitorCoordinator(int coordinatorFd)
{
  while (true) {
    DmtcpMessage message;
    message.poison();
    if (!readAll(coordinatorFd, &message, sizeof(message))) {
      break;
    }
    if (!message.isValid()) {
      break;
    }

    if (message.type == DMT_KILL_PEER) {
      _exit(0);
    }

    if (message.extraBytes > 0) {
      std::vector<char> ignored(message.extraBytes);
      if (!readAll(coordinatorFd, ignored.data(), ignored.size())) {
        break;
      }
    }

    if (message.type == DMT_DO_CHECKPOINT) {
      static constexpr char warning[] =
        "dmtcp_launch: coordinator sent an unsupported checkpoint request\n";
      write(STDERR_FILENO, warning, sizeof(warning) - 1);
    }
  }
  close(coordinatorFd);
}

} // namespace Registration
} // namespace dmtcp
