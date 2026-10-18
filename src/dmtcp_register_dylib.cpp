/****************************************************************************
 * macOS registration-only runtime loaded by DYLD_INSERT_LIBRARIES.         *
 ****************************************************************************/

#include "coordinatorapi_registration.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "constants.h"
#include "dmtcp.h"
#include "util.h"

namespace {

std::atomic<int> coordinatorFd{-1};

std::string
programName()
{
  uint32_t capacity = 0;
  _NSGetExecutablePath(nullptr, &capacity);
  std::vector<char> path(capacity + 1);
  if (_NSGetExecutablePath(path.data(), &capacity) != 0) {
    return "unknown";
  }
  path.back() = '\0';
  const char *name = strrchr(path.data(), '/');
  return name == nullptr ? path.data() : name + 1;
}

bool
parsePort(const char *text, int *port)
{
  if (text == nullptr || text[0] == '\0') {
    *port = DEFAULT_PORT;
    return true;
  }

  char *end = nullptr;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 1 ||
      value > 65535) {
    return false;
  }
  *port = static_cast<int>(value);
  return true;
}

void
terminateForkChild()
{
  // The registration-only runtime cannot track a second process safely.  The
  // public process-creation APIs are interposed, and this handler is a final
  // guard for code paths that reach fork through another symbol.
  _exit(DMTCP_FAIL_RC);
}

void *
monitorCoordinator(void *argument)
{
  const int fd = static_cast<int>(reinterpret_cast<intptr_t>(argument));
  dmtcp::Registration::monitorCoordinator(fd);
  int expected = fd;
  coordinatorFd.compare_exchange_strong(expected, -1);
  return nullptr;
}

} // namespace

extern "C" void __attribute__((constructor))
dmtcpRegisterProcess()
{
  const char *enabled = getenv(dmtcp::Registration::kEnabledEnv);
  if (enabled == nullptr || strcmp(enabled, "1") != 0) {
    return;
  }

  initializeJalib();

  const char *host = getenv(ENV_VAR_NAME_HOST);
  if (host == nullptr || host[0] == '\0') {
    host = DEFAULT_HOST;
  }

  int port = DEFAULT_PORT;
  if (!parsePort(getenv(ENV_VAR_NAME_PORT), &port)) {
    dprintf(STDERR_FILENO,
            "dmtcp_launch: invalid coordinator port in %s\n",
            ENV_VAR_NAME_PORT);
    _exit(DMTCP_FAIL_RC);
  }

  const std::string name = programName();
  std::string error;
  const int fd = dmtcp::Registration::connectToCoordinator(
    host, port, name.c_str(), &error);
  if (fd == -1) {
    dprintf(STDERR_FILENO, "dmtcp_launch: %s\n", error.c_str());
    _exit(DMTCP_FAIL_RC);
  }
  coordinatorFd.store(fd);

  const int atForkResult = pthread_atfork(nullptr, nullptr,
                                          terminateForkChild);
  if (atForkResult != 0) {
    dprintf(STDERR_FILENO,
            "dmtcp_launch: cannot install fork handler: %s\n",
            strerror(atForkResult));
    _exit(DMTCP_FAIL_RC);
  }

  pthread_t monitorThread;
  const int threadResult = pthread_create(
    &monitorThread, nullptr, monitorCoordinator,
    reinterpret_cast<void *>(static_cast<intptr_t>(fd)));
  if (threadResult != 0) {
    dprintf(STDERR_FILENO,
            "dmtcp_launch: cannot create coordinator monitor: %s\n",
            strerror(threadResult));
    _exit(DMTCP_FAIL_RC);
  }
  pthread_detach(monitorThread);
}
