#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Cross-process exclusive file lock, blocking acquire.
 *
 * Private internal ABI of the neko base layer: not installed, not part of
 * the public interface, no compatibility promise. The lock only expresses
 * open/acquire/release; policy (what a lock protects, how long it is held)
 * belongs to the C++ caller. */
typedef struct neko_lock {
  void* handle; /* fd on POSIX (cast), HANDLE on Windows */
} neko_lock;

enum { neko_lock_ok = 0, neko_lock_open_failed = 1, neko_lock_acquire_failed = 2 };

/* `path` is UTF-8. Blocks until the lock is held. Returns a neko_lock_*
 * status; the OS error detail stays in errno / GetLastError(). */
int neko_lock_acquire(neko_lock* lock, const char* path);

/* Releases the lock and closes the underlying handle. No-op when the lock
 * was never acquired. */
void neko_lock_release(neko_lock* lock);

#ifdef __cplusplus
}
#endif
