// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "StateManager.h"

#include <cstring>
#include <fstream>

namespace BoltRPC
{

  namespace
  {

    constexpr uint32_t STATE_FILE_MAGIC = 0x43435354; // "CCST" = Conceal State
    constexpr uint32_t STATE_FILE_VERSION = 3; // v3: keyImage added to OutputInfo

    // ── Binary serialization helpers (no external dependency) ──────────────

    template <typename T>
    void writePod(std::ofstream &file, const T &value)
    {
      file.write(reinterpret_cast<const char *>(&value), sizeof(T));
    }

    template <typename T>
    bool readPod(std::ifstream &file, T &value)
    {
      file.read(reinterpret_cast<char *>(&value), sizeof(T));
      return file.good();
    }

    void writeVector(std::ofstream &file, const std::vector<uint8_t> &data)
    {
      uint32_t size = static_cast<uint32_t>(data.size());
      writePod(file, size);
      if (size > 0)
        file.write(reinterpret_cast<const char *>(data.data()), size);
    }

    bool readVector(std::ifstream &file, std::vector<uint8_t> &data)
    {
      uint32_t size = 0;
      if (!readPod(file, size))
        return false;
      if (size > 100 * 1024 * 1024)
        return false; // 100 MB sanity
      data.resize(size);
      if (size > 0)
        file.read(reinterpret_cast<char *>(data.data()), size);
      return file.good();
    }

    void writeOutputInfo(std::ofstream &file, const OutputInfo &out)
    {
      writePod(file, out.blockHeight);
      file.write(reinterpret_cast<const char *>(&out.txHash), sizeof(out.txHash));
      writePod(file, out.amount);
      writePod(file, out.outputIndex);
      writePod(file, out.globalOutputIndex);
      writePod(file, static_cast<uint8_t>(out.hasGlobalOutputIndex ? 1 : 0));
      file.write(reinterpret_cast<const char *>(&out.outputKey), sizeof(out.outputKey));
      file.write(reinterpret_cast<const char *>(&out.txPublicKey), sizeof(out.txPublicKey));
      file.write(reinterpret_cast<const char *>(&out.keyImage), sizeof(out.keyImage)); // v3
      writePod(file, static_cast<uint8_t>(out.spent ? 1 : 0));
      writePod(file, static_cast<uint8_t>(out.isDeposit ? 1 : 0));
      writePod(file, out.term);
    }

    bool readOutputInfo(std::ifstream &file, OutputInfo &out)
    {
      if (!readPod(file, out.blockHeight))
        return false;
      file.read(reinterpret_cast<char *>(&out.txHash), sizeof(out.txHash));
      if (!file.good())
        return false;
      if (!readPod(file, out.amount))
        return false;
      if (!readPod(file, out.outputIndex))
        return false;
      if (!readPod(file, out.globalOutputIndex))
        return false;
      uint8_t hasGlobal = 0;
      if (!readPod(file, hasGlobal))
        return false;
      out.hasGlobalOutputIndex = (hasGlobal != 0);
      file.read(reinterpret_cast<char *>(&out.outputKey), sizeof(out.outputKey));
      if (!file.good())
        return false;
      file.read(reinterpret_cast<char *>(&out.txPublicKey), sizeof(out.txPublicKey));
      if (!file.good())
        return false;
      file.read(reinterpret_cast<char *>(&out.keyImage), sizeof(out.keyImage)); // v3
      if (!file.good())
        return false;
      uint8_t spent = 0, isDep = 0;
      if (!readPod(file, spent))
        return false;
      if (!readPod(file, isDep))
        return false;
      if (!readPod(file, out.term))
        return false;
      out.spent = (spent != 0);
      out.isDeposit = (isDep != 0);
      return true;
    }

