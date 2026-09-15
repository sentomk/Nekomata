#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whole-file mechanical I/O.
 *
 * Private internal ABI of the neko base layer: not installed, not part of
 * the public interface, no compatibility promise. The layer knows bytes and
 * paths only; generation identity, ready markers, and staging layout are
 * caller policy. `path` is UTF-8. */
typedef struct neko_file_contents {
  unsigned char* data; /* malloc'd by neko_file_read; free with neko_file_contents_free */
  size_t size;
} neko_file_contents;

/* C cannot pin an enum's underlying type; the 4-byte default is accepted
 * here. NOLINT on the enum line silences performance-enum-size for it. */
enum { /* NOLINT(performance-enum-size) */
       neko_file_ok = 0,
       neko_file_missing = 1,  /* not a regular file, or cannot be opened */
       neko_file_io_failed = 2 /* opened, but reading failed */
};

int neko_file_read(const char* path, neko_file_contents* out);

void neko_file_contents_free(neko_file_contents* contents);

/* Truncating whole-file write. Returns neko_file_ok or neko_file_io_failed. */
int neko_file_write(const char* path, const void* data, size_t size);

#ifdef __cplusplus
}
#endif
