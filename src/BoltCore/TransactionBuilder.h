// TransactionBuilder - builds, signs, and relays transactions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "ITransaction.h"
#include "CryptoNoteCore/Account.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "crypto/crypto.h"
#include <memory>
#include <vector>

namespace cn
{
  class ITransaction;
  class Currency;
  class INode;
  struct AccountPublicAddress;
  struct AccountKeys;
}

namespace BoltCore
{
  class OutputSelector;
  class SignatureBuilder;
  class RelayHandler;

  struct BuilderParams
  {
    std::vector<Transfer> transfers;
    uint64_t fee;
    uint64_t mixin;
    std::string extra;
    uint64_t unlockTime;
    uint64_t ttl;
    cn::AccountPublicAddress changeAddress;
    cn::AccountPublicAddress donationAddress;
    uint64_t donationThreshold;
    cn::AccountPublicAddress mainAddress;
  };

  class TransactionBuilder
  {
  public:
    TransactionBuilder(const cn::Currency &currency,
                       cn::INode &node,
                       OutputSelector &outputSelector,
                       SignatureBuilder &signatureBuilder,
                       RelayHandler &relayHandler,
                       const cn::AccountKeys &accountKeys);

    struct PlannedOutput
    {
      cn::AccountPublicAddress address;
      uint64_t amount;
    };

    struct BuildResult
    {
      std::unique_ptr<cn::ITransaction> transaction;
      std::vector<OutputInfo> selectedOutputs;
      std::vector<PlannedOutput> plannedOutputs;
      uint64_t changeAmount;
      uint64_t fee;
      bool success;
      std::string error;
    };

    // Build unsigned transfer outputs and select funding inputs.
    BuildResult build(const std::vector<OutputInfo> &fundingOutputs,
                      const BuilderParams &params);

    // Complete signed transfer: build + fund key inputs + relay.
    TransferResult buildTransfer(const std::vector<OutputInfo> &fundingOutputs,
                                 const BuilderParams &params);

    // Add mixin key inputs, sign, and relay an existing transaction shell.
    TransferResult fundKeyInputs(std::unique_ptr<cn::ITransaction> &tx,
                                 const std::vector<OutputInfo> &selectedOutputs,
                                 uint64_t mixin);

    // Fusion path: mixin 0 means anonymity 1 (no effectiveMixin remapping).
    TransferResult fundFusionKeyInputs(std::unique_ptr<cn::ITransaction> &tx,
                                       const std::vector<OutputInfo> &selectedOutputs,
                                       uint64_t mixin);

    // Sign fusion inputs without relaying (for size-trim loop).
    bool signFusionKeyInputs(cn::ITransaction &tx,
                             const std::vector<OutputInfo> &selectedOutputs,
                             uint64_t mixin,
                             std::string &error) const;

    // Serialize and relay a fully signed transaction.
    TransferResult finalizeAndRelay(cn::ITransaction &tx);

    // Resolve missing global output indices on selected key inputs (same requirement as transfer).
    bool ensureGlobalOutputIndices(std::vector<OutputInfo> &outputs, std::string &error) const;

    // Daemon RPC lookup when MDBX / stored index is unavailable.
    bool resolveGlobalOutputIndex(const OutputInfo &output, uint32_t &globalOutputIndex,
                                  std::string *errorOut = nullptr) const;

    const cn::AccountKeys &accountKeys() const { return m_accountKeys; }

  private:
    struct KeyInputBundle
    {
      cn::transaction_types::InputKeyInfo keyInfo;
      cn::KeyPair ephKeys;
    };

    bool requestMixinOutputs(const std::vector<OutputInfo> &selectedOutputs,
                             uint64_t mixin,
                             std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &mixinResult,
                             std::string &error) const;

    bool prepareKeyInputs(const std::vector<OutputInfo> &selectedOutputs,
                          std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &mixinResult,
                          uint64_t mixin,
                          std::vector<KeyInputBundle> &keyInputs,
                          std::string &error) const;

    bool addKeyInputsAndSign(cn::ITransaction &tx,
                             std::vector<KeyInputBundle> &keyInputs) const;

    void applyPlannedOutputs(cn::ITransaction &tx,
                             const std::vector<PlannedOutput> &plannedOutputs) const;

    void appendTransferExtras(cn::ITransaction &tx, const BuilderParams &params) const;

    const cn::Currency &m_currency;
    cn::INode &m_node;
    OutputSelector &m_outputSelector;
    SignatureBuilder &m_signatureBuilder;
    RelayHandler &m_relayHandler;
    cn::AccountKeys m_accountKeys;
  };
}
