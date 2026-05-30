// Copyright (c) 2019 The Circle Foundation
// Copyright (c) 2019 fireice-uk
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow_hash/cn_slow_hash.hpp"
#include "cryptonight.hpp"

#include <cstdio>
#include <cstdlib>

namespace crypto {

namespace
{
bool g_cnGpuInnerTrace = false;
} // namespace

bool cn_gpu_inner_trace_enabled()
{
  if (g_cnGpuInnerTrace)
    return true;
  const char* env = std::getenv("CN_GPU_INNER_TRACE");
  return env && env[0] == '1' && env[1] == '\0';
}

void cn_gpu_set_inner_trace(bool enable) { g_cnGpuInnerTrace = enable; }

void cn_gpu_dump_scratchpad_words(const char* tag, const uint8_t* scratchpad, size_t off,
                                  size_t count)
{
  if (!scratchpad || count == 0)
    return;
  const int32_t* words = reinterpret_cast<const int32_t*>(scratchpad + off);
  std::fprintf(stderr, "[cn-gpu-trace] %s scratchpad+%zu:", tag, off);
  for (size_t i = 0; i < count; ++i)
    std::fprintf(stderr, " %d", words[i]);
  std::fprintf(stderr, "\n");
}


void cn_slow_hash_v0(cn_context &context, const void *data, size_t length, Hash &hash) {
	if(hw_check_aes())
		cryptonight_hash<false, CRYPTONIGHT>(data, length, reinterpret_cast<char *>(&hash), context);
	else
		cryptonight_hash<true, CRYPTONIGHT>(data, length, reinterpret_cast<char *>(&hash), context);
}

void cn_fast_slow_hash_v1(cn_context &context, const void *data, size_t length, Hash &hash) {
	if(hw_check_aes())
		cryptonight_hash<false, CRYPTONIGHT_FAST_V8>(data, length, reinterpret_cast<char *>(&hash), context);
	else
		cryptonight_hash<true, CRYPTONIGHT_FAST_V8>(data, length, reinterpret_cast<char *>(&hash), context);
}

void cn_conceal_slow_hash_v0(cn_context &context, const void *data, size_t length, Hash &hash) {
	if(hw_check_aes())
		cryptonight_hash<false, CRYPTONIGHT_CONCEAL>(data, length, reinterpret_cast<char *>(&hash), context);
	else
		cryptonight_hash<true, CRYPTONIGHT_CONCEAL>(data, length, reinterpret_cast<char *>(&hash), context);
}

void cn_gpu_hash_v0(cn_context &context, const void *data, size_t length, Hash &hash) {
    context.cn_gpu_state.hash(data, length, reinterpret_cast<char *>(&hash));
}

void cn_gpu_prepare_inner(cn_context &context, const void *data, size_t length) {
  context.cn_gpu_state.cn_gpu_prepare_inner(data, length);
}

void cn_gpu_prepare_mining(cn_context &context, const void *data, size_t length, uint32_t nonce) {
  context.cn_gpu_state.cn_gpu_prepare_mining(data, length, nonce);
}

void cn_gpu_run_inner(cn_context &context) {
  context.cn_gpu_state.cn_gpu_run_inner();
}

void cn_gpu_run_inner_reference(cn_context &context) {
  context.cn_gpu_state.cn_gpu_run_inner_reference();
}

void cn_gpu_run_inner_reference_iters(cn_context &context, size_t iters) {
  context.cn_gpu_state.cn_gpu_run_inner_reference_iters(iters);
}

void cn_gpu_run_inner_sse_reference_iters(cn_context &context, size_t iters) {
  context.cn_gpu_state.cn_gpu_run_inner_sse_iters(iters);
}

void cn_gpu_hash_staged_reference(cn_context &context, const void *data, size_t length, Hash &hash) {
  cn_gpu_prepare_inner(context, data, length);
  cn_gpu_run_inner_reference(context);
  cn_gpu_finish_hash(context, hash);
}

void cn_gpu_finish_hash(cn_context &context, Hash &hash) {
  context.cn_gpu_state.cn_gpu_finish(reinterpret_cast<char *>(&hash));
}

uint8_t* cn_gpu_scratchpad_ptr(cn_context &context) {
  return context.cn_gpu_state.scratchpad_data();
}

uint8_t* cn_gpu_state_ptr(cn_context &context) {
  return context.cn_gpu_state.state_data();
}

size_t cn_gpu_scratchpad_bytes() { return cn_v3_hash_t::memory_bytes(); }
size_t cn_gpu_state_bytes() { return 200; }
}
