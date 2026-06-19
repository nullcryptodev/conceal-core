// TransactionBuilder implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "TransactionBuilder.h"
#include "OutputSelector.h"
#include "SignatureBuilder.h"
#include "RelayHandler.h"
#include "OutputUtils.h"
#include "ITransaction.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteConfig.h"
#include "INode.h"
#include "Common/StringTools.h"
#include <CryptoNoteCore/TransactionExtra.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <random>
#include <cmath>
#include <system_error>

namespace BoltCore
{
  TransactionBuilder::TransactionBuilder(const cn::Currency &currency,
                                         cn::INode &node,
                                         OutputSelector &outputSelector,
                                         SignatureBuilder &signatureBuilder,
                                         RelayHandler &relayHandler,
                                         const cn::AccountKeys &accountKeys)
      : m_currency(currency), m_node(node), m_outputSelector(outputSelector),
        m_signatureBuilder(signatureBuilder), m_relayHandler(relayHandler),
        m_accountKeys(accountKeys) {}

  namespace
  {
    using outs_for_amount = cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount;
    using out_entry = cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry;

    void decomposeAmount(uint64_t amount, uint64_t dustThreshold, std::vector<uint64_t> &amounts)
    {
      cn::decompose_amount_into_digits(
          amount, dustThreshold,
          [&amounts](uint64_t chunk)
          { amounts.push_back(chunk); },
          [&amounts](uint64_t dust)
          { amounts.push_back(dust); });
    }

    uint64_t countNeededMoney(const std::vector<Transfer> &transfers, uint64_t fee)
    {
      uint64_t needed = fee;
      for (const auto &t : transfers)
        needed += t.amount;
      return needed;
    }

    // Ring mixin defaults to cn::parameters::MINIMUM_MIXIN unless caller overrides (0 = default).
    uint64_t effectiveMixin(uint64_t mixin)
    {
      return mixin != 0 ? mixin : static_cast<uint64_t>(cn::parameters::MINIMUM_MIXIN);
    }

    std::error_code waitNodeFuture(std::future<std::error_code> &future,
                                   std::chrono::milliseconds timeout)
    {
      if (future.wait_for(timeout) != std::future_status::ready)
        return std::make_error_code(std::errc::timed_out);
      return future.get();
    }

    // WalletGreen::prepareInputs uses transfer.globalOutputIndex from the wallet record.
    bool storedGlobalOutputIndex(const OutputInfo &output,
                                 uint32_t &globalOutputIndex,
                                 std::string &error)
    {
      if (output.blockHeight == 0)
      {
        error = "Global output index unavailable until transaction is confirmed in a block";
        return false;
      }
      if (!output.hasGlobalOutputIndex)
      {
        error = "Missing global output index on UTXO (wait for sync or rescan wallet)";
        return false;
      }
      globalOutputIndex = output.globalOutputIndex;
      return true;
    }
  }

  bool TransactionBuilder::resolveGlobalOutputIndex(const OutputInfo &output,
                                                    uint32_t &globalOutputIndex,
                                                    std::string *errorOut) const
  {
    const auto setError = [errorOut](const std::string &msg)
    {
      if (errorOut)
        *errorOut = msg;
    };

    if (output.hasGlobalOutputIndex)
    {
      globalOutputIndex = output.globalOutputIndex;
      return true;
    }

    if (output.blockHeight == 0)
    {
      setError("Global output index unavailable until transaction is confirmed in a block");
      return false;
    }

    const std::string txHex = common::podToHex(output.txHash);

    std::vector<uint32_t> indices;
    std::promise<std::error_code> promise;
    auto future = promise.get_future();
    m_node.getTransactionOutsGlobalIndices(
        output.txHash, indices,
        [&promise](std::error_code ec)
        { promise.set_value(ec); });
    const std::error_code ec =
        waitNodeFuture(future, std::chrono::seconds(10));
    if (ec)
    {
      setError("Missing global output index for tx " + txHex.substr(0, 16) +
               "... (connect daemon on port " + std::to_string(cn::RPC_DEFAULT_PORT) +
               " or rescan wallet)");
      return false;
    }
    if (output.outputIndex >= indices.size())
    {
      setError("Missing global output index for tx " + txHex.substr(0, 16) +
               "... output " + std::to_string(output.outputIndex) +
               " (rescan wallet or wait for sync)");
      return false;
    }

    globalOutputIndex = indices[output.outputIndex];
    return true;
  }