    void writeTxRecord(std::ofstream &file, const TransactionRecord &tx)
    {
      file.write(reinterpret_cast<const char *>(&tx.txHash), sizeof(tx.txHash));
      writePod(file, tx.blockHeight);
      writePod(file, tx.timestamp);
      writePod(file, tx.fee);
      writePod(file, tx.totalSent);
      writePod(file, tx.totalReceived);
      writePod(file, static_cast<uint8_t>(tx.type));
      writePod(file, static_cast<uint8_t>(tx.confirmed ? 1 : 0));

      // paymentId
      uint8_t pidLen = static_cast<uint8_t>(std::min(tx.paymentId.size(), size_t(255)));
      writePod(file, pidLen);
      if (pidLen > 0)
        file.write(tx.paymentId.data(), pidLen);

      // extraKeys
      uint16_t ekCount = static_cast<uint16_t>(tx.extraKeys.size());
      writePod(file, ekCount);
      for (const auto &pk : tx.extraKeys)
        file.write(reinterpret_cast<const char *>(&pk), sizeof(pk));
    }

    bool readTxRecord(std::ifstream &file, TransactionRecord &tx)
    {
      file.read(reinterpret_cast<char *>(&tx.txHash), sizeof(tx.txHash));
      if (!file.good())
        return false;
      if (!readPod(file, tx.blockHeight))
        return false;
      if (!readPod(file, tx.timestamp))
        return false;
      if (!readPod(file, tx.fee))
        return false;
      if (!readPod(file, tx.totalSent))
        return false;
      if (!readPod(file, tx.totalReceived))
        return false;
      uint8_t type = 0, confirmed = 0;
      if (!readPod(file, type))
        return false;
      if (!readPod(file, confirmed))
        return false;
      tx.type = static_cast<TransactionRecord::Type>(type);
      tx.confirmed = (confirmed != 0);

      uint8_t pidLen = 0;
      if (!readPod(file, pidLen))
        return false;
      if (pidLen > 0)
      {
        tx.paymentId.resize(pidLen);
        file.read(&tx.paymentId[0], pidLen);
        if (!file.good())
          return false;
      }
      else
      {
        tx.paymentId.clear();
      }

      uint16_t ekCount = 0;
      if (!readPod(file, ekCount))
        return false;
      tx.extraKeys.resize(ekCount);
      for (uint16_t i = 0; i < ekCount; ++i)
      {
        file.read(reinterpret_cast<char *>(&tx.extraKeys[i]), sizeof(crypto::PublicKey));
        if (!file.good())
          return false;
      }

      return true;
    }

  } // anonymous namespace

  // ─── Constructor / Destructor ──────────────────────────────────────────────

  StateManager::StateManager(const std::string &dataDir)
      : m_filePath(dataDir + "/wallet_state.bin")
  {
  }

  StateManager::StateManager(const std::string &filePath, bool pathIsFullFile)
      : m_filePath(pathIsFullFile ? filePath : filePath + "/wallet_state.bin")
  {
  }

  StateManager::~StateManager()
  {
  }

  // ─── File Path ─────────────────────────────────────────────────────────────

  std::string StateManager::filePath() const
  {
    return m_filePath;
  }

  bool StateManager::exists() const
  {
    std::ifstream file(filePath());
    return file.good();
  }

  size_t StateManager::fileSize() const
  {
    std::ifstream file(filePath(), std::ios::binary | std::ios::ate);
    if (!file)
      return 0;
    return static_cast<size_t>(file.tellg());
  }

  // ─── Load ──────────────────────────────────────────────────────────────────

  bool StateManager::load(WalletState &state)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ifstream file(filePath(), std::ios::binary);
    if (!file.is_open())
      return false;

    // Magic + version
    uint32_t magic = 0, version = 0;
    if (!readPod(file, magic) || magic != STATE_FILE_MAGIC)
      return false;
    if (!readPod(file, version) || version != STATE_FILE_VERSION)
      return false;

    // Height
    if (!readPod(file, state.lastHeight))
      return false;

