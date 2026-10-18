/****************************************************************************
 * Process-creation restrictions for the registration-only macOS runtime.   *
 ****************************************************************************/

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <unistd.h>

namespace {

extern "C" pid_t
dmtcpUnsupportedFork()
{
  errno = ENOTSUP;
  return -1;
}

extern "C" pid_t
dmtcpUnsupportedVfork()
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedPosixSpawn(pid_t *,
                           const char *,
                           const posix_spawn_file_actions_t *,
                           const posix_spawnattr_t *,
                           char *const [],
                           char *const [])
{
  return ENOTSUP;
}

extern "C" int
dmtcpUnsupportedPosixSpawnp(pid_t *,
                            const char *,
                            const posix_spawn_file_actions_t *,
                            const posix_spawnattr_t *,
                            char *const [],
                            char *const [])
{
  return ENOTSUP;
}

extern "C" int
dmtcpUnsupportedSystem(const char *)
{
  errno = ENOTSUP;
  return -1;
}

extern "C" FILE *
dmtcpUnsupportedPopen(const char *, const char *)
{
  errno = ENOTSUP;
  return nullptr;
}

extern "C" int
dmtcpUnsupportedExecl(const char *, const char *, ...)
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedExecle(const char *, const char *, ...)
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedExeclp(const char *, const char *, ...)
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedExecv(const char *, char *const [])
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedExecve(const char *, char *const [], char *const [])
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedExecvp(const char *, char *const [])
{
  errno = ENOTSUP;
  return -1;
}

extern "C" int
dmtcpUnsupportedExecvP(const char *, const char *, char *const [])
{
  errno = ENOTSUP;
  return -1;
}

struct InterposeEntry {
  const void *replacement;
  const void *replacee;
};

#define DMTCP_INTERPOSE(replacement, replacee) \
  { reinterpret_cast<const void *>(replacement), \
    reinterpret_cast<const void *>(replacee) }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
__attribute__((used, section("__DATA,__interpose")))
static const InterposeEntry interposeEntries[] = {
  DMTCP_INTERPOSE(dmtcpUnsupportedFork, fork),
  DMTCP_INTERPOSE(dmtcpUnsupportedVfork, vfork),
  DMTCP_INTERPOSE(dmtcpUnsupportedPosixSpawn, posix_spawn),
  DMTCP_INTERPOSE(dmtcpUnsupportedPosixSpawnp, posix_spawnp),
  DMTCP_INTERPOSE(dmtcpUnsupportedSystem, system),
  DMTCP_INTERPOSE(dmtcpUnsupportedPopen, popen),
  DMTCP_INTERPOSE(dmtcpUnsupportedExecl, execl),
  DMTCP_INTERPOSE(dmtcpUnsupportedExecle, execle),
  DMTCP_INTERPOSE(dmtcpUnsupportedExeclp, execlp),
  DMTCP_INTERPOSE(dmtcpUnsupportedExecv, execv),
  DMTCP_INTERPOSE(dmtcpUnsupportedExecve, execve),
  DMTCP_INTERPOSE(dmtcpUnsupportedExecvp, execvp),
  DMTCP_INTERPOSE(dmtcpUnsupportedExecvP, execvP),
};
#pragma clang diagnostic pop

#undef DMTCP_INTERPOSE

} // namespace
