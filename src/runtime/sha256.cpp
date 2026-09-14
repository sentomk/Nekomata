#include "sha256.hpp"

#include <vector>

namespace neko::detail {
namespace {

constexpr std::array<std::uint32_t, 64> k_round_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::array<std::uint32_t, 8> k_initial_state = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32 - bits));
}

void compress_block(const std::uint8_t* block, std::array<std::uint32_t, 8>& state) {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t{block[4 * i]} << 24) | (std::uint32_t{block[4 * i + 1]} << 16) |
           (std::uint32_t{block[4 * i + 2]} << 8) | std::uint32_t{block[4 * i + 3]};
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choose + k_round_constants[i] + w[i];
    const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;

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

} // namespace

std::array<std::uint8_t, 32> sha256_digest(std::span<const std::uint8_t> data) {
  std::array<std::uint32_t, 8> state = k_initial_state;

  // Message padding: 0x80, zeroes to 56 mod 64, then the big-endian bit length.
  std::vector<std::uint8_t> padded(data.begin(), data.end());
  const std::uint64_t bit_length = std::uint64_t{data.size()} * 8;
  padded.push_back(0x80);
  while (padded.size() % 64 != 56) {
    padded.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    padded.push_back(static_cast<std::uint8_t>(bit_length >> (i * 8)));
  }

  for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
    compress_block(padded.data() + offset, state);
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < state.size(); ++i) {
    digest[4 * i] = static_cast<std::uint8_t>(state[i] >> 24);
    digest[4 * i + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    digest[4 * i + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    digest[4 * i + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

std::string sha256_hex(std::span<const std::uint8_t> data) {
  static constexpr char k_digits[] = "0123456789abcdef";
  const auto digest = sha256_digest(data);
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    hex.push_back(k_digits[byte >> 4]);
    hex.push_back(k_digits[byte & 0x0f]);
  }
  return hex;
}

std::string sha256_hex(std::string_view text) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  return sha256_hex(std::span<const std::uint8_t>{bytes, text.size()});
}

} // namespace neko::detail
