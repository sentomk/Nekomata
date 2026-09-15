#include "neko_lock.h"

#define NOMINMAX /* windows.h macros must not eat min/max at call sites */
#include <windows.h>

int neko_lock_acquire(neko_lock* lock, const char* path) {
  lock->handle = NULL;

  const int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (length <= 0) {
    return neko_lock_open_failed;
  }
  wchar_t wide[MAX_PATH];
  if (length > MAX_PATH || MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH) != length) {
    return neko_lock_open_failed;
  }

  HANDLE handle =
      CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return neko_lock_open_failed;
  }

  OVERLAPPED overlapped;
  ZeroMemory(&overlapped, sizeof(overlapped));
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
    CloseHandle(handle);
    return neko_lock_acquire_failed;
  }
  lock->handle = handle;
  return neko_lock_ok;
}

void neko_lock_release(neko_lock* lock) {
  if (lock->handle != NULL) {
    HANDLE handle = (HANDLE)lock->handle;
    OVERLAPPED overlapped;
    ZeroMemory(&overlapped, sizeof(overlapped));
    UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle);
    lock->handle = NULL;
  }
}
