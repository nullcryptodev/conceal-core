// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "MultiWalletManager.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <mdbx.h>

#include "BoltSync/BoltSync.h"
#include "Common/JsonValue.h"
#include "Common/PathTools.h"
#include "Common/StringTools.h"
#include "Common/Util.h"
#include "CryptoNoteCore/Currency.h"
#include "crypto/crypto.h"
#include "NodeClient/NodeClient.h"
#include "SyncManager.h"
#include "version.h"

namespace BoltRPC
{
  // Internal Constants & MDBX Helpers
  namespace
  {
    // wallets.db index database names
    constexpr const char *DBI_WALLETS = "wallets";

    // State file format (mirrors StateManager's format exactly)
    constexpr uint32_t STATE_FILE_MAGIC = 0x43435354; // "CCST"
    constexpr uint32_t STATE_FILE_VERSION = 1;

    // MDBX_val helpers (matching MDBXBlockchainStorage)

    MDBX_val to_val(const void *data, size_t len)
    {
      MDBX_val v;
      v.iov_base = const_cast<void *>(data);
      v.iov_len = len;
      return v;
    }

    MDBX_val to_val(const std::string &s)
    {
      return to_val(s.data(), s.size());
    }

    // Binary serialization (same format as StateManager)
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

    void writeOutputInfo(std::ofstream &file, const OutputInfo &out)
    {
      writePod(file, out.blockHeight);
      file.write(reinterpret_cast<const char *>(&out.txHash), sizeof(out.txHash));
      writePod(file, out.amount);
      writePod(file, out.outputIndex);
      file.write(reinterpret_cast<const char *>(&out.outputKey), sizeof(out.outputKey));
      file.write(reinterpret_cast<const char *>(&out.txPublicKey), sizeof(out.txPublicKey));
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
      file.read(reinterpret_cast<char *>(&out.outputKey), sizeof(out.outputKey));
      if (!file.good())
        return false;
      file.read(reinterpret_cast<char *>(&out.txPublicKey), sizeof(out.txPublicKey));
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

      uint8_t pidLen = static_cast<uint8_t>(std::min(tx.paymentId.size(), size_t(255)));
      writePod(file, pidLen);
      if (pidLen > 0)
        file.write(tx.paymentId.data(), pidLen);

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
        tx.paymentId.clear();

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

    // Save full wallet state to a .state file
    // If password is non-empty, XOR-encrypts keys with password hash.
    // If password is empty, writes keyDataSize=0 (state-only save).

    bool saveWalletStateToFile(const std::string &filePath,
                               const WalletKeys &keys,
                               const WalletState &state,
                               const std::string &password)
    {
      std::string tmpPath = filePath + ".tmp";
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

      // Keys: XOR-encrypted with password hash
      if (!password.empty())
      {
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

        // Write key material size (unencrypted)
        uint32_t keyDataSize = sizeof(crypto::SecretKey) * 2;
        writePod(file, keyDataSize);

        // Write XOR-encrypted keys
        xorWrite(&keys.viewSecretKey, sizeof(crypto::SecretKey));
        xorWrite(&keys.spendSecretKey, sizeof(crypto::SecretKey));
      }
      else
      {
        // No password provided — state-only save, keys already on disk
        uint32_t keyDataSize = 0;
        writePod(file, keyDataSize);
      }

      file.close();
      if (!file.good())
        return false;

      std::rename(tmpPath.c_str(), filePath.c_str());
      return true;
    }

    // Load full wallet state from a .state file
    bool loadWalletStateFromFile(const std::string &filePath,
                                 const std::string &password,
                                 WalletKeys &keys,
                                 WalletState &state)
    {
      std::ifstream file(filePath, std::ios::binary);
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

      // Keys: XOR-decrypt with password hash
      uint32_t keyDataSize = 0;
      if (!readPod(file, keyDataSize))
        return false;

      if (keyDataSize == sizeof(crypto::SecretKey) * 2)
      {
        crypto::Hash pwHash;
        crypto::cn_fast_hash(password.data(), password.size(), pwHash);

        auto xorRead = [&](void *data, size_t size) -> bool
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

        if (!xorRead(&keys.viewSecretKey, sizeof(crypto::SecretKey)))
          return false;
        if (!xorRead(&keys.spendSecretKey, sizeof(crypto::SecretKey)))
          return false;
      }
      else if (keyDataSize != 0)
      {
        return false;
      }

      // Derive spend public key
      crypto::PublicKey spendPub;
      if (!crypto::secret_key_to_public_key(keys.spendSecretKey, spendPub))
        return false;
      keys.spendPublicKey = spendPub;

      return true;
    }

    // Derive address from keys
    std::string deriveAddress(const WalletKeys &keys, const cn::Currency &currency)
    {
      cn::AccountPublicAddress addr;
      addr.spendPublicKey = keys.spendPublicKey;
      crypto::PublicKey viewPub;
      if (!crypto::secret_key_to_public_key(keys.viewSecretKey, viewPub))
        return "";
      addr.viewPublicKey = viewPub;
      return currency.accountAddressAsString(addr);
    }

    // Error response helper
    common::JsonValue makeErrorResponse(const std::string &message)
    {
      common::JsonValue response(common::JsonValue::OBJECT);
      common::JsonValue &error = response.insert("error", common::JsonValue(common::JsonValue::OBJECT));
      error.insert("code", common::JsonValue(static_cast<int64_t>(-1)));
      error.insert("message", common::JsonValue(message));
      return response;
    }

  } // anonymous namespace

