// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "GpuMiner.hpp"

#include <boost/utility/value_init.hpp>
#include <memory>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>

#include "../../CryptoNoteCore/CryptoNoteFormatUtils.h"

using namespace logging;

namespace cn
{

namespace
{
uint64_t millisecondsSinceEpoch()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
} // namespace

GpuMiner::GpuMiner(const Currency& currency, IMinerHandler& handler, logging::ILogger& log)
    : m_currency(currency),
      logger(log, "gpu-miner"),
      m_handler(handler),
      m_update_block_template_interval(5),
      m_update_merge_hr_interval(2)
{
}

GpuMiner::~GpuMiner()
{
  stop();
}

bool GpuMiner::init(const GpuMinerConfig& config, int verifyOffloadDeviceIndex)
{
#ifndef CONCEAL_WITH_OPENCL
  if (config.enabled())
  {
    logger(ERROR) << "GPU mining requested but OpenCL is not enabled (rebuild with -DWITH_OPENCL=ON)";
    return false;
  }
  return true;
#else
  m_config = config;
  m_do_mining = config.enabled();
  m_verifyOffloadDeviceIndex = verifyOffloadDeviceIndex;

  if (!m_do_mining)
    return true;

  if (!m_currency.parseAccountAddressString(config.rewardAddress, m_mine_address))
  {
    logger(ERROR) << "GPU mining reward address " << config.rewardAddress << " has wrong format";
    return false;
  }

  for (const auto& spec : config.devices)
  {
    if (m_verifyOffloadDeviceIndex >= 0 && spec.deviceIndex == m_verifyOffloadDeviceIndex)
    {
      logger(ERROR) << "GPU device " << spec.deviceIndex
                    << " is already used by --gpu-device verify offload; cannot mine on same device";
      return false;
    }
  }

  return true;
#endif
}

void GpuMiner::releaseVerifyOffloadDevice(int deviceIndex)
{
  if (m_verifyOffloadDeviceIndex == deviceIndex)
    m_verifyOffloadDeviceIndex = -1;
}

bool GpuMiner::set_block_template(const Block& bl, const difficulty_type& diffic)
{
  std::lock_guard<std::mutex> lk(m_template_lock);
  m_template = bl;
  m_diffic = diffic;
  ++m_template_no;
  m_starter_nonce = crypto::rand<uint32_t>();
  return true;
}

bool GpuMiner::request_block_template()
{
  Block bl = boost::value_initialized<Block>();
  difficulty_type di = 0;
  uint32_t height = 0;
  BinaryArray extra_nonce;
  if (!m_handler.get_block_template(bl, m_mine_address, di, height, extra_nonce))
  {
    logger(ERROR) << "Failed to get_block_template(), stopping GPU mining";
    return false;
  }
  set_block_template(bl, di);
  return true;
}

bool GpuMiner::on_block_chain_update()
{
  if (!is_mining())
    return true;
  return request_block_template();
}

bool GpuMiner::start(const AccountPublicAddress& adr, const std::vector<GpuDeviceSpec>& devices)
{
  if (is_mining())
  {
    logger(ERROR) << "Starting GPU miner but it is already started";
    return false;
  }

  std::lock_guard<std::mutex> lk(m_threads_lock);
  m_devices.clear();
  m_mine_address = adr;
  m_starter_nonce = crypto::rand<uint32_t>();
  m_total_host_threads = static_cast<uint32_t>(devices.size()) * GpuMinerConfig::kThreadsPerGpu;

  if (!m_template_no)
    request_block_template();

  for (const auto& spec : devices)
  {
    if (m_verifyOffloadDeviceIndex >= 0 && spec.deviceIndex == m_verifyOffloadDeviceIndex)
    {
      logger(ERROR) << "GPU device " << spec.deviceIndex
                    << " is still used by verify offload; wait until synchronized or use another device";
      return false;
    }
  }

  uint32_t threadBase = 0;
  for (const auto& spec : devices)
  {
    auto dev = std::unique_ptr<OpenclMinerDevice>(new OpenclMinerDevice(*this, spec));
    std::string err;
    if (!dev->init(err))
    {
      logger(ERROR) << "Failed to init GPU device " << spec.deviceIndex << ": " << err;
      return false;
    }
    dev->setGlobalThreadBase(threadBase);
    threadBase += GpuMinerConfig::kThreadsPerGpu;
    dev->startWorkers();
    m_devices.push_back(std::move(dev));
  }

  m_stop = false;
  logger(INFO) << "GPU mining started on " << devices.size() << " device(s), "
               << m_total_host_threads << " host threads";
  return true;
}

bool GpuMiner::stop()
{
  send_stop_signal();
  std::lock_guard<std::mutex> lk(m_threads_lock);
  for (auto& dev : m_devices)
    dev->stopWorkers();
  m_devices.clear();
  logger(INFO) << "GPU mining stopped";
  return true;
}

bool GpuMiner::is_mining() const
{
  return !m_stop.load();
}

void GpuMiner::send_stop_signal()
{
  m_stop = true;
}

void GpuMiner::on_synchronized()
{
  if (m_do_mining)
    start(m_mine_address, m_config.devices);
}

bool GpuMiner::on_idle()
{
  m_update_block_template_interval.call([&]() {
    if (is_mining())
      request_block_template();
    return true;
  });

  m_update_merge_hr_interval.call([&]() {
    merge_hr();
    return true;
  });
  return true;
}

void GpuMiner::pause()
{
  ++m_pausers_count;
}

void GpuMiner::resume()
{
  --m_pausers_count;
  if (m_pausers_count < 0)
    m_pausers_count = 0;
}

void GpuMiner::do_print_hashrate(bool do_hr)
{
  m_do_print_hashrate = do_hr;
}

uint64_t GpuMiner::get_speed()
{
  return is_mining() ? m_current_hash_rate.load() : 0;
}

void GpuMiner::add_hashes(uint64_t n)
{
  m_hashes += n;
}

void GpuMiner::merge_hr()
{
  if (m_last_hr_merge_time && is_mining())
  {
    m_current_hash_rate =
        m_hashes * 1000 / (millisecondsSinceEpoch() - m_last_hr_merge_time + 1);
    std::lock_guard<std::mutex> lk(m_last_hash_rates_lock);
    m_last_hash_rates.push_back(m_current_hash_rate);
    if (m_last_hash_rates.size() > 19)
      m_last_hash_rates.pop_front();

    if (m_do_print_hashrate)
    {
      const uint64_t total =
          std::accumulate(m_last_hash_rates.begin(), m_last_hash_rates.end(), uint64_t(0));
      const float hr =
          static_cast<float>(total) / static_cast<float>(m_last_hash_rates.size());
      std::cout << "gpu hashrate: " << std::setprecision(4) << std::fixed << hr << std::endl;
    }
  }
  m_last_hr_merge_time = millisecondsSinceEpoch();
  m_hashes = 0;
}

bool GpuMiner::worker_wait_template(Block& outBlock, difficulty_type& outDiff, uint32_t& outTemplateVer)
{
  if (m_stop)
    return false;

  const uint32_t ver = m_template_no.load();
  if (ver == 0)
    return false;

  std::lock_guard<std::mutex> lk(m_template_lock);
  outBlock = m_template;
  outDiff = m_diffic;
  outTemplateVer = ver;
  return true;
}

void GpuMiner::worker_report_found(const Block& block)
{
  logger(INFO, GREEN) << "GPU found block for difficulty: " << m_diffic;
  Block b = block;
  if (!m_handler.handle_block_found(b))
    logger(WARNING) << "GPU found block but handle_block_found failed";
}

} // namespace cn
