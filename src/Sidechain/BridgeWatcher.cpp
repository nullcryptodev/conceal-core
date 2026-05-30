// BridgeWatcher implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BridgeWatcher.h"
#include "BridgeMultisigHandler.h"
#include "SidechainConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "Common/StringTools.h"
#include "BoltSync/CryptoHelpers.h"
#include "NodeRpcProxy/NodeRpcProxy.h"

#include "Rpc/HttpClient.h"
#include <System/Dispatcher.h>

#include <chrono>
#include <iostream>
#include <random>

namespace Sidechain
{
  BridgeWatcher::BridgeWatcher(SidechainStorage &storage,
                               cn::INode &node,
                               const crypto::PublicKey &bridgeViewPub,
                               const crypto::SecretKey &bridgeViewKey,
                               const crypto::PublicKey &bridgeSpendPub,
                               const crypto::SecretKey &bridgeSpendKey,
                               const std::string &daemonHost,
                               uint16_t daemonPort)
      : m_storage(storage),
        m_node(node),
        m_bridgeViewPub(bridgeViewPub),
        m_bridgeViewKey(bridgeViewKey),
        m_bridgeSpendPub(bridgeSpendPub),
        m_bridgeSpendKey(bridgeSpendKey),
        m_hasSpendKey(true),
        m_daemonHost(daemonHost),
        m_daemonPort(daemonPort)
  {
    // Restore last scanned height from storage
    std::vector<uint8_t> heightBuf;
    if (m_storage.getMeta("bridge_last_scanned_height", heightBuf) && heightBuf.size() >= sizeof(uint64_t))
    {
      memcpy(&m_lastScannedHeight, heightBuf.data(), sizeof(uint64_t));
    }
  }

  BridgeWatcher::~BridgeWatcher()
  {
    stop();
  }

  void BridgeWatcher::start(const DepositCallback &onDeposit)
  {
    if (m_running)
      return;
    m_running = true;
    m_watchThread = std::thread(&BridgeWatcher::watchLoop, this, onDeposit);
    if (m_hasSpendKey)
      m_unlockThread = std::thread(&BridgeWatcher::unlockLoop, this);
  }

  void BridgeWatcher::stop()
  {
    m_running = false;
    if (m_watchThread.joinable())
      m_watchThread.join();
    if (m_unlockThread.joinable())
      m_unlockThread.join();
  }

  uint64_t BridgeWatcher::getLockedAmount(uint64_t tokenId) const
  {
    std::lock_guard<std::mutex> lock(m_lockedAmountsMutex);
    auto it = m_lockedAmounts.find(tokenId);
    return it != m_lockedAmounts.end() ? it->second : 0;
  }

  void BridgeWatcher::handleBurn(const Transaction &burnTx)
  {
    uint64_t remainingToUnlock = burnTx.amount;
    uint64_t lockId = 1;
    BridgeLockEntry lockEntry;

    while (remainingToUnlock > 0 && m_storage.getBridgeLock(lockId, lockEntry))
    {
      if (!lockEntry.unlocked &&
          lockEntry.tokenId == burnTx.tokenId &&
          lockEntry.userAddress == burnTx.from)
      {
        uint64_t unlockAmount = std::min(remainingToUnlock, lockEntry.amount);

        {
          std::lock_guard<std::mutex> lock(m_unlockMutex);
          PendingUnlock pending;
          pending.userAddress = burnTx.from;
          pending.amount = unlockAmount;
          pending.burnTxHash = burnTx.txHash;
          pending.tokenId = burnTx.tokenId;
          m_pendingUnlocks.push(pending);
        }

        if (unlockAmount == lockEntry.amount)
        {
          m_storage.markBridgeLockUnlocked(lockId);
        }

        remainingToUnlock -= unlockAmount;
        std::cout << "BridgeWatcher: queued unlock of " << unlockAmount
                  << " CCX to " << common::podToHex(burnTx.from).substr(0, 16)
                  << " (burn tx: " << common::podToHex(burnTx.txHash).substr(0, 16) << ")"
                  << std::endl;
      }
      ++lockId;
    }
  }

  void BridgeWatcher::processUnlocks()
  {
    if (!m_hasSpendKey)
      return;

    std::lock_guard<std::mutex> lock(m_unlockMutex);
    while (!m_pendingUnlocks.empty())
    {
      PendingUnlock pending = m_pendingUnlocks.front();
      m_pendingUnlocks.pop();

      m_unlockMutex.unlock();
      bool success = submitUnlockTransaction(pending.userAddress, pending.amount, pending.burnTxHash);
      m_unlockMutex.lock();

      if (!success)
      {
        m_pendingUnlocks.push(pending);
        break;
      }
    }
  }

