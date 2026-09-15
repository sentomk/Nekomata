#include "neko_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/file.h>
#include <unistd.h>

int neko_lock_acquire(neko_lock* lock, const char* path) {
  lock->handle = NULL;
  const int fd = open(path, O_CREAT | O_RDWR, 0666);
  if (fd < 0) {
    return neko_lock_open_failed;
  }
  /* BSD flock() locks the open file description, so threads of one process
   * exclude each other too. */
  if (flock(fd, LOCK_EX) != 0) {
    const int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return neko_lock_acquire_failed;
  }
  lock->handle = (void*)(intptr_t)fd;
  return neko_lock_ok;
}

void neko_lock_release(neko_lock* lock) {
  if (lock->handle != NULL) {
    close((int)(intptr_t)lock->handle);
    lock->handle = NULL;
  }
}
