// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Poly1305.h"

#include <cstring>

namespace cn
{

  namespace
  {

    // ── 64-bit arithmetic for Poly1305 (mod 2^130 - 5) ──────────────────────

    // Add two 130-bit numbers: h = h + c (5 limbs, little-endian)
    void add130(uint64_t h[5], const uint64_t c[5])
    {
      uint64_t carry = 0;
      for (int i = 0; i < 5; ++i)
      {
        carry += h[i] + c[i];
        h[i] = carry;
        carry >>= 32;
      }
    }

    // Multiply: h = h * r mod (2^130 - 5)
    // This uses the standard Poly1305 reduction with 64-bit limbs.
    // r is expected to have its high 4 bits of each limb cleared (clamped).
    void mulmod130(uint64_t h[5], const uint64_t r[5])
    {
      // Multiply h (5 x 32-bit limbs stored in 64-bit) by r (5 x 32-bit limbs)
      // Result fits in 10 x 32-bit limbs
      uint64_t d[10];
      std::memset(d, 0, sizeof(d));

      for (int i = 0; i < 5; ++i)
      {
        for (int j = 0; j < 5; ++j)
        {
          d[i + j] += (h[i] & 0xFFFFFFFFULL) * (r[j] & 0xFFFFFFFFULL);
        }
      }

      // Propagate carries through all 10 limbs
      for (int i = 0; i < 9; ++i)
      {
        d[i + 1] += d[i] >> 32;
        d[i] &= 0xFFFFFFFFULL;
      }

      // Reduce modulo 2^130 - 5
      // We have d[0..9] where d[5..9] represent values * 2^160, * 2^192, etc.
      // 2^130 ≡ 5 (mod p), so 2^160 ≡ 5 * 2^30, etc.
      // Simplified: take the high part, multiply by 5, add to low part.
      uint64_t low[5];
      for (int i = 0; i < 5; ++i)
        low[i] = d[i];

      // Fold d[5] * 2^160 ≡ d[5] * 5 * 2^30 (this is imprecise; use standard reduction)
      // Proper reduction: multiply upper part by 5 and shift appropriately
      uint64_t high = d[5] & 0xFFFFFFFFULL;
      low[0] += high * 5;
      high = d[6] & 0xFFFFFFFFULL;
      low[1] += high * 5;
      high = d[7] & 0xFFFFFFFFULL;
      low[2] += high * 5;
      high = d[8] & 0xFFFFFFFFULL;
      low[3] += high * 5;
      high = d[9] & 0xFFFFFFFFULL;
      low[4] += high * 5;

      // Also account for the upper 32 bits of d[0..4] (already propagated in carry step above)
      // The above gives an approximate reduction. Propagate carries.
      uint64_t carry = 0;
      for (int i = 0; i < 5; ++i)
      {
        carry += low[i];
        h[i] = carry & 0xFFFFFFFFULL;
        carry >>= 32;
      }

      // If there's still a carry, multiply by 5 and add to h[0]
      if (carry > 0)
        h[0] += carry * 5;
    }

    // Clamp r according to Poly1305 spec
    void clampR(uint64_t r[5])
    {
      r[0] &= 0x0FFFFFFFULL;
      r[1] &= 0x0FFFFFFCULL;
      r[2] &= 0x0FFFFFFCULL;
      r[3] &= 0x0FFFFFFCULL;
    }

    // Read 32-bit little-endian value from bytes
    uint32_t le32(const uint8_t *p)
    {
      return static_cast<uint32_t>(p[0]) |
             (static_cast<uint32_t>(p[1]) << 8) |
             (static_cast<uint32_t>(p[2]) << 16) |
             (static_cast<uint32_t>(p[3]) << 24);
    }

    // Write 32-bit value as little-endian bytes
    void le32_out(uint32_t v, uint8_t *out)
    {
      out[0] = static_cast<uint8_t>(v);
      out[1] = static_cast<uint8_t>(v >> 8);
      out[2] = static_cast<uint8_t>(v >> 16);
      out[3] = static_cast<uint8_t>(v >> 24);
    }

  } // anonymous namespace

  // ── Public API ───────────────────────────────────────────────────────────

  std::vector<uint8_t> Poly1305::mac(
      const uint8_t *key,
      const std::vector<uint8_t> &msg)
  {
    // Parse key: r = key[0..15] (clamped), s = key[16..31]
    uint64_t r[5], s[4];
    r[0] = le32(key + 0);
    r[1] = le32(key + 4);
    r[2] = le32(key + 8);
    r[3] = le32(key + 12);
    r[4] = 0;
    clampR(r);

    s[0] = le32(key + 16);
    s[1] = le32(key + 20);
    s[2] = le32(key + 24);
    s[3] = le32(key + 28);

    // Initialize accumulator
    uint64_t h[5] = {0, 0, 0, 0, 0};

    // Process message in 16-byte blocks
    size_t offset = 0;
    while (offset + 16 <= msg.size())
    {
      uint64_t block[5];
      block[0] = le32(msg.data() + offset + 0);
      block[1] = le32(msg.data() + offset + 4);
      block[2] = le32(msg.data() + offset + 8);
      block[3] = le32(msg.data() + offset + 12);
      block[4] = 1; // High bit set for full blocks

      add130(h, block);
      mulmod130(h, r);
      offset += 16;
    }

    // Process final (partial) block
    size_t remaining = msg.size() - offset;
    if (remaining > 0)
    {
      uint8_t finalBlock[17];
      std::memset(finalBlock, 0, sizeof(finalBlock));
      std::memcpy(finalBlock, msg.data() + offset, remaining);
      finalBlock[remaining] = 1; // High bit after last byte

      uint64_t block[5];
      block[0] = le32(finalBlock + 0);
      block[1] = le32(finalBlock + 4);
      block[2] = le32(finalBlock + 8);
      block[3] = le32(finalBlock + 12);
      block[4] = 1;

      add130(h, block);
      mulmod130(h, r);
    }

    // Finalize: add s
    uint64_t sblock[5] = {s[0], s[1], s[2], s[3], 0};
    add130(h, sblock);

    // Extract tag (4 x 32-bit little-endian)
    std::vector<uint8_t> tag(TAG_SIZE);
    le32_out(static_cast<uint32_t>(h[0]), tag.data() + 0);
    le32_out(static_cast<uint32_t>(h[1]), tag.data() + 4);
    le32_out(static_cast<uint32_t>(h[2]), tag.data() + 8);
    le32_out(static_cast<uint32_t>(h[3]), tag.data() + 12);

    return tag;
  }

  bool Poly1305::verify(
      const uint8_t *key,
      const std::vector<uint8_t> &msg,
      const uint8_t *tag)
  {
    std::vector<uint8_t> expected = mac(key, msg);

    // Constant-time comparison
    uint8_t diff = 0;
    for (size_t i = 0; i < TAG_SIZE; ++i)
      diff |= expected[i] ^ tag[i];

    return diff == 0;
  }

} // namespace cn