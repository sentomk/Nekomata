#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-256 (FIPS 180-4), streaming form.
 *
 * Private internal ABI of the neko base layer: it is not installed, not
 * part of the public interface, and carries no compatibility promise. */
typedef struct neko_sha256_context {
  uint32_t state[8];
  uint64_t byte_count;
  uint8_t buffer[64];
  size_t buffer_size;
} neko_sha256_context;

void neko_sha256_init(neko_sha256_context* context);

void neko_sha256_update(neko_sha256_context* context, const void* data, size_t size);

void neko_sha256_final(neko_sha256_context* context, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