  bool TransactionBuilder::requestMixinOutputs(
      const std::vector<OutputInfo> &selectedOutputs,
      uint64_t mixin,
      std::vector<outs_for_amount> &mixinResult,
      std::string &error) const
  {
    error.clear();
    std::vector<uint64_t> amounts;
    amounts.reserve(selectedOutputs.size());
    for (const auto &out : selectedOutputs)
      amounts.push_back(out.amount);

    // Ring size is `mixin` decoys (default: cn::parameters::MINIMUM_MIXIN). Request one extra
    // random out from the daemon so the real input can be skipped if in the candidate set
    // (WalletLegacy: mixIn + 1 for outs_count only — ring mixin unchanged).
    const uint64_t daemonOutsCount = mixin + 1;

    std::promise<std::error_code> promise;
    auto future = promise.get_future();
    m_node.getRandomOutsByAmounts(
        std::move(amounts), daemonOutsCount, mixinResult,
        [&promise](std::error_code ec)
        { promise.set_value(ec); });
    const std::error_code ec =
        waitNodeFuture(future, std::chrono::seconds(25));
    if (ec)
    {
      if (ec == std::errc::timed_out)
        error = "Timed out waiting for daemon mixin outputs (ensure conceald RPC on port 16000 is synced)";
      else if (ec == std::errc::not_connected)
        error = "No daemon connection (offline mode). Start conceald or use --daemon HOST:16000";
      else if (ec == std::errc::connection_refused)
        error = "Cannot connect to daemon RPC for mixin outputs (wallet uses RPC port 16000, not P2P port 15000)";
      else if (ec == std::errc::resource_unavailable_try_again)
        error = "Daemon core is busy — wait for 'Synchronization complete' in conceald log, then retry";
      else if (ec == std::errc::protocol_error)
        error = "Daemon rejected getrandom_outs — ensure conceald is fully synced, RPC enabled, and the chain has outputs at this amount";
      else
        error = "Failed to fetch mixin outputs from daemon (verify --daemon 127.0.0.1:16000 matches your conceald RPC bind)";
      return false;
    }

    const auto notEnough = std::find_if(
        mixinResult.begin(), mixinResult.end(),
        [mixin](const outs_for_amount &entry)
        { return entry.outs.size() < mixin; });
    if (notEnough != mixinResult.end())
    {
      error = "Not enough decoy outputs for amount " + m_currency.formatAmount(notEnough->amount) +
              " (need " + std::to_string(mixin) +
              "). Try fusion first, or ensure the daemon is fully synced.";
      return false;
    }
    return true;
  }

  bool TransactionBuilder::prepareKeyInputs(
      const std::vector<OutputInfo> &selectedOutputs,
      std::vector<outs_for_amount> &mixinResult,
      uint64_t mixin,
      std::vector<KeyInputBundle> &keyInputs,
      std::string &error) const
  {
    error.clear();
    keyInputs.clear();
    keyInputs.reserve(selectedOutputs.size());

    size_t mixinIdx = 0;
    for (const auto &selected : selectedOutputs)
    {
      uint32_t globalOutputIndex = 0;
      if (!storedGlobalOutputIndex(selected, globalOutputIndex, error))
      {
        if (error.empty())
          error = "Failed to resolve global output index for an input";
        return false;
      }

      cn::transaction_types::InputKeyInfo keyInfo;
      keyInfo.amount = selected.amount;

      if (!mixinResult.empty() && mixinIdx < mixinResult.size())
      {
        auto &mixOuts = mixinResult[mixinIdx].outs;
        std::sort(mixOuts.begin(), mixOuts.end(),
                  [](const out_entry &a, const out_entry &b)
                  { return a.global_amount_index < b.global_amount_index; });

        for (const auto &fakeOut : mixOuts)
        {
          if (fakeOut.global_amount_index == globalOutputIndex)
            continue;

          cn::transaction_types::GlobalOutput globalOutput;
          globalOutput.outputIndex = static_cast<uint32_t>(fakeOut.global_amount_index);
          globalOutput.targetKey = fakeOut.out_key;
          keyInfo.outputs.push_back(std::move(globalOutput));
          if (keyInfo.outputs.size() >= mixin)
            break;
        }
      }

      if (keyInfo.outputs.size() < mixin)
      {
        error = "Not enough mixin outputs after excluding real output (amount " +
                m_currency.formatAmount(selected.amount) + ", need " + std::to_string(mixin) +
                " decoys). Try fusion first or ensure the daemon is fully synced.";
        return false;
      }

      const auto insertAt = std::find_if(
          keyInfo.outputs.begin(), keyInfo.outputs.end(),
          [globalOutputIndex](const cn::transaction_types::GlobalOutput &entry)
          { return entry.outputIndex >= globalOutputIndex; });

      cn::transaction_types::GlobalOutput realOutput;
      realOutput.outputIndex = globalOutputIndex;
      realOutput.targetKey = selected.outputKey;
      const auto insertedAt = keyInfo.outputs.insert(insertAt, realOutput);

      keyInfo.realOutput.transactionPublicKey = selected.txPublicKey;
      keyInfo.realOutput.transactionIndex =
          static_cast<size_t>(insertedAt - keyInfo.outputs.begin());
      keyInfo.realOutput.outputInTransaction = outputSigningIndex(selected);

      KeyInputBundle bundle;
      bundle.keyInfo = std::move(keyInfo);
      keyInputs.push_back(std::move(bundle));
      ++mixinIdx;
    }

    return true;
  }

  bool TransactionBuilder::addKeyInputsAndSign(cn::ITransaction &tx,
                                               std::vector<KeyInputBundle> &keyInputs) const
  {
    // WalletGreen: add all inputs first, then sign (signInputKey sets signatures non-empty).
    for (auto &input : keyInputs)
      tx.addInput(m_accountKeys, input.keyInfo, input.ephKeys);

    size_t inputIndex = 0;
    for (auto &input : keyInputs)
      tx.signInputKey(inputIndex++, input.keyInfo, input.ephKeys);
    return true;
  }

  void TransactionBuilder::applyPlannedOutputs(cn::ITransaction &tx,
                                             const std::vector<PlannedOutput> &plannedOutputs) const
  {
    for (const auto &out : plannedOutputs)
      tx.addOutput(out.amount, out.address);
  }

  void TransactionBuilder::appendTransferExtras(cn::ITransaction &tx,
                                                const BuilderParams &params) const
  {
    if (params.ttl != 0)
    {
      cn::BinaryArray ba;
      cn::appendTTLToExtra(ba, params.ttl);
      tx.appendExtra(ba);
    }

    if (!params.extra.empty())
      tx.appendExtra(common::asBinaryArray(params.extra));
  }

  bool TransactionBuilder::ensureGlobalOutputIndices(std::vector<OutputInfo> &outputs,
                                                      std::string &error) const
  {
    error.clear();
    for (auto &out : outputs)
    {
      if (out.blockHeight == 0)
      {
        error = "Global output index unavailable until transaction is confirmed in a block";
        return false;
      }
      if (out.hasGlobalOutputIndex)
        continue;

      uint32_t globalIndex = 0;
      if (!resolveGlobalOutputIndex(out, globalIndex, &error))
      {
        if (error.empty())
          error = "Missing global output index on UTXO (wait for sync or rescan wallet)";
        return false;
      }
      out.globalOutputIndex = globalIndex;
      out.hasGlobalOutputIndex = true;
    }
    return true;
  }

  TransferResult TransactionBuilder::finalizeAndRelay(cn::ITransaction &tx)
  {
    TransferResult result;
    result.success = false;

    cn::Transaction rawTx;
    if (!cn::fromBinaryArray(rawTx, tx.getTransactionData()))
    {
      result.error = "Failed to serialize transaction";
      return result;
    }

    const TransferResult relayResult = m_relayHandler.relaySync(rawTx);
    if (!relayResult.success)
    {
      result.error = relayResult.error.empty() ? "Failed to relay transaction" : relayResult.error;
      return result;
    }

    result.success = true;
    result.transaction = rawTx;
    result.txHash = common::podToHex(cn::getObjectHash(rawTx));
    return result;
  }

  bool TransactionBuilder::signFusionKeyInputs(cn::ITransaction &tx,
                                             const std::vector<OutputInfo> &selectedOutputs,
                                             uint64_t mixin,
                                             std::string &error) const
  {
    error.clear();
    std::vector<outs_for_amount> mixinResult;
    if (mixin > 0)
    {
      if (!requestMixinOutputs(selectedOutputs, mixin, mixinResult, error))
      {
        if (error.empty())
          error = "Failed to obtain mixin outputs";
        return false;
      }
    }

    std::vector<KeyInputBundle> keyInputs;
    if (!prepareKeyInputs(selectedOutputs, mixinResult, mixin, keyInputs, error))
    {
      if (error.empty())
        error = "Failed to prepare transaction inputs (connect daemon for global output indices, or rescan wallet)";
      return false;
    }

    if (!addKeyInputsAndSign(tx, keyInputs))
    {
      error = "Failed to sign transaction inputs";
      return false;
    }
    return true;
  }

  TransferResult TransactionBuilder::fundFusionKeyInputs(std::unique_ptr<cn::ITransaction> &tx,
                                                       const std::vector<OutputInfo> &selectedOutputs,
                                                       uint64_t mixin)
  {
    TransferResult result;
    result.success = false;

    if (!tx)
    {
      result.error = "Invalid transaction";
      return result;
    }

    std::string signError;
    if (!signFusionKeyInputs(*tx, selectedOutputs, mixin, signError))
    {
      result.error = signError;
      return result;
    }

    result = finalizeAndRelay(*tx);
    if (result.success)
      result.spentInputs = selectedOutputs;
    return result;
  }

  TransferResult TransactionBuilder::fundKeyInputs(std::unique_ptr<cn::ITransaction> &tx,
                                                   const std::vector<OutputInfo> &selectedOutputs,
                                                   uint64_t mixin)
  {
    TransferResult result;
    result.success = false;
    mixin = effectiveMixin(mixin);

    if (!tx)
    {
      result.error = "Invalid transaction";
      return result;
    }

    std::vector<outs_for_amount> mixinResult;
    if (!requestMixinOutputs(selectedOutputs, mixin, mixinResult, result.error))
    {
      if (result.error.empty())
        result.error = "Failed to obtain mixin outputs";
      return result;
    }

    std::vector<KeyInputBundle> keyInputs;
    std::string prepError;
    if (!prepareKeyInputs(selectedOutputs, mixinResult, mixin, keyInputs, prepError))
    {
      result.error = prepError.empty()
                         ? "Failed to prepare transaction inputs (connect daemon for global output indices, or rescan wallet)"
                         : prepError;
      return result;
    }

    if (!addKeyInputsAndSign(*tx, keyInputs))
    {
      result.error = "Failed to sign transaction inputs";
      return result;
    }

    result = finalizeAndRelay(*tx);
    if (result.success)
      result.spentInputs = selectedOutputs;
    return result;
  }

  TransactionBuilder::BuildResult TransactionBuilder::build(
      const std::vector<OutputInfo> &fundingOutputs,
      const BuilderParams &params)
  {
    BuildResult result;
    result.success = false;

    const uint64_t neededMoney = countNeededMoney(params.transfers, params.fee);

    const auto selection = m_outputSelector.select(neededMoney, fundingOutputs);
    if (!selection.enough)
    {
      result.error = "Insufficient funds";
      return result;
    }

    for (const auto &out : selection.outputs)
    {
      if (out.blockHeight == 0 || !out.hasGlobalOutputIndex)
      {
        result.error = "Missing global output index on selected UTXO (wait for sync or rescan wallet)";
        return result;
      }
    }

    const uint64_t foundMoney = selection.totalFound;
    uint64_t changeAmount = foundMoney - neededMoney;

    uint64_t donationAmount = 0;
    if (params.donationThreshold > 0)
    {
      donationAmount = std::min(changeAmount, params.donationThreshold);
      changeAmount -= donationAmount;
    }

    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(params.unlockTime);

    std::vector<cn::AccountPublicAddress> destAddresses;
    destAddresses.reserve(params.transfers.size());
    for (const auto &t : params.transfers)
    {
      cn::AccountPublicAddress destAddr;
      if (!m_currency.parseAccountAddressString(t.address, destAddr))
      {
        result.error = "Invalid destination address: " + t.address;
        return result;
      }
      destAddresses.push_back(destAddr);
    }

    struct AmountToAddress
    {
      const cn::AccountPublicAddress *addr;
      uint64_t amount;
    };
    std::vector<AmountToAddress> outputs;

    for (size_t i = 0; i < params.transfers.size(); ++i)
    {
      std::vector<uint64_t> amounts;
      decomposeAmount(params.transfers[i].amount, m_currency.defaultDustThreshold(), amounts);
      for (auto a : amounts)
        outputs.push_back({&destAddresses[i], a});
    }

    if (changeAmount > 0)
    {
      std::vector<uint64_t> amounts;
      decomposeAmount(changeAmount, m_currency.defaultDustThreshold(), amounts);
      for (auto a : amounts)
        outputs.push_back({&params.changeAddress, a});
    }

    if (donationAmount > 0)
      outputs.push_back({&params.donationAddress, donationAmount});

    std::shuffle(outputs.begin(), outputs.end(),
                 std::default_random_engine{crypto::rand<std::default_random_engine::result_type>()});
    std::sort(outputs.begin(), outputs.end(),
              [](const AmountToAddress &l, const AmountToAddress &r)
              { return l.amount < r.amount; });

    result.plannedOutputs.reserve(outputs.size());
    for (const auto &out : outputs)
      result.plannedOutputs.push_back({*out.addr, out.amount});

    result.transaction = std::move(tx);
    result.selectedOutputs = selection.outputs;
    result.changeAmount = changeAmount;
    result.fee = params.fee;
    result.success = true;
    return result;
  }

  TransferResult TransactionBuilder::buildTransfer(const std::vector<OutputInfo> &fundingOutputs,
                                                 const BuilderParams &params)
  {
    TransferResult result;
    result.success = false;

    BuildResult buildResult = build(fundingOutputs, params);
    if (!buildResult.success)
    {
      result.error = buildResult.error;
      return result;
    }

    std::unique_ptr<cn::ITransaction> tx = std::move(buildResult.transaction);
    const uint64_t mixin = effectiveMixin(params.mixin);

    std::vector<outs_for_amount> mixinResult;
    if (!requestMixinOutputs(buildResult.selectedOutputs, mixin, mixinResult, result.error))
    {
      if (result.error.empty())
        result.error = "Failed to obtain mixin outputs";
      return result;
    }

    std::vector<KeyInputBundle> keyInputs;
    std::string prepError;
    if (!prepareKeyInputs(buildResult.selectedOutputs, mixinResult, mixin, keyInputs, prepError))
    {
      result.error = prepError.empty()
                         ? "Failed to prepare transaction inputs (connect daemon for global output indices, or rescan wallet)"
                         : prepError;
      return result;
    }

    // WalletGreen::makeTransaction order: inputs, deterministic tx key, outputs, sign.
    for (auto &input : keyInputs)
      tx->addInput(m_accountKeys, input.keyInfo, input.ephKeys);

    tx->setDeterministicTransactionSecretKey(m_signatureBuilder.deriveEphemeralSecretKey(
        params.mainAddress.viewPublicKey, 0));

    applyPlannedOutputs(*tx, buildResult.plannedOutputs);
    appendTransferExtras(*tx, params);

    size_t inputIndex = 0;
    for (const auto &input : keyInputs)
      tx->signInputKey(inputIndex++, input.keyInfo, input.ephKeys);

    result = finalizeAndRelay(*tx);
    if (result.success)
    {
      result.fee = buildResult.fee;
      result.spentInputs = buildResult.selectedOutputs;
    }
    return result;
  }
}
