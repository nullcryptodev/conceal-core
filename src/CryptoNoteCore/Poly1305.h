// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cn
{

  //
  // Poly1305 one-time authenticator (RFC 7539).
  //
  // Produces a 16-byte authentication tag given a 32-byte one-time key
  // and a message of arbitrary length.
  //
  // SECURITY: The key MUST be used only once per message.
  //
  class Poly1305
  {
  public:
    enum
    {
      KEY_SIZE = 32,
      TAG_SIZE = 16
    };

    //
    // Compute a Poly1305 MAC.
    // @param key  32-byte one-time key (must be unique per message)
    // @param msg  Message to authenticate
    // @return     16-byte authentication tag
    //
    static std::vector<uint8_t> mac(
        const uint8_t *key,
        const std::vector<uint8_t> &msg);

    //
    // Verify a Poly1305 MAC in constant time.
    // @param key  32-byte one-time key
    // @param msg  Message to verify
    // @param tag  16-byte expected tag
    // @return     true if the tag matches
    //
    static bool verify(
        const uint8_t *key,
        const std::vector<uint8_t> &msg,
        const uint8_t *tag);

  private:
    Poly1305() {}
  };

} // namespace cn