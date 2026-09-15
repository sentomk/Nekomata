#include "neko_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int neko_file_read(const char* path, neko_file_contents* out) {
  out->data = NULL;
  out->size = 0;

  const int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return neko_file_missing;
  }
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    const int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return neko_file_missing;
  }

  unsigned char* data = NULL;
  size_t used = 0;
  size_t capacity = (size_t)info.st_size;
  if (capacity == 0) {
    /* Slack for files that grow between fstat and read. */
    capacity = 4096;
  }
  data = (unsigned char*)malloc(capacity);
  if (data == NULL) {
    close(fd);
    return neko_file_io_failed;
  }
  for (;;) {
    if (used == capacity) {
      if (capacity > SIZE_MAX / 2) {
        free(data);
        close(fd);
        return neko_file_io_failed;
      }
      capacity *= 2;
      unsigned char* bigger = (unsigned char*)realloc(data, capacity);
      if (bigger == NULL) {
        free(data);
        close(fd);
        return neko_file_io_failed;
      }
      data = bigger;
    }
    const ssize_t taken = read(fd, data + used, capacity - used);
    if (taken < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(data);
      close(fd);
      return neko_file_io_failed;
    }
    if (taken == 0) {
      break;
    }
    used += (size_t)taken;
  }

  close(fd);
  out->data = data;
  out->size = used;
  return neko_file_ok;
}

void neko_file_contents_free(neko_file_contents* contents) {
  if (contents != NULL) {
    free(contents->data);
    contents->data = NULL;
    contents->size = 0;
  }
}

int neko_file_write(const char* path, const void* data, size_t size) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    return neko_file_io_failed;
  }
  const unsigned char* cursor = (const unsigned char*)data;
  while (size > 0) {
    const ssize_t written = write(fd, cursor, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return neko_file_io_failed;
    }
    cursor += (size_t)written;
    size -= (size_t)written;
  }
  if (close(fd) != 0) {
    return neko_file_io_failed;
  }
  return neko_file_ok;
}