  // Constructor / Destructor
  MultiWalletManager::MultiWalletManager(const std::string &walletsDir,
                                         cn::INode *node,
                                         const cn::Currency &currency)
      : m_walletsDir(walletsDir),
        m_node(node),
        m_currency(currency),
        m_locked(true),
        m_env(nullptr),
        m_dbiWallets(0)
  {
    openIndex();
  }

  MultiWalletManager::~MultiWalletManager()
  {
    if (!m_activeWalletId.empty())
    {
      saveActiveWallet();
      stopActiveSync();
    }

    if (m_env)
    {
      mdbx_env_close(m_env);
      m_env = nullptr;
    }
  }

  // Index Management (wallets.db)
  void MultiWalletManager::openIndex()
  {
    std::string indexPath = m_walletsDir + "/wallets.db";

    int rc = mdbx_env_create(&m_env);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("MultiWalletManager: mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

    mdbx_env_set_maxdbs(m_env, 1);

    const MDBX_env_flags_t openFlags = MDBX_NOSUBDIR;
    rc = mdbx_env_open(m_env, indexPath.c_str(), openFlags, 0664);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("MultiWalletManager: mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));

    // Create/verify the wallets database
    MDBX_txn *txn;
    rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("MultiWalletManager: txn_begin failed: " + std::string(mdbx_strerror(rc)));

    rc = mdbx_dbi_open(txn, DBI_WALLETS, MDBX_CREATE, &m_dbiWallets);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("MultiWalletManager: dbi_open wallets failed: " + std::string(mdbx_strerror(rc)));
    }

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("MultiWalletManager: txn_commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  void MultiWalletManager::saveIndexEntry(const WalletEntry &entry)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("saveIndexEntry: txn_begin failed: " + std::string(mdbx_strerror(rc)));

    common::JsonValue json(common::JsonValue::OBJECT);
    json.insert("id", common::JsonValue(entry.id));
    json.insert("address", common::JsonValue(entry.address));
    json.insert("stateFilePath", common::JsonValue(entry.stateFilePath));
    json.insert("createdAt", common::JsonValue(static_cast<int64_t>(entry.createdAt)));
    json.insert("lastUsed", common::JsonValue(static_cast<int64_t>(entry.lastUsed)));

    std::string value = json.toString();
    MDBX_val mkey = to_val(entry.id);
    MDBX_val mval = to_val(value);

    rc = mdbx_put(txn, m_dbiWallets, &mkey, &mval, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("saveIndexEntry: put failed: " + std::string(mdbx_strerror(rc)));
    }

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("saveIndexEntry: commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  void MultiWalletManager::removeIndexEntry(const std::string &walletId)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      return;

    MDBX_val mkey = to_val(walletId);
    mdbx_del(txn, m_dbiWallets, &mkey, nullptr);

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
      mdbx_txn_abort(txn);
  }

  std::vector<WalletEntry> MultiWalletManager::readAllIndexEntries() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<WalletEntry> entries;

    MDBX_txn *txn;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS)
      return entries;

    MDBX_cursor *cursor;
    rc = mdbx_cursor_open(txn, m_dbiWallets, &cursor);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      return entries;
    }

    MDBX_val mkey, mval;
    rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_FIRST);
    while (rc == MDBX_SUCCESS)
    {
      std::string valueStr(static_cast<const char *>(mval.iov_base), mval.iov_len);
      common::JsonValue json = common::JsonValue::fromString(valueStr);

      WalletEntry entry;
      entry.id = json("id").getString();
      entry.address = json("address").getString();
      entry.stateFilePath = json("stateFilePath").getString();
      entry.createdAt = static_cast<uint64_t>(json("createdAt").getInteger());
      entry.lastUsed = static_cast<uint64_t>(json("lastUsed").getInteger());
      entries.push_back(std::move(entry));

      rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_NEXT);
    }

