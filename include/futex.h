#ifndef DMTCP_FUTEX_H
#define DMTCP_FUTEX_H

#include <stdint.h>

#ifdef __APPLE__

#include <atomic>
#include <cerrno>

// DMTCP's control-plane allocator uses only FUTEX_WAIT and FUTEX_WAKE
// semantics.  std::atomic_ref provides the same wait-until-changed behavior
// without relying on Linux system calls.  All users provide naturally aligned
// uint32_t storage.
static inline int
futex_wait(uint32_t *uaddr, uint32_t old_val)
{
  std::atomic_ref<uint32_t> value(*uaddr);
  if (value.load(std::memory_order_seq_cst) != old_val) {
    errno = EAGAIN;
    return -1;
  }
  value.wait(old_val, std::memory_order_seq_cst);
  return 0;
}

static inline int
futex_wake(uint32_t *uaddr, uint32_t num)
{
  std::atomic_ref<uint32_t> value(*uaddr);
  if (num == 1) {
    value.notify_one();
  } else {
    value.notify_all();
  }
  return 0;
}

#else // ifdef __APPLE__

#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>

static inline int
futex(uint32_t *uaddr,
      int futex_op,
      uint32_t val,
      const struct timespec *timeout,
      int *uaddr2,
      int val3)
{
  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr, val3);
}

static inline int
futex_wait(uint32_t *uaddr, uint32_t old_val)
{
  return futex(uaddr, FUTEX_WAIT, old_val, NULL, NULL, 0);
}

static inline int
futex_wake(uint32_t *uaddr, uint32_t num)
{
  return futex(uaddr, FUTEX_WAKE, num, NULL, NULL, 0);
}

#endif // ifdef __APPLE__

#endif // DMTCP_FUTEX_H
