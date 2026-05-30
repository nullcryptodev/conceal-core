// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT — debug helpers for CN-GPU OpenCL inner-loop equivalence

#pragma once

#include <cstddef>
#include <cstdint>

namespace crypto
{

/** True if CN_GPU_INNER_TRACE=1 or cn_gpu_set_inner_trace(true) was called */
bool cn_gpu_inner_trace_enabled();

void cn_gpu_set_inner_trace(bool enable);

/** Log `count` int32 words from scratchpad at byte offset `off` */
void cn_gpu_dump_scratchpad_words(const char* tag, const uint8_t* scratchpad, size_t off,
                                size_t count);

} // namespace crypto
