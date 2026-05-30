// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "backend.hpp"

namespace cn
{

bool PowVerifyBackend::submitLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                                      const crypto::Hash& blockId, uint32_t blockHeight)
{
  (void)blockHeight;
  crypto::Hash out;
  return computeLonghash(ctx, data, length, out);
}

bool PowVerifyBackend::tryConsumeLonghash(uint32_t blockHeight, const crypto::Hash& blockId,
                                          crypto::Hash& out)
{
  (void)blockHeight;
  (void)blockId;
  (void)out;
  return false;
}

bool PowVerifyBackend::tryGetLonghash(const crypto::Hash& blockId, crypto::Hash& out)
{
  (void)blockId;
  (void)out;
  return false;
}

bool PowVerifyBackend::awaitLonghash(const crypto::Hash& blockId, crypto::Hash& out)
{
  (void)blockId;
  (void)out;
  return false;
}

} // namespace cn