    // Balance
    if (!readPod(file, state.balance))
      return false;
    if (!readPod(file, state.unlockedBalance))
      return false;

    // Owned outputs
    uint32_t outputCount = 0;
    if (!readPod(file, outputCount) || outputCount > 10000000)
      return false;
    state.ownedOutputs.resize(outputCount);
    for (uint32_t i = 0; i < outputCount; ++i)
    {
      if (!readOutputInfo(file, state.ownedOutputs[i]))
        return false;
    }

    // Spent key images
    uint32_t kiCount = 0;
    if (!readPod(file, kiCount) || kiCount > 10000000)
      return false;
    state.spentKeyImages.resize(kiCount);
    for (uint32_t i = 0; i < kiCount; ++i)
    {
      file.read(reinterpret_cast<char *>(&state.spentKeyImages[i]), sizeof(crypto::KeyImage));
      if (!file.good())
        return false;
    }

    // Transactions
    uint32_t txCount = 0;
    if (!readPod(file, txCount) || txCount > 10000000)
      return false;
    state.transactions.resize(txCount);
    for (uint32_t i = 0; i < txCount; ++i)
    {
      if (!readTxRecord(file, state.transactions[i]))
        return false;
    }

    return true;
  }

  // ─── Save ──────────────────────────────────────────────────────────────────

  bool StateManager::save(const WalletState &state)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Write to temp file first, then rename (atomic on most OS)
    std::string tmpPath = filePath() + ".tmp";
    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return false;

    // Magic + version
    writePod(file, STATE_FILE_MAGIC);
    writePod(file, STATE_FILE_VERSION);

    // Height
    writePod(file, state.lastHeight);

    // Balance
    writePod(file, state.balance);
    writePod(file, state.unlockedBalance);

    // Owned outputs
    uint32_t outputCount = static_cast<uint32_t>(state.ownedOutputs.size());
    writePod(file, outputCount);
    for (const auto &out : state.ownedOutputs)
      writeOutputInfo(file, out);

    // Spent key images
    uint32_t kiCount = static_cast<uint32_t>(state.spentKeyImages.size());
    writePod(file, kiCount);
    for (const auto &ki : state.spentKeyImages)
      file.write(reinterpret_cast<const char *>(&ki), sizeof(ki));

    // Transactions
    uint32_t txCount = static_cast<uint32_t>(state.transactions.size());
    writePod(file, txCount);
    for (const auto &tx : state.transactions)
      writeTxRecord(file, tx);

    file.close();
    if (!file.good())
      return false;

    // Atomic rename
    std::rename(tmpPath.c_str(), filePath().c_str());
    return true;
  }

  // ─── Incremental Updates ───────────────────────────────────────────────────

  void StateManager::addOutput(const OutputInfo &output)
  {
    WalletState state;
    if (!load(state))
      state = WalletState();
    state.ownedOutputs.push_back(output);
    save(state);
  }

  void StateManager::markSpent(const crypto::KeyImage &keyImage)
  {
    WalletState state;
    if (!load(state))
      return;
    state.spentKeyImages.push_back(keyImage);
    for (auto &out : state.ownedOutputs)
    {
      // Match would need key image derivation — handled by caller
    }
    save(state);
  }

  void StateManager::addTransaction(const TransactionRecord &tx)
  {
    WalletState state;
    if (!load(state))
      state = WalletState();
    state.transactions.push_back(tx);
    save(state);
  }

  void StateManager::setHeight(uint32_t height)
  {
    WalletState state;
    if (!load(state))
      state = WalletState();
    state.lastHeight = height;
    save(state);
  }

  void StateManager::setBalance(uint64_t balance, uint64_t unlocked)
  {
    WalletState state;
    if (!load(state))
      state = WalletState();
    state.balance = balance;
    state.unlockedBalance = unlocked;
    save(state);
  }

  void StateManager::commit(const WalletState &state)
  {
    save(state);
  }

} // namespace BoltRPC