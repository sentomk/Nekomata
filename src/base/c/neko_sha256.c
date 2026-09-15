#include "neko_sha256.h"

static const uint32_t k_round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static const uint32_t k_initial_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

static uint32_t rotate_right(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}

static void compress_block(const uint8_t* block, uint32_t* state) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
           ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];

  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const uint32_t choose = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + choose + k_round_constants[i] + w[i];
    const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

/* Padding bytes must not advance `byte_count`; the bit length is captured
 * before padding starts. */
static void append_padding_byte(neko_sha256_context* context, uint8_t value) {
  context->buffer[context->buffer_size] = value;
  context->buffer_size += 1;
  if (context->buffer_size == 64) {
    compress_block(context->buffer, context->state);
    context->buffer_size = 0;
  }
}

void neko_sha256_init(neko_sha256_context* context) {
  for (size_t i = 0; i < 8; ++i) {
    context->state[i] = k_initial_state[i];
  }
  context->byte_count = 0;
  context->buffer_size = 0;
}

void neko_sha256_update(neko_sha256_context* context, const void* data, size_t size) {
  const uint8_t* cursor = (const uint8_t*)data;
  context->byte_count += size;

  while (size > 0) {
    if (context->buffer_size == 0 && size >= 64) {
      compress_block(cursor, context->state);
      cursor += 64;
      size -= 64;
      continue;
    }
    const size_t taken = 64 - context->buffer_size;
    const size_t chunk = size < taken ? size : taken;
    for (size_t i = 0; i < chunk; ++i) {
      context->buffer[context->buffer_size + i] = cursor[i];
    }
    context->buffer_size += chunk;
    cursor += chunk;
    size -= chunk;
    if (context->buffer_size == 64) {
      compress_block(context->buffer, context->state);
      context->buffer_size = 0;
    }
  }
}

void neko_sha256_final(neko_sha256_context* context, uint8_t digest[32]) {
  const uint64_t bit_length = context->byte_count * 8;

  append_padding_byte(context, 0x80);
  while (context->buffer_size != 56) {
    append_padding_byte(context, 0);
  }
  for (int i = 7; i >= 0; --i) {
    append_padding_byte(context, (uint8_t)(bit_length >> (i * 8)));
  }

  for (size_t i = 0; i < 8; ++i) {
    digest[4 * i] = (uint8_t)(context->state[i] >> 24);
    digest[4 * i + 1] = (uint8_t)(context->state[i] >> 16);
    digest[4 * i + 2] = (uint8_t)(context->state[i] >> 8);
    digest[4 * i + 3] = (uint8_t)(context->state[i]);
  }
}
