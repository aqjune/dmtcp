/****************************************************************************
 * Portable utility subset for DMTCP control-plane-only builds.             *
 ****************************************************************************/

#include "util.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "dmtcp_assert.h"

namespace dmtcp {

bool
Util::strStartsWith(const char *str, const char *pattern)
{
  if (str == nullptr || pattern == nullptr) {
    return false;
  }
  return strStartsWith(std::string_view(str), std::string_view(pattern));
}

bool
Util::strEndsWith(const char *str, const char *pattern)
{
  if (str == nullptr || pattern == nullptr) {
    return false;
  }
  return strEndsWith(std::string_view(str), std::string_view(pattern));
}

ssize_t
Util::writeAll(int fd, const void *buffer, size_t count)
{
  const char *bytes = static_cast<const char *>(buffer);
  size_t written = 0;
  while (written < count) {
    const ssize_t result = write(fd, bytes + written, count - written);
    if (result == -1) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return -1;
    }
    if (result == 0) {
      break;
    }
    written += static_cast<size_t>(result);
  }

  ASSERT(written == count,
         "short write in Util::writeAll: fd={} written={} expected={}",
         fd, written, count);
  return static_cast<ssize_t>(written);
}

ssize_t
Util::readAll(int fd, void *buffer, size_t count)
{
  char *bytes = static_cast<char *>(buffer);
  size_t readCount = 0;
  while (readCount < count) {
    const ssize_t result = read(fd, bytes + readCount, count - readCount);
    if (result == -1) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return -1;
    }
    if (result == 0) {
      break;
    }
    readCount += static_cast<size_t>(result);
  }
  return static_cast<ssize_t>(readCount);
}

string
Util::getTimestampStr()
{
  struct timeval now;
  struct tm localTime;
  char buffer[128] = {0};

  gettimeofday(&now, nullptr);
  localtime_r(&now.tv_sec, &localTime);
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
           localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
           localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
           static_cast<long>(now.tv_usec / 1000));
  return buffer;
}

} // namespace dmtcp