    mdbx_cursor_close(cursor);
    mdbx_txn_abort(txn);
    return entries;
  }

  WalletEntry MultiWalletManager::getIndexEntry(const std::string &walletId) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("Wallet not found: " + walletId);

    MDBX_val mkey = to_val(walletId);
    MDBX_val mval;
    rc = mdbx_get(txn, m_dbiWallets, &mkey, &mval);

    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("Wallet not found: " + walletId);
    }

    std::string valueStr(static_cast<const char *>(mval.iov_base), mval.iov_len);
    common::JsonValue json = common::JsonValue::fromString(valueStr);

    WalletEntry entry;
    entry.id = json("id").getString();
    entry.address = json("address").getString();
    entry.stateFilePath = json("stateFilePath").getString();
    entry.createdAt = static_cast<uint64_t>(json("createdAt").getInteger());
    entry.lastUsed = static_cast<uint64_t>(json("lastUsed").getInteger());

    mdbx_txn_abort(txn);
    return entry;
  }

  std::string MultiWalletManager::stateFilePath(const std::string &walletId) const
  {
    return m_walletsDir + "/" + walletId + ".state";
  }

  // Wallet Lifecycle
  common::JsonValue MultiWalletManager::createWallet(const std::string &walletId,
                                                     const std::string &password)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (walletExists(walletId))
      return makeErrorResponse("Wallet already exists: " + walletId);

    // Generate new keys
    crypto::SecretKey viewKey, spendKey;
    crypto::PublicKey spendPub, viewPub;

    crypto::generate_keys(viewPub, viewKey);
    crypto::generate_keys(spendPub, spendKey);

    WalletKeys keys;
    keys.viewSecretKey = viewKey;
    keys.spendSecretKey = spendKey;
    keys.spendPublicKey = spendPub;
    keys.address = deriveAddress(keys, m_currency);

    WalletState state;

    std::string path = stateFilePath(walletId);
    if (!saveWalletStateToFile(path, keys, state, password))
      return makeErrorResponse("Failed to save wallet state file");

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    WalletEntry entry;
    entry.id = walletId;
    entry.address = keys.address;
    entry.stateFilePath = path;
    entry.createdAt = now;
    entry.lastUsed = now;

    saveIndexEntry(entry);

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(walletId));
    response.insert("address", common::JsonValue(keys.address));
    response.insert("state_file", common::JsonValue(path));

    return response;
  }

  common::JsonValue MultiWalletManager::importWallet(const std::string &walletId,
                                                     const std::string &viewKeyHex,
                                                     const std::string &spendKeyHex,
                                                     const std::string &password)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (walletExists(walletId))
      return makeErrorResponse("Wallet already exists: " + walletId);

    crypto::SecretKey viewKey, spendKey;
    if (!common::podFromHex(viewKeyHex, viewKey))
      return makeErrorResponse("Invalid view key hex");
    if (!common::podFromHex(spendKeyHex, spendKey))
      return makeErrorResponse("Invalid spend key hex");

    crypto::PublicKey spendPub, viewPub;
    if (!crypto::secret_key_to_public_key(spendKey, spendPub))
      return makeErrorResponse("Invalid spend key");
    if (!crypto::secret_key_to_public_key(viewKey, viewPub))
      return makeErrorResponse("Invalid view key");

    WalletKeys keys;
    keys.viewSecretKey = viewKey;
    keys.spendSecretKey = spendKey;
    keys.spendPublicKey = spendPub;
    keys.address = deriveAddress(keys, m_currency);

    WalletState state;

    std::string path = stateFilePath(walletId);
    if (!saveWalletStateToFile(path, keys, state, password))
      return makeErrorResponse("Failed to save wallet state file");

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    WalletEntry entry;
    entry.id = walletId;
    entry.address = keys.address;
    entry.stateFilePath = path;
    entry.createdAt = now;
    entry.lastUsed = now;

    saveIndexEntry(entry);

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(walletId));
    response.insert("address", common::JsonValue(keys.address));
    response.insert("state_file", common::JsonValue(path));

    return response;
  }

  common::JsonValue MultiWalletManager::importFromStateFile(const std::string &walletId,
                                                            const std::string &sourceFilePath,
                                                            const std::string &password)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (walletExists(walletId))
      return makeErrorResponse("Wallet already exists: " + walletId);

    WalletKeys keys;
    WalletState state;
    if (!loadWalletStateFromFile(sourceFilePath, password, keys, state))
      return makeErrorResponse("Failed to load source state file. Wrong password or corrupt file.");

    std::string destPath = stateFilePath(walletId);
    std::ifstream src(sourceFilePath, std::ios::binary);
    std::ofstream dst(destPath, std::ios::binary | std::ios::trunc);
    if (!src || !dst)
      return makeErrorResponse("Failed to copy state file");
    dst << src.rdbuf();
    src.close();
    dst.close();

    keys.address = deriveAddress(keys, m_currency);

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    WalletEntry entry;
    entry.id = walletId;
    entry.address = keys.address;
    entry.stateFilePath = destPath;
    entry.createdAt = now;
    entry.lastUsed = now;

    saveIndexEntry(entry);

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(walletId));
    response.insert("address", common::JsonValue(keys.address));
    response.insert("state_file", common::JsonValue(destPath));

    return response;
  }

  common::JsonValue MultiWalletManager::loadWallet(const std::string &walletId,
                                                   const std::string &password)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_activeWalletId.empty())
    {
      saveActiveWallet();
      stopActiveSync();
      m_activeWalletId.clear();
    }

    if (!walletExists(walletId))
      return makeErrorResponse("Wallet not found: " + walletId);

    WalletKeys keys;
    WalletState state;
    std::string path = stateFilePath(walletId);
    if (!loadWalletStateFromFile(path, password, keys, state))
      return makeErrorResponse("Failed to load wallet. Wrong password?");

    m_activeWalletId = walletId;
    m_activeKeys = keys;
    m_activeState = state;
    m_locked.store(false);

    crypto::cn_fast_hash(password.data(), password.size(), m_passwordHash);

    WalletEntry entry = getIndexEntry(walletId);
    entry.lastUsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    saveIndexEntry(entry);

    if (m_node)
      startSyncForActive();

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(walletId));
    response.insert("address", common::JsonValue(keys.address));
    response.insert("balance", common::JsonValue(static_cast<int64_t>(state.balance)));
    response.insert("height", common::JsonValue(static_cast<int64_t>(state.lastHeight)));

    return response;
  }

  common::JsonValue MultiWalletManager::unloadWallet()
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet is currently loaded");

    saveActiveWallet();
    stopActiveSync();

    m_activeWalletId.clear();
    m_activeKeys = WalletKeys();
    m_activeState = WalletState();
    m_passwordHash = crypto::Hash();
    m_locked.store(true);

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("success", common::JsonValue(true));
    return response;
  }

  common::JsonValue MultiWalletManager::deleteWallet(const std::string &walletId,
                                                     const std::string &password)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (walletId == m_activeWalletId)
      return makeErrorResponse("Cannot delete the currently loaded wallet. Unload it first.");

    if (!walletExists(walletId))
      return makeErrorResponse("Wallet not found: " + walletId);

    WalletKeys keys;
    WalletState state;
    std::string path = stateFilePath(walletId);
    if (!loadWalletStateFromFile(path, password, keys, state))
      return makeErrorResponse("Wrong password");

    std::remove(path.c_str());
    removeIndexEntry(walletId);

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("success", common::JsonValue(true));
    response.insert("deleted", common::JsonValue(walletId));
    return response;
  }

  common::JsonValue MultiWalletManager::listWallets() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto entries = readAllIndexEntries();

    common::JsonValue response(common::JsonValue::OBJECT);
    common::JsonValue &wallets = response.insert("wallets", common::JsonValue(common::JsonValue::ARRAY));

    for (const auto &entry : entries)
    {
      common::JsonValue walletJson(common::JsonValue::OBJECT);
      walletJson.insert("wallet_id", common::JsonValue(entry.id));
      walletJson.insert("address", common::JsonValue(entry.address));
      walletJson.insert("state_file", common::JsonValue(entry.stateFilePath));

      if (entry.id == m_activeWalletId && !m_locked.load())
      {
        walletJson.insert("balance", common::JsonValue(static_cast<int64_t>(m_activeState.balance)));
        walletJson.insert("loaded", common::JsonValue(true));
      }
      else
      {
        walletJson.insert("balance", common::JsonValue(static_cast<int64_t>(0)));
        walletJson.insert("loaded", common::JsonValue(false));
      }

      wallets.pushBack(std::move(walletJson));
    }

    return response;
  }

  // Wallet Operations
  common::JsonValue MultiWalletManager::getStatus() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(m_activeWalletId));
    response.insert("address", common::JsonValue(m_activeKeys.address));
    response.insert("locked", common::JsonValue(m_locked.load()));
    response.insert("balance", common::JsonValue(static_cast<int64_t>(m_activeState.balance)));
    response.insert("unlockedBalance", common::JsonValue(static_cast<int64_t>(m_activeState.unlockedBalance)));
    response.insert("height", common::JsonValue(static_cast<int64_t>(m_activeState.lastHeight)));
    response.insert("outputCount", common::JsonValue(static_cast<int64_t>(m_activeState.ownedOutputs.size())));
    response.insert("transactionCount", common::JsonValue(static_cast<int64_t>(m_activeState.transactions.size())));

    return response;
  }

  common::JsonValue MultiWalletManager::getBalance() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("balance", common::JsonValue(static_cast<int64_t>(m_activeState.balance)));
    response.insert("unlockedBalance", common::JsonValue(static_cast<int64_t>(m_activeState.unlockedBalance)));
    return response;
  }

  common::JsonValue MultiWalletManager::getAddress() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("address", common::JsonValue(m_activeKeys.address));
    return response;
  }

  common::JsonValue MultiWalletManager::getOutputs(const common::JsonValue &params) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    bool unspentOnly = true;
    if (params.isObject() && params.contains("unspent_only"))
      unspentOnly = params("unspent_only").getBool();

    common::JsonValue response(common::JsonValue::OBJECT);
    common::JsonValue &arr = response.insert("outputs", common::JsonValue(common::JsonValue::ARRAY));

    for (const auto &out : m_activeState.ownedOutputs)
    {
      if (unspentOnly && out.spent)
        continue;

      common::JsonValue outJson(common::JsonValue::OBJECT);
      outJson.insert("height", common::JsonValue(static_cast<int64_t>(out.blockHeight)));
      outJson.insert("txHash", common::JsonValue(common::podToHex(out.txHash)));
      outJson.insert("amount", common::JsonValue(static_cast<int64_t>(out.amount)));
      outJson.insert("outputIndex", common::JsonValue(static_cast<int64_t>(out.outputIndex)));
      outJson.insert("spent", common::JsonValue(out.spent));
      outJson.insert("isDeposit", common::JsonValue(out.isDeposit));
      arr.pushBack(std::move(outJson));
    }

    return response;
  }

  common::JsonValue MultiWalletManager::getTransactions(const common::JsonValue &params) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    uint32_t offset = 0, limit = 50;
    if (params.isObject())
    {
      if (params.contains("offset"))
        offset = static_cast<uint32_t>(params("offset").getInteger());
      if (params.contains("limit"))
        limit = static_cast<uint32_t>(params("limit").getInteger());
    }

    common::JsonValue response(common::JsonValue::OBJECT);
    common::JsonValue &arr = response.insert("transactions", common::JsonValue(common::JsonValue::ARRAY));

    auto &txs = m_activeState.transactions;
    for (size_t i = offset; i < txs.size() && i < offset + limit; ++i)
    {
      const auto &tx = txs[i];
      common::JsonValue txJson(common::JsonValue::OBJECT);
      txJson.insert("txHash", common::JsonValue(common::podToHex(tx.txHash)));
      txJson.insert("height", common::JsonValue(static_cast<int64_t>(tx.blockHeight)));
      txJson.insert("timestamp", common::JsonValue(static_cast<int64_t>(tx.timestamp)));
      txJson.insert("fee", common::JsonValue(static_cast<int64_t>(tx.fee)));
      txJson.insert("totalSent", common::JsonValue(static_cast<int64_t>(tx.totalSent)));
      txJson.insert("totalReceived", common::JsonValue(static_cast<int64_t>(tx.totalReceived)));
      txJson.insert("type", common::JsonValue(static_cast<int64_t>(tx.type)));
      txJson.insert("confirmed", common::JsonValue(tx.confirmed));
      arr.pushBack(std::move(txJson));
    }

    return response;
  }

  common::JsonValue MultiWalletManager::sendTransfer(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("txHash", common::JsonValue(std::string("placeholder_transfer_tx_hash")));
    response.insert("fee", common::JsonValue(static_cast<int64_t>(0)));
    return response;
  }

  common::JsonValue MultiWalletManager::sendDeposit(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("txHash", common::JsonValue(std::string("placeholder_deposit_tx_hash")));
    response.insert("depositId", common::JsonValue(static_cast<int64_t>(0)));
    return response;
  }

  common::JsonValue MultiWalletManager::sendWithdrawal(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("txHash", common::JsonValue(std::string("placeholder_withdrawal_tx_hash")));
    return response;
  }

  // Export
  common::JsonValue MultiWalletManager::exportKeys() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(m_activeWalletId));
    response.insert("viewSecretKey", common::JsonValue(common::podToHex(m_activeKeys.viewSecretKey)));
    response.insert("spendSecretKey", common::JsonValue(common::podToHex(m_activeKeys.spendSecretKey)));
    response.insert("spendPublicKey", common::JsonValue(common::podToHex(m_activeKeys.spendPublicKey)));
    response.insert("address", common::JsonValue(m_activeKeys.address));
    return response;
  }

  common::JsonValue MultiWalletManager::exportState() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("wallet_id", common::JsonValue(m_activeWalletId));
    response.insert("state_file", common::JsonValue(stateFilePath(m_activeWalletId)));
    return response;
  }

  common::JsonValue MultiWalletManager::changePassword(const std::string &oldPassword,
                                                       const std::string &newPassword)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_activeWalletId.empty())
      return makeErrorResponse("No wallet loaded");

    crypto::Hash oldHash;
    crypto::cn_fast_hash(oldPassword.data(), oldPassword.size(), oldHash);
    if (std::memcmp(oldHash.data, m_passwordHash.data, sizeof(m_passwordHash.data)) != 0)
      return makeErrorResponse("Old password is incorrect");

    std::string path = stateFilePath(m_activeWalletId);
    if (!saveWalletStateToFile(path, m_activeKeys, m_activeState, newPassword))
      return makeErrorResponse("Failed to save with new password");

    crypto::cn_fast_hash(newPassword.data(), newPassword.size(), m_passwordHash);

    common::JsonValue response(common::JsonValue::OBJECT);
    response.insert("success", common::JsonValue(true));
    return response;
  }

  // Queries
  bool MultiWalletManager::hasActiveWallet() const
  {
    return !m_activeWalletId.empty() && !m_locked.load();
  }

  std::string MultiWalletManager::activeWalletId() const
  {
    return m_activeWalletId;
  }

  bool MultiWalletManager::walletExists(const std::string &walletId) const
  {
    auto entries = readAllIndexEntries();
    for (const auto &entry : entries)
    {
      if (entry.id == walletId)
        return true;
    }
    return false;
  }

  // Private Helpers
  void MultiWalletManager::saveActiveWallet()
  {
    if (m_activeWalletId.empty())
      return;

    std::string path = stateFilePath(m_activeWalletId);
    saveWalletStateToFile(path, m_activeKeys, m_activeState, "");
  }

  bool MultiWalletManager::loadWalletState(const std::string &walletId,
                                           const std::string &password,
                                           WalletKeys &keys,
                                           WalletState &state)
  {
    std::string path = stateFilePath(walletId);
    return loadWalletStateFromFile(path, password, keys, state);
  }

  void MultiWalletManager::startSyncForActive()
  {
    if (!m_node || m_activeWalletId.empty())
      return;

    DaemonRpcCallback rpcCallback = [this](const std::string &method,
                                           const std::string &paramsJson) -> std::string
    {
      return "{}";
    };

    std::string syncDir = m_walletsDir + "/" + m_activeWalletId + "_sync";
    tools::create_directories_if_necessary(syncDir);

    m_syncManager = std::unique_ptr<SyncManager>(new SyncManager(
        *m_node,
        m_activeKeys.viewSecretKey,
        m_activeKeys.spendPublicKey,
        syncDir,
        rpcCallback));

    m_syncManager->start(
        [this](const SyncProgress &progress) {},
        [this](const std::vector<OutputInfo> &newOutputs,
               const std::vector<crypto::KeyImage> &spentKeyImages)
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          for (const auto &out : newOutputs)
            m_activeState.ownedOutputs.push_back(out);
          for (const auto &ki : spentKeyImages)
            m_activeState.spentKeyImages.push_back(ki);
        });
  }

  void MultiWalletManager::stopActiveSync()
  {
    if (m_syncManager)
    {
      m_syncManager->stop();
      m_syncManager.reset();
    }
  }

} // namespace BoltRPC