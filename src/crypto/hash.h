// Copyright (c) 2012-2016, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <stddef.h>

#include <CryptoTypes.h>
#include "generic-ops.h"
#include <boost/align/aligned_alloc.hpp>
#include "pow_hash/cn_slow_hash.hpp"

/* Standard Cryptonight */
#define CN_PAGE_SIZE                    2097152
#define CN_SCRATCHPAD                   2097152
#define CN_ITERATIONS                   1048576

/* Cryptonight Fast */
#define CN_FAST_PAGE_SIZE               2097152
#define CN_FAST_SCRATCHPAD              2097152
#define CN_FAST_ITERATIONS              524288

namespace crypto {

  extern "C" {
#include "hash-ops.h"
  }

  /*
    Cryptonight hash functions
  */

  inline void cn_fast_hash(const void *data, size_t length, Hash &hash) {
    cn_fast_hash(data, length, reinterpret_cast<char *>(&hash));
  }

  inline Hash cn_fast_hash(const void *data, size_t length) {
    Hash h;
    cn_fast_hash(data, length, reinterpret_cast<char *>(&h));
    return h;
  }

  class cn_context {
  public:

    cn_context()
    {
        long_state = (uint8_t*)boost::alignment::aligned_alloc(4096, CN_PAGE_SIZE);
        hash_state = (uint8_t*)boost::alignment::aligned_alloc(4096, 4096);
    }

    ~cn_context()
    {
        if(long_state != nullptr)
            boost::alignment::aligned_free(long_state);
        if(hash_state != nullptr)
            boost::alignment::aligned_free(hash_state);
    }

    cn_context(const cn_context &) = delete;
    void operator=(const cn_context &) = delete;

    cn_v3_hash_t cn_gpu_state;
    uint8_t* long_state = nullptr;
    uint8_t* hash_state = nullptr;
  };

  void cn_slow_hash_v0(cn_context &context, const void *data, size_t length, Hash &hash);
  void cn_fast_slow_hash_v1(cn_context &context, const void *data, size_t length, Hash &hash);
  void cn_conceal_slow_hash_v0(cn_context &context, const void *data, size_t length, Hash &hash);  
  void cn_gpu_hash_v0(cn_context &context, const void *data, size_t length, Hash &hash);
  void cn_gpu_prepare_inner(cn_context &context, const void *data, size_t length);
  /** Mining path: cn0 (keccak state + nonce inject) + cn00 (explode); matches OpenCL cn0_cn_gpu/cn00_cn_gpu. */
  void cn_gpu_prepare_mining(cn_context &context, const void *data, size_t length, uint32_t nonce);
  void cn_gpu_run_inner(cn_context &context);
  void cn_gpu_run_inner_reference(cn_context &context);
  void cn_gpu_run_inner_reference_iters(cn_context &context, size_t iters);
  /** SSE inner_hash_3_iters only (x86 diagnostic; not production when AVX2 is used) */
  void cn_gpu_run_inner_sse_reference_iters(cn_context &context, size_t iters);
  /** prepare + production inner (AVX2 on x86 when available) + finish; OpenCL equivalence reference */
  void cn_gpu_hash_staged_reference(cn_context &context, const void *data, size_t length, Hash &hash);
  void cn_gpu_finish_hash(cn_context &context, Hash &hash);
  uint8_t* cn_gpu_scratchpad_ptr(cn_context &context);
  uint8_t* cn_gpu_state_ptr(cn_context &context);
  size_t cn_gpu_scratchpad_bytes();
  size_t cn_gpu_state_bytes();

  bool cn_gpu_inner_trace_enabled();
  void cn_gpu_set_inner_trace(bool enable);
  void cn_gpu_dump_scratchpad_words(const char* tag, const uint8_t* scratchpad, size_t off,
                                    size_t count);

  inline void tree_hash(const Hash *hashes, size_t count, Hash &root_hash) {
    tree_hash(reinterpret_cast<const char (*)[HASH_SIZE]>(hashes), count, reinterpret_cast<char *>(&root_hash));
  }

  inline void tree_branch(const Hash *hashes, size_t count, Hash *branch) {
    tree_branch(reinterpret_cast<const char (*)[HASH_SIZE]>(hashes), count, reinterpret_cast<char (*)[HASH_SIZE]>(branch));
  }

  inline void tree_hash_from_branch(const Hash *branch, size_t depth, const Hash &leaf, const void *path, Hash &root_hash) {
    tree_hash_from_branch(reinterpret_cast<const char (*)[HASH_SIZE]>(branch), depth, reinterpret_cast<const char *>(&leaf), path, reinterpret_cast<char *>(&root_hash));
  }

}

CRYPTO_MAKE_HASHABLE(Hash)
