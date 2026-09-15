#include "neko_file.h"

#define NOMINMAX /* windows.h macros must not eat min/max at call sites */
#include <stdint.h>
#include <windows.h>

static int utf8_to_wide(const char* path, wchar_t* wide, int capacity) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (length <= 0 || length > capacity) {
    return 0;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, capacity) != length) {
    return 0;
  }
  return length;
}

int neko_file_read(const char* path, neko_file_contents* out) {
  out->data = NULL;
  out->size = 0;

  wchar_t wide[MAX_PATH];
  if (!utf8_to_wide(path, wide, MAX_PATH)) {
    return neko_file_missing;
  }
  /* fstream opened files deny nothing; keep that leniency so concurrent
   * readers/writers of one stream keep working. */
  HANDLE handle =
      CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return neko_file_missing;
  }

  LARGE_INTEGER size;
  if (GetFileSizeEx(handle, &size) == 0 || size.QuadPart < 0) {
    CloseHandle(handle);
    return neko_file_io_failed;
  }

  size_t capacity = (size_t)size.QuadPart;
  if (capacity == 0) {
    /* Slack for files that grow between sizing and reading. */
    capacity = 4096;
  }
  unsigned char* data = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, capacity);
  if (data == NULL) {
    CloseHandle(handle);
    return neko_file_io_failed;
  }

  size_t used = 0;
  for (;;) {
    if (used == capacity) {
      if (capacity > SIZE_MAX / 2) {
        HeapFree(GetProcessHeap(), 0, data);
        CloseHandle(handle);
        return neko_file_io_failed;
      }
      capacity *= 2;
      unsigned char* bigger = (unsigned char*)HeapReAlloc(GetProcessHeap(), 0, data, capacity);
      if (bigger == NULL) {
        HeapFree(GetProcessHeap(), 0, data);
        CloseHandle(handle);
        return neko_file_io_failed;
      }
      data = bigger;
    }
    DWORD taken = 0;
    const DWORD chunk = (capacity - used) > MAXDWORD ? MAXDWORD : (DWORD)(capacity - used);
    if (ReadFile(handle, data + used, chunk, &taken, NULL) == 0) {
      HeapFree(GetProcessHeap(), 0, data);
      CloseHandle(handle);
      return neko_file_io_failed;
    }
    if (taken == 0) {
      break;
    }
    used += taken;
  }

  CloseHandle(handle);
  out->data = data;
  out->size = used;
  return neko_file_ok;
}

void neko_file_contents_free(neko_file_contents* contents) {
  if (contents != NULL && contents->data != NULL) {
    HeapFree(GetProcessHeap(), 0, contents->data);
    contents->data = NULL;
    contents->size = 0;
  }
}

int neko_file_write(const char* path, const void* data, size_t size) {
  wchar_t wide[MAX_PATH];
  if (!utf8_to_wide(path, wide, MAX_PATH)) {
    return neko_file_io_failed;
  }
  HANDLE handle = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return neko_file_io_failed;
  }
  const unsigned char* cursor = (const unsigned char*)data;
  while (size > 0) {
    const DWORD chunk = size > MAXDWORD ? MAXDWORD : (DWORD)size;
    DWORD written = 0;
    if (WriteFile(handle, cursor, chunk, &written, NULL) == 0) {
      CloseHandle(handle);
      return neko_file_io_failed;
    }
    cursor += written;
    size -= written;
  }
  if (CloseHandle(handle) == 0) {
    return neko_file_io_failed;
  }
  return neko_file_ok;
}
