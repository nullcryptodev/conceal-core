// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "WalletManager.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Common/Base58.h"
#include "INode.h"

#include <fstream>
#include <chrono>
#include <thread>
#include <ctime>
#include <boost/filesystem.hpp>

using namespace cn;

namespace BoltRPC
{

  // ─── Constructor / Destructor ──────────────────────────────────────────────

  WalletManager::WalletManager(cn::INode &node,
                               const cn::Currency &currency,
                               const std::string &dataDir,
                               const std::string &daemonHost,
                               uint16_t daemonPort)
      : m_node(node),
        m_currency(currency),
        m_dataDir(dataDir),
        m_daemonHost(daemonHost),
        m_daemonPort(daemonPort),
        m_stateManager(new StateManager(dataDir))
  {
  }

  WalletManager::~WalletManager()
  {
    stopSync();
    if (!m_locked.load())
      saveState();
  }

  // ─── Key Management ────────────────────────────────────────────────────────

  bool WalletManager::generateNewWallet(const std::string &password)
  {
    if (hasExistingWallet())
      return false;

    AccountBase account;
    account.generate();

    WalletKeys keys;
    const AccountKeys &ak = account.getAccountKeys();
    keys.viewSecretKey = ak.viewSecretKey;
    keys.spendSecretKey = ak.spendSecretKey;
    keys.spendPublicKey = ak.address.spendPublicKey;
    keys.address = m_currency.accountAddressAsString(ak.address);

    m_activeWallet = keys.address;

    if (!encryptKeys(keys, password))
      return false;

    m_keys = keys;
    m_locked.store(false);
    m_state = WalletState();
    saveState();

    m_syncManager.reset(new SyncManager(
        m_node,
        m_keys.viewSecretKey,
        m_keys.spendPublicKey,
        m_dataDir,
        m_rpcCallback));

    return true;
  }

  bool WalletManager::importFromKeys(const std::string &viewKeyHex,
                                     const std::string &spendKeyHex,
                                     const std::string &password)
  {
    if (hasExistingWallet())
      return false;

    WalletKeys keys;
    if (!common::podFromHex(viewKeyHex, keys.viewSecretKey))
      return false;
    if (!common::podFromHex(spendKeyHex, keys.spendSecretKey))
      return false;

    if (!crypto::secret_key_to_public_key(keys.spendSecretKey, keys.spendPublicKey))
      return false;

    AccountPublicAddress addr;
    addr.spendPublicKey = keys.spendPublicKey;
    if (!crypto::secret_key_to_public_key(keys.viewSecretKey, addr.viewPublicKey))
      return false;

    keys.address = m_currency.accountAddressAsString(addr);
    m_activeWallet = keys.address;

    if (!encryptKeys(keys, password))
      return false;

    m_keys = keys;
    m_locked.store(false);
    m_state = WalletState();
    saveState();

    m_syncManager.reset(new SyncManager(
        m_node,
        m_keys.viewSecretKey,
        m_keys.spendPublicKey,
        m_dataDir,
        m_rpcCallback));

    return true;
  }

  bool WalletManager::importFromMnemonic(const std::string &mnemonic,
                                         const std::string &password)
  {
    return false;
  }

  bool WalletManager::unlock(const std::string &password)
  {
    if (!hasExistingWallet())
      return false;

    WalletKeys keys;
    if (!decryptKeys(password, keys))
      return false;

    m_keys = keys;

    if (m_activeWallet.empty())
      m_activeWallet = keys.address;

    m_locked.store(false);

    loadState();

    m_syncManager.reset(new SyncManager(
        m_node,
        m_keys.viewSecretKey,
        m_keys.spendPublicKey,
        m_dataDir,
        m_rpcCallback));

    return true;
  }

  void WalletManager::lock()
  {
    stopSync();
    saveState();
    m_keys = WalletKeys();
    m_state = WalletState();
    m_syncManager.reset();
    m_locked.store(true);
  }

  // ─── Sync ──────────────────────────────────────────────────────────────────

  void WalletManager::startSync(StatusCallback onStatus)
  {
    if (m_locked.load() || !m_syncManager)
      return;

    m_onStatus = std::move(onStatus);

    m_syncManager->start(
        [this](const SyncProgress &progress)
        {
          onSyncProgress(progress);
        },
        [this](const std::vector<OutputInfo> &newOutputs,
               const std::vector<crypto::KeyImage> &spentKeyImages)
        {
          onNewOutputs(newOutputs, spentKeyImages);
        });
  }

  void WalletManager::stopSync()
  {
    if (m_syncManager)
      m_syncManager->stop();
  }

  void WalletManager::syncNow()
  {
    if (m_syncManager)
      m_syncManager->syncNow();
  }

