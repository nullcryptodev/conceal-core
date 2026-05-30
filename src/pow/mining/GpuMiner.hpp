// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../CryptoNoteCore/CryptoNoteBasic.h"
#include "../../CryptoNoteCore/Currency.h"
#include "../../CryptoNoteCore/Difficulty.h"
#include "../../CryptoNoteCore/IMinerHandler.h"
#include "../../CryptoNoteCore/OnceInInterval.h"
#include "GpuMinerConfig.hpp"
#include "OpenclMinerDevice.hpp"

#include <Logging/LoggerRef.h>

namespace cn
{

class GpuMiner
{
public:
  GpuMiner(const Currency& currency, IMinerHandler& handler, logging::ILogger& log);
  ~GpuMiner();

  bool init(const GpuMinerConfig& config, int verifyOffloadDeviceIndex);
  void releaseVerifyOffloadDevice(int deviceIndex);
  bool set_block_template(const Block& bl, const difficulty_type& diffic);
  bool on_block_chain_update();
  bool start(const AccountPublicAddress& adr, const std::vector<GpuDeviceSpec>& devices);
  uint64_t get_speed();
  void send_stop_signal();
  bool stop();
  bool is_mining() const;
  bool on_idle();
  void on_synchronized();
  void pause();
  void resume();
  void do_print_hashrate(bool do_hr);
  void add_hashes(uint64_t n);

  bool worker_wait_template(Block& outBlock, difficulty_type& outDiff, uint32_t& outTemplateVer);
  void worker_report_found(const Block& block);

  bool is_paused() const { return m_pausers_count.load() > 0; }
  uint32_t starter_nonce() const { return m_starter_nonce.load(); }
  uint32_t total_host_threads() const { return m_total_host_threads; }
  void set_global_thread_base(uint32_t base) { m_global_thread_base = base; }
  uint32_t global_thread_base() const { return m_global_thread_base; }

private:
  bool request_block_template();
  void merge_hr();

  const Currency& m_currency;
  logging::LoggerRef logger;
  IMinerHandler& m_handler;

  std::atomic<bool> m_stop{true};
  std::mutex m_template_lock;
  Block m_template;
  std::atomic<uint32_t> m_template_no{0};
  std::atomic<uint32_t> m_starter_nonce{0};
  difficulty_type m_diffic = 0;

  std::atomic<int32_t> m_pausers_count{0};
  std::mutex m_threads_lock;
  AccountPublicAddress m_mine_address;
  std::vector<std::unique_ptr<OpenclMinerDevice>> m_devices;

  OnceInInterval m_update_block_template_interval;
  OnceInInterval m_update_merge_hr_interval;

  std::atomic<uint64_t> m_last_hr_merge_time{0};
  std::atomic<uint64_t> m_hashes{0};
  std::atomic<uint64_t> m_current_hash_rate{0};
  std::mutex m_last_hash_rates_lock;
  std::list<uint64_t> m_last_hash_rates;
  bool m_do_print_hashrate = false;
  bool m_do_mining = false;
  GpuMinerConfig m_config;
  int m_verifyOffloadDeviceIndex = -1;
  uint32_t m_total_host_threads = 0;
  uint32_t m_global_thread_base = 0;
};

} // namespace cn