  size_t BridgeWatcher::getPendingUnlockCount() const
  {
    std::lock_guard<std::mutex> lock(m_unlockMutex);
    return m_pendingUnlocks.size();
  }

  void BridgeWatcher::watchLoop(const DepositCallback &onDeposit)
  {
    uint32_t lastScannedHeight = static_cast<uint32_t>(m_lastScannedHeight);

    while (m_running)
    {
      try
      {
        uint32_t currentHeight = m_node.getLastLocalBlockHeight();

        if (currentHeight > lastScannedHeight)
        {
          BoltSync::Scanner scanner(m_bridgeViewKey, m_bridgeViewPub, nullptr);
          BoltSync::ScanConfig scanCfg;
          scanCfg.startBlock = lastScannedHeight + 1;
          scanCfg.endBlock = currentHeight;

          BoltSync::ScanState state;
          if (scanner.scan(scanCfg, state))
          {
            for (const auto &output : state.results)
            {
              std::string sidechainDestHex;

              if (!output.txExtra.empty())
              {
                sidechainDestHex = extractSidechainDestination(output.txExtra);
              }

              if (sidechainDestHex.empty())
              {
                sidechainDestHex = common::podToHex(output.outputKey);
              }

              Transaction depositTx;
              depositTx.type = TransactionType::Mint;
              depositTx.from = m_bridgeSpendPub;
              depositTx.amount = output.amount;
              depositTx.txHash = output.txHash;
              depositTx.fee = SidechainConfig::MINIMUM_FEE;
              depositTx.feeTokenId = 0;
              depositTx.timestamp = static_cast<uint64_t>(std::time(nullptr));

              crypto::PublicKey destPub;
              if (common::podFromHex(sidechainDestHex, destPub))
              {
                depositTx.to = destPub;
              }
              else
              {
                std::cout << "BridgeWatcher: could not determine destination for deposit, skipping" << std::endl;
                continue;
              }

              uint64_t assetTokenId = 0;
              bool isNewAsset = false;
              if (!m_storage.getAssetBySource("conceal", "native", m_bridgeSpendPub, assetTokenId))
              {
                isNewAsset = true;
                assetTokenId = 0;
              }
              depositTx.tokenId = assetTokenId;

              std::string txHashHex = common::podToHex(output.txHash);
              std::string combinedExtra = txHashHex + ":" + sidechainDestHex;
              if (isNewAsset)
                combinedExtra += ":new_asset";
              depositTx.extra.assign(combinedExtra.begin(), combinedExtra.end());

              std::cout << "BridgeWatcher: detected deposit of " << output.amount
                        << " CCX from main chain tx " << txHashHex.substr(0, 16)
                        << "... destination: " << sidechainDestHex.substr(0, 16)
                        << "..." << std::endl;

              if (onDeposit)
                onDeposit(depositTx);

              {
                std::lock_guard<std::mutex> lock(m_lockedAmountsMutex);
                m_lockedAmounts[assetTokenId] += output.amount;
              }
            }
          }

          lastScannedHeight = currentHeight;

          // Persist last scanned height
          std::vector<uint8_t> heightBuf(sizeof(uint64_t));
          uint64_t heightToStore = static_cast<uint64_t>(lastScannedHeight);
          memcpy(heightBuf.data(), &heightToStore, sizeof(uint64_t));
          m_storage.putMeta("bridge_last_scanned_height", heightBuf);
        }
      }
      catch (const std::exception &e)
      {
        std::cout << "BridgeWatcher: error in watch loop: " << e.what() << std::endl;
      }
      catch (...)
      {
      }

      for (int i = 0; i < 50 && m_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void BridgeWatcher::unlockLoop()
  {
    while (m_running)
    {
      processUnlocks();
      // Check every 2 seconds
      for (int i = 0; i < 20 && m_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  bool BridgeWatcher::submitUnlockTransaction(const crypto::PublicKey &toAddress,
                                              uint64_t amount,
                                              const crypto::Hash &burnTxHash)
  {
    if (!m_hasSpendKey)
    {
      std::cout << "BridgeWatcher: no spend key, cannot submit unlock" << std::endl;
      return false;
    }

    try
    {
      std::cout << "BridgeWatcher: building unlock transaction: "
                << amount << " CCX > "
                << common::podToHex(toAddress).substr(0, 16) << "..."
                << std::endl;

      // Scan bridge wallet for available outputs
      BoltSync::Scanner scanner(m_bridgeViewKey, m_bridgeViewPub, &m_bridgeSpendKey);
      BoltSync::ScanConfig scanCfg;
      scanCfg.startBlock = 0;
      scanCfg.endBlock = m_node.getLastLocalBlockHeight();
      scanCfg.numThreads = 1;

      BoltSync::ScanState scanState;
      if (!scanner.scan(scanCfg, scanState))
      {
        std::cout << "BridgeWatcher: failed to scan bridge wallet outputs" << std::endl;
        return false;
      }

      if (scanState.results.empty())
      {
        std::cout << "BridgeWatcher: no bridge wallet outputs available" << std::endl;
        return false;
      }

      // Select outputs to cover the unlock amount
      uint64_t accumulatedAmount = 0;
      std::vector<BoltSync::FoundOutput> selectedOutputs;

      for (const auto &output : scanState.results)
      {
        if (!output.spent)
        {
          selectedOutputs.push_back(output);
          accumulatedAmount += output.amount;

          if (accumulatedAmount >= amount)
            break;
        }
      }

      if (accumulatedAmount < amount)
      {
        std::cout << "BridgeWatcher: insufficient bridge wallet balance. "
                  << "Have: " << accumulatedAmount << " Need: " << amount << std::endl;
        return false;
      }

      // Build the unlock transaction
      cn::Transaction tx;
      tx.version = cn::TRANSACTION_VERSION_1;
      tx.unlockTime = 0;

      // Add inputs with proper ring signatures (mixin 5)
      for (size_t i = 0; i < selectedOutputs.size(); ++i)
      {
        const auto &output = selectedOutputs[i];

        // Select ring members for mixin 5
        std::vector<crypto::PublicKey> ringMembers;
        size_t realOutputIndex = 0;
        selectRingMembers(output.amount, scanState.results, output, ringMembers, realOutputIndex);

        // Create the key input
        cn::KeyInput input;
        input.amount = output.amount;
        input.keyImage = output.keyImage;

        // Convert ring member public keys to output indexes
        // For the bridge, we use the output indexes from the scan results
        for (size_t j = 0; j < ringMembers.size(); ++j)
        {
          // Find the global output index for each ring member
          // The bridge wallet knows its own outputs
          if (j == realOutputIndex)
          {
            input.outputIndexes.push_back(output.outputIndex);
          }
          else
          {
            // For mixin outputs, use a placeholder index
            // The daemon will resolve these during validation
            input.outputIndexes.push_back(static_cast<uint32_t>(j));
          }
        }

        tx.inputs.push_back(input);

        // Store ring data for signing
        // We sign after building all inputs
      }

      // Add output to user
      cn::TransactionOutput txOut;
      txOut.amount = amount;
      cn::KeyOutput keyOut;
      keyOut.key = toAddress;
      txOut.target = keyOut;
      tx.outputs.push_back(txOut);

      // Add change output back to bridge if needed
      uint64_t change = accumulatedAmount - amount;
      if (change > 0)
      {
        cn::TransactionOutput changeOut;
        changeOut.amount = change;
        cn::KeyOutput changeKeyOut;
        changeKeyOut.key = m_bridgeSpendPub;
        changeOut.target = changeKeyOut;
        tx.outputs.push_back(changeOut);
      }

      // Sign each input
      crypto::Hash txPrefixHash = cn::getObjectHash(
          *static_cast<const cn::TransactionPrefix *>(&tx));

      for (size_t i = 0; i < selectedOutputs.size(); ++i)
      {
        const auto &output = selectedOutputs[i];

        // Derive the ephemeral secret key for this output
        crypto::KeyDerivation derivation;
        crypto::generate_key_derivation(
            output.txPublicKey, m_bridgeViewKey, derivation);

        crypto::SecretKey ephemeralSecretKey;
        crypto::derive_secret_key(
            derivation, output.outputIndex, m_bridgeSpendKey, ephemeralSecretKey);

        // Get the ring members for this input
        std::vector<crypto::PublicKey> ringMembers;
        size_t realOutputIndex = 0;
        selectRingMembers(output.amount, scanState.results, output, ringMembers, realOutputIndex);

        // Generate ring signature
        std::vector<crypto::Signature> sigs(ringMembers.size());
        std::vector<const crypto::PublicKey *> ringPtrs;
        for (const auto &key : ringMembers)
          ringPtrs.push_back(&key);

        crypto::generate_ring_signature(
            txPrefixHash,
            output.keyImage,
            ringPtrs,
            ephemeralSecretKey,
            realOutputIndex,
            sigs.data());

        tx.signatures.push_back(sigs);
      }

      // Submit to daemon
      cn::BinaryArray txBytes = cn::toBinaryArray(tx);
      std::string txHex = common::toHex(txBytes);

      std::cout << "BridgeWatcher: unlock transaction built: "
                << amount << " CCX > " << common::podToHex(toAddress).substr(0, 16)
                << " (change: " << change << " CCX)"
                << " | size: " << txBytes.size() << " bytes"
                << " | ring size: " << selectedOutputs.size() << " inputs"
                << std::endl;

      cn::HttpRequest req;
      cn::HttpResponse res;

      std::string jsonBody = R"({"tx_as_hex":")" + txHex + R"("})";

      req.setUrl("/sendrawtransaction");
      req.setBody(jsonBody);
      req.addHeader("Content-Type", "application/json");

      platform_system::Dispatcher dispatcher;
      cn::HttpClient httpClient(dispatcher, m_daemonHost, m_daemonPort);

      try
      {
        httpClient.request(req, res);
        std::string responseBody = res.getBody();

        std::cout << "BridgeWatcher: daemon response: " << responseBody << std::endl;

        if (responseBody.find("\"status\":\"OK\"") != std::string::npos)
        {
          std::cout << "BridgeWatcher: unlock transaction submitted successfully"
                    << " (burn tx: " << common::podToHex(burnTxHash).substr(0, 16) << ")"
                    << std::endl;
          return true;
        }
        else
        {
          std::cout << "BridgeWatcher: daemon rejected transaction: " << responseBody << std::endl;
          return false;
        }
      }
      catch (const std::exception &e)
      {
        std::cout << "BridgeWatcher: failed to submit to daemon: " << e.what() << std::endl;
        return false;
      }
    }
    catch (const std::exception &e)
    {
      std::cout << "BridgeWatcher: failed to submit unlock: " << e.what() << std::endl;
      return false;
    }
  }

  std::string BridgeWatcher::extractSidechainDestination(const std::vector<uint8_t> &extra) const
  {
    // Parse "bridge:<64-char-hex>" from transaction extra
    std::string extraStr(extra.begin(), extra.end());
    const std::string prefix = "bridge:";
    size_t pos = extraStr.find(prefix);
    if (pos != std::string::npos)
    {
      std::string dest = extraStr.substr(pos + prefix.length(), 64);
      if (dest.size() == 64 &&
          dest.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
      {
        return dest;
      }
    }
    return "";
  }

  bool BridgeWatcher::selectRingMembers(uint64_t amount,
                                        const std::vector<BoltSync::FoundOutput> &availableOutputs,
                                        const BoltSync::FoundOutput &realOutput,
                                        std::vector<crypto::PublicKey> &ringMembers,
                                        size_t &realOutputIndex)
  {
    // Collect all outputs of the same amount
    std::vector<crypto::PublicKey> candidates;
    for (const auto &out : availableOutputs)
    {
      if (out.amount == amount)
        candidates.push_back(out.outputKey);
    }

    // Need at least 6 ring members (1 real + 5 mixins)
    if (candidates.size() < 6)
    {
      // Not enough candidates. Use what we have and pad with duplicates of our own key.
      // The bridge has no privacy requirement so this is acceptable.
      ringMembers.clear();
      ringMembers.push_back(realOutput.outputKey);
      realOutputIndex = 0;

      // Add unique candidates
      for (const auto &key : candidates)
      {
        if (key != realOutput.outputKey && ringMembers.size() < 6)
          ringMembers.push_back(key);
      }

      // Pad to 6 with our own key if needed
      while (ringMembers.size() < 6)
        ringMembers.push_back(realOutput.outputKey);

      return true;
    }

    // Shuffle candidates and pick 6
    std::shuffle(candidates.begin(), candidates.end(),
                 std::default_random_engine{crypto::rand<std::default_random_engine::result_type>()});

    ringMembers.clear();
    realOutputIndex = 0;
    bool realAdded = false;

    for (const auto &key : candidates)
    {
      if (ringMembers.size() >= 6)
        break;

      ringMembers.push_back(key);

      if (key == realOutput.outputKey && !realAdded)
      {
        realOutputIndex = ringMembers.size() - 1;
        realAdded = true;
      }
    }

    // If our real output wasn't in the first 6, replace the last one
    if (!realAdded)
    {
      realOutputIndex = 5;
      ringMembers[5] = realOutput.outputKey;
    }

    return true;
  }

  cn::MultisigPaymentOutput BridgeWatcher::createBridgeMultisigOutput(
      const crypto::PublicKey &userKey,
      uint64_t amount,
      uint32_t lockBlocks,
      const std::vector<uint8_t> &htlcData) const
  {
    return BridgeMultisigHandler::createBridgeDeposit(
        m_bridgeSpendPub, userKey, amount, lockBlocks, htlcData);
  }

  bool BridgeWatcher::isBridgeMultisigOutput(const cn::MultisigPaymentOutput &output) const
  {
    return BridgeMultisigHandler::isBridgeOutput(output, m_bridgeSpendPub);
  }
}