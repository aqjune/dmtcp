/****************************************************************************
 * JALIB wiring for control-plane-only executables.                         *
 ****************************************************************************/

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef __APPLE__
# include <sys/syscall.h>
#endif

#include "../jalib/jalib.h"
#include "dmtcp.h"
#include "constants.h"
#include "protectedfds.h"
#include "util.h"

namespace {

long
systemCall(long number,
           long arg1,
           long arg2,
           long arg3,
           long arg4,
           long arg5,
           long arg6,
           long arg7)
{
#ifdef __APPLE__
  (void)number;
  (void)arg1;
  (void)arg2;
  (void)arg3;
  (void)arg4;
  (void)arg5;
  (void)arg6;
  (void)arg7;
  errno = ENOSYS;
  return -1;
#else
  return syscall(number, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
#endif
}

pid_t
currentThreadId()
{
#ifdef __APPLE__
  uint64_t threadId = 0;
  if (pthread_threadid_np(nullptr, &threadId) != 0) {
    return getpid();
  }
  return static_cast<pid_t>(threadId);
#else
  return static_cast<pid_t>(syscall(SYS_gettid));
#endif
}

} // namespace

extern "C" void
initializeJalib()
{
  JalibFuncPtrs functions = {};
  functions.open = ::open;
  functions.fopen = ::fopen;
  functions.close = ::close;
  functions.fclose = ::fclose;
  functions.dup = ::dup;
  functions.dup2 = ::dup2;
  functions.readlink = ::readlink;
  functions.syscall = systemCall;
  functions.socket = ::socket;
  functions.connect = ::connect;
  functions.bind = ::bind;
  functions.listen = ::listen;
  functions.accept = ::accept;
  functions.setsockopt = ::setsockopt;
  functions.writeAll = dmtcp::Util::writeAll;
  functions.readAll = dmtcp::Util::readAll;
  functions.getTimestampStr = dmtcp::Util::getTimestampStr;
  functions.gettid = currentThreadId;

  jalib_init(functions,
             nullptr,
             PROTECTED_STDERR_FD,
             PROTECTED_LOG_FD,
             DMTCP_FAIL_RC_PARAM);
}