  // ─── Wallet Queries ────────────────────────────────────────────────────────

  WalletStatus WalletManager::getStatus() const
  {
    WalletStatus status;
    status.locked = m_locked.load();

    std::lock_guard<std::mutex> lock(m_stateMutex);
    status.blockHeight = m_state.lastHeight;
    status.balance = m_state.balance;
    status.unlockedBalance = m_state.unlockedBalance;
    status.outputCount = static_cast<uint32_t>(m_state.ownedOutputs.size());
    status.transactionCount = static_cast<uint32_t>(m_state.transactions.size());
    status.synced = (m_syncManager != nullptr);

    return status;
  }

  std::vector<OutputInfo> WalletManager::getOutputs(bool unspentOnly) const
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (!unspentOnly)
      return m_state.ownedOutputs;

    std::vector<OutputInfo> unspent;
    for (const auto &out : m_state.ownedOutputs)
      if (!out.spent)
        unspent.push_back(out);
    return unspent;
  }

  std::vector<TransactionRecord> WalletManager::getTransactions(uint32_t offset,
                                                                uint32_t limit) const
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    std::vector<TransactionRecord> result;
    if (offset >= m_state.transactions.size())
      return result;

    uint32_t end = std::min(offset + limit, static_cast<uint32_t>(m_state.transactions.size()));
    result.assign(m_state.transactions.begin() + offset,
                  m_state.transactions.begin() + end);
    return result;
  }

  uint64_t WalletManager::getBalance() const
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state.balance;
  }

  uint64_t WalletManager::getUnlockedBalance() const
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state.unlockedBalance;
  }

  std::string WalletManager::getAddress() const
  {
    return m_keys.address;
  }

  // ─── Transactions ──────────────────────────────────────────────────────────

  TransferResult WalletManager::sendTransfer(const TransferRequest &req)
  {
    TransferResult result;

    if (m_locked.load())
    {
      result.errorMessage = "Wallet is locked";
      return result;
    }

    // TODO: Implement full transaction building and relaying
    // For now, record the outgoing transaction when the full builder is ready
    result.errorMessage = "Transaction builder not yet implemented";
    return result;
  }

  TransferResult WalletManager::sendDeposit(const DepositRequest &req)
  {
    TransferResult result;
    result.errorMessage = "Deposit builder not yet implemented";
    return result;
  }

  TransferResult WalletManager::sendWithdrawal(const WithdrawalRequest &req)
  {
    TransferResult result;
    result.errorMessage = "Withdrawal builder not yet implemented";
    return result;
  }

  // ─── Export ────────────────────────────────────────────────────────────────

  std::string WalletManager::exportKeys() const
  {
    if (m_locked.load())
      return "";

    return common::podToHex(m_keys.spendSecretKey) + ":" +
           common::podToHex(m_keys.viewSecretKey);
  }

  std::string WalletManager::exportMnemonic() const
  {
    return "";
  }

  std::string WalletManager::exportState() const
  {
    return m_stateManager->filePath();
  }

  // ─── State File ────────────────────────────────────────────────────────────

  bool WalletManager::hasExistingWallet() const
  {
    std::ifstream file(keysFilePath());
    return file.good();
  }

  // ─── Internal Helpers ──────────────────────────────────────────────────────

  void WalletManager::onSyncProgress(const SyncProgress &progress)
  {
    if (m_onStatus)
    {
      WalletStatus status = getStatus();
      status.syncProgress = progress;
      m_onStatus(status);
    }
  }

  void WalletManager::onNewOutputs(const std::vector<OutputInfo> &newOutputs,
                                   const std::vector<crypto::KeyImage> &spentKeyImages)
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    uint64_t addedBalance = 0;
    for (const auto &out : newOutputs)
    {
      bool exists = false;
      for (const auto &existing : m_state.ownedOutputs)
      {
        if (existing.txHash == out.txHash && existing.outputIndex == out.outputIndex)
        {
          exists = true;
          break;
        }
      }

      if (!exists)
      {
        m_state.ownedOutputs.push_back(out);
        if (!out.spent)
          addedBalance += out.amount;

        // Record incoming transaction
        TransactionRecord tx;
        tx.txHash = out.txHash;
        tx.blockHeight = out.blockHeight;
        tx.timestamp = 0;
        tx.totalReceived = out.amount;
        tx.totalSent = 0;
        tx.type = out.isDeposit ? TransactionRecord::DEPOSIT : TransactionRecord::INCOMING;
        tx.confirmed = true;
        m_state.transactions.push_back(tx);
      }
    }

    for (const auto &ki : spentKeyImages)
    {
      m_state.spentKeyImages.push_back(ki);
    }

    m_state.balance += addedBalance;

    uint32_t currentHeight = (m_syncManager) ? m_syncManager->lastScannedHeight() : 0;
    uint64_t unlocked = 0;
    for (const auto &out : m_state.ownedOutputs)
    {
      if (!out.spent &&
          (currentHeight - out.blockHeight) >= m_currency.minedMoneyUnlockWindow())
      {
        unlocked += out.amount;
      }
    }
    m_state.unlockedBalance = unlocked;

    m_stateManager->commit(m_state);
  }

  void WalletManager::updateStatus()
  {
    if (m_onStatus)
      m_onStatus(getStatus());
  }

  void WalletManager::loadState()
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_stateManager->load(m_state))
      m_state = WalletState();
  }

  void WalletManager::saveState()
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stateManager->save(m_state);
  }

  std::string WalletManager::keysFilePath() const
  {
    if (!m_activeWallet.empty())
      return m_dataDir + "/" + m_activeWallet + ".keys";
    return m_dataDir + "/wallet.keys";
  }

  std::string WalletManager::stateFilePath() const
  {
    if (!m_activeWallet.empty())
      return m_dataDir + "/" + m_activeWallet + "_state.bin";
    return m_stateManager->filePath();
  }

  void WalletManager::setActiveWallet(const std::string &walletName)
  {
    lock();
    m_activeWallet = walletName;
  }

  std::vector<std::string> WalletManager::listWalletFiles() const
  {
    std::vector<std::string> wallets;

    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(m_dataDir, ec))
      return wallets;

    for (boost::filesystem::directory_iterator it(m_dataDir, ec), end; it != end; it.increment(ec))
    {
      if (ec)
        break;
      if (!boost::filesystem::is_regular_file(it->status()))
        continue;

      const std::string name = it->path().filename().string();
      if (name.size() > 5 && name.substr(name.size() - 5) == ".keys")
        wallets.push_back(name.substr(0, name.size() - 5));
    }

    return wallets;
  }

  // ─── Encryption ────────────────────────────────────────────────────────────

  bool WalletManager::encryptKeys(const WalletKeys &keys, const std::string &password)
  {
    std::ofstream file(keysFilePath(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return false;

    constexpr uint32_t MAGIC = 0x43434B46;
    file.write(reinterpret_cast<const char *>(&MAGIC), sizeof(MAGIC));

    constexpr uint32_t VERSION = 1;
    file.write(reinterpret_cast<const char *>(&VERSION), sizeof(VERSION));

    crypto::Hash pwHash;
    crypto::cn_fast_hash(password.data(), password.size(), pwHash);

    auto xorWrite = [&](const void *data, size_t size)
    {
      std::vector<uint8_t> buf(static_cast<const uint8_t *>(data),
                               static_cast<const uint8_t *>(data) + size);
      for (size_t i = 0; i < size; ++i)
        buf[i] ^= pwHash.data[i % sizeof(pwHash.data)];
      file.write(reinterpret_cast<const char *>(buf.data()), size);
    };

    xorWrite(&keys.viewSecretKey, sizeof(keys.viewSecretKey));
    xorWrite(&keys.spendSecretKey, sizeof(keys.spendSecretKey));

    return file.good();
  }

  bool WalletManager::decryptKeys(const std::string &password, WalletKeys &keys) const
  {
    std::ifstream file(keysFilePath(), std::ios::binary);
    if (!file.is_open())
      return false;

    uint32_t magic = 0, version = 0;
    file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char *>(&version), sizeof(version));

    if (magic != 0x43434B46 || version != 1)
      return false;

    crypto::Hash pwHash;
    crypto::cn_fast_hash(password.data(), password.size(), pwHash);

    auto xorRead = [&](void *data, size_t size)
    {
      std::vector<uint8_t> buf(size);
      file.read(reinterpret_cast<char *>(buf.data()), size);
      if (!file.good())
        return false;
      for (size_t i = 0; i < size; ++i)
        buf[i] ^= pwHash.data[i % sizeof(pwHash.data)];
      std::memcpy(data, buf.data(), size);
      return true;
    };

    if (!xorRead(&keys.viewSecretKey, sizeof(keys.viewSecretKey)))
      return false;
    if (!xorRead(&keys.spendSecretKey, sizeof(keys.spendSecretKey)))
      return false;

    if (!crypto::secret_key_to_public_key(keys.spendSecretKey, keys.spendPublicKey))
      return false;

    AccountPublicAddress addr;
    addr.spendPublicKey = keys.spendPublicKey;
    if (!crypto::secret_key_to_public_key(keys.viewSecretKey, addr.viewPublicKey))
      return false;
    keys.address = m_currency.accountAddressAsString(addr);

    return true;
  }

} // namespace BoltRPC