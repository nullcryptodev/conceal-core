// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../CryptoNoteCore/CryptoNoteBasic.h"
#include "../../CryptoNoteCore/Difficulty.h"
#include "../mining/GpuMinerConfig.hpp"

namespace cn
{

class GpuMiner;

class OpenclMinerDevice
{
public:
  OpenclMinerDevice(GpuMiner& owner, const GpuDeviceSpec& spec);
  ~OpenclMinerDevice();

  OpenclMinerDevice(const OpenclMinerDevice&) = delete;
  OpenclMinerDevice& operator=(const OpenclMinerDevice&) = delete;

  bool init(std::string& err);
  void setGlobalThreadBase(uint32_t base) { m_globalThreadBase = base; }
  void startWorkers();
  void stopWorkers();
  bool selfTest(std::string& err) const;

  int deviceIndex() const { return m_spec.deviceIndex; }
  uint32_t perThreadIntensity() const { return m_spec.perThreadIntensity; }

private:
  void workerLoop(uint32_t hostThreadIndex);
  bool runMiningBatch(uint32_t hostThreadIndex, const Block& block, difficulty_type diff,
                      uint32_t nonceBase, uint32_t jobCount, bool& found, uint32_t& foundNonce);

  GpuMiner& m_owner;
  GpuDeviceSpec m_spec;
  uint32_t m_globalThreadBase = 0;

  struct OclState;
  std::unique_ptr<OclState> m_ocl;

  std::vector<std::thread> m_threads;
  std::atomic<bool> m_stop{true};
};

} // namespace cn
