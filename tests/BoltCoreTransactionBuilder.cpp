// BoltCore TransactionBuilder unit tests
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <gtest/gtest.h>

#include <cstring>
#include <unordered_map>

#include "BoltCore/OutputSelector.h"
#include "BoltCore/OutputUtils.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "BoltCore/RelayHandler.h"
#include "BoltCore/SignatureBuilder.h"
#include "BoltCore/TransactionBuilder.h"
#include "BoltCore/DepositManager.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteConfig.h"
#include "Logging/ConsoleLogger.h"
#include "INode.h"
#include "CryptoNote.h"

namespace
{
  logging::ConsoleLogger g_logger;

  cn::Currency makeCurrency()
  {
    cn::CurrencyBuilder builder(g_logger);
    return builder.currency();
  }

  cn::AccountKeys makeAccountKeys()
  {
    cn::AccountKeys keys;
    crypto::generate_keys(keys.address.spendPublicKey, keys.spendSecretKey);
    crypto::generate_keys(keys.address.viewPublicKey, keys.viewSecretKey);
    return keys;
  }

  BoltCore::OutputInfo makeKeyOutput(uint64_t amount, uint32_t blockHeight = 1000,
                                     bool spent = false, bool isDeposit = false)
  {
    BoltCore::OutputInfo out = {};
    out.blockHeight = blockHeight;
    out.amount = amount;
    out.outputIndex = 0;
    out.spent = spent;
    out.isDeposit = isDeposit;
    out.term = 0;
    std::memset(&out.txHash, 0, sizeof(out.txHash));
    out.txHash.data[0] = static_cast<uint8_t>(amount & 0xFF);
    crypto::SecretKey sk;
    crypto::generate_keys(out.outputKey, sk);
    crypto::generate_key_image(out.outputKey, sk, out.keyImage);
    crypto::generate_keys(out.txPublicKey, sk);
    return out;
  }

  void setStoredGlobalIndex(BoltCore::OutputInfo &out, uint32_t globalIndex)
  {
    out.globalOutputIndex = globalIndex;
    out.hasGlobalOutputIndex = true;
  }

  class TransactionBuilderNodeStub : public cn::INode
  {
  public:
    std::unordered_map<crypto::Hash, std::vector<uint32_t>> globalIndices;
    std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> mixinOuts;
    uint64_t mixinCount = cn::parameters::MINIMUM_MIXIN;
    uint64_t lastRandomOutsCount = 0;
    size_t randomOutsPerAmount = 0;
    bool relaySuccess = true;

    bool addObserver(cn::INodeObserver *) override { return true; }
    bool removeObserver(cn::INodeObserver *) override { return true; }
    void init(const Callback &callback) override { callback(std::error_code()); }
    bool shutdown() override { return true; }
    size_t getPeerCount() const override { return 0; }
    uint32_t getLastLocalBlockHeight() const override { return 0; }
    uint32_t getLastKnownBlockHeight() const override { return 0; }
    uint32_t getLocalBlockCount() const override { return 0; }
    uint32_t getKnownBlockCount() const override { return 0; }
    uint64_t getLastLocalBlockTimestamp() const override { return 0; }

    void relayTransaction(const cn::Transaction &, const Callback &callback) override
    {
      callback(relaySuccess ? std::error_code() : std::make_error_code(std::errc::io_error));
    }

    void getRandomOutsByAmounts(std::vector<uint64_t> &&amounts, uint64_t outsCount,
                                std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &result,
                                const Callback &callback) override
    {
      lastRandomOutsCount = outsCount;
      randomOutsPerAmount = static_cast<size_t>(outsCount);
      result.clear();
      for (auto amount : amounts)
      {
        cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount entry;
        entry.amount = amount;
        if (!mixinOuts.empty())
          entry = mixinOuts.front();
        else
        {
          const size_t count = randomOutsPerAmount > 0 ? randomOutsPerAmount : static_cast<size_t>(outsCount);
          for (size_t i = 0; i < count; ++i)
          {
            cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry fake;
            fake.global_amount_index = 100 + static_cast<uint64_t>(i);
            crypto::SecretKey sk;
            crypto::generate_keys(fake.out_key, sk);
            entry.outs.push_back(fake);
          }
        }
        result.push_back(entry);
      }
      callback(std::error_code());
    }

    void getTransactionOutsGlobalIndices(const crypto::Hash &transactionHash,
                                           std::vector<uint32_t> &outsGlobalIndices,
                                           const Callback &callback) override
    {
      const auto it = globalIndices.find(transactionHash);
      if (it == globalIndices.end())
      {
        callback(std::make_error_code(std::errc::invalid_argument));
        return;
      }
      outsGlobalIndices = it->second;
      callback(std::error_code());
    }

    void getNewBlocks(std::vector<crypto::Hash> &&, std::vector<cn::block_complete_entry> &, uint32_t &,
                      const Callback &callback) override
    {
      callback(std::error_code());
    }
    void queryBlocks(std::vector<crypto::Hash> &&, uint64_t, std::vector<cn::BlockShortEntry> &, uint32_t &,
                     const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getPoolSymmetricDifference(std::vector<crypto::Hash> &&, crypto::Hash, bool &isBcActual,
                                    std::vector<std::unique_ptr<cn::ITransactionReader>> &,
                                    std::vector<crypto::Hash> &, const Callback &callback) override
    {
      isBcActual = true;
      callback(std::error_code());
    }
    void getMultisignatureOutputByGlobalIndex(uint64_t, uint32_t, cn::MultisignatureOutput &,
                                              const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getTransaction(const crypto::Hash &, cn::Transaction &, const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getBlocks(const std::vector<uint32_t> &, std::vector<std::vector<cn::BlockDetails>> &,
                   const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getBlocks(const std::vector<crypto::Hash> &, std::vector<cn::BlockDetails> &,
                   const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getBlocks(uint64_t, uint64_t, uint32_t, std::vector<cn::BlockDetails> &, uint32_t &,
                   const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getTransactions(const std::vector<crypto::Hash> &, std::vector<cn::TransactionDetails> &,
                         const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getTransactionsByPaymentId(const crypto::Hash &, std::vector<cn::TransactionDetails> &,
                                    const Callback &callback) override
    {
      callback(std::error_code());
    }
    void getPoolTransactions(uint64_t, uint64_t, uint32_t, std::vector<cn::TransactionDetails> &, uint64_t &,
                           const Callback &callback) override
    {
      callback(std::error_code());
    }
    void isSynchronized(bool &, const Callback &callback) override { callback(std::error_code()); }
    std::vector<crypto::Hash> getPoolTransactions() override { return {}; }
    bool getTransactionSync(const crypto::Hash &, cn::Transaction &) override { return false; }
  };
}

TEST(BoltCoreFundingOutput, ExcludesDepositDustAndLockedCoinbase)
{
  const cn::Currency currency = makeCurrency();
  const uint32_t height = 1000;
  const uint32_t unlockWindow = currency.minedMoneyUnlockWindow();
  const uint64_t dust = currency.defaultDustThreshold();

  auto spendable = makeKeyOutput(100 * cn::parameters::COIN);
  auto dustOut = makeKeyOutput(dust);
  auto deposit = makeKeyOutput(50 * cn::parameters::COIN);
  deposit.isDeposit = true;
  deposit.term = 1000;
  auto lockedCoinbase = makeKeyOutput(10 * cn::parameters::COIN, height);
  lockedCoinbase.blockHeight = height;

  EXPECT_TRUE(BoltCore::isFundingKeyOutput(spendable, height + unlockWindow, unlockWindow, dust));
  EXPECT_FALSE(BoltCore::isFundingKeyOutput(dustOut, height + unlockWindow, unlockWindow, dust));
  EXPECT_FALSE(BoltCore::isFundingKeyOutput(deposit, height + unlockWindow, unlockWindow, dust));
  EXPECT_FALSE(BoltCore::isFundingKeyOutput(lockedCoinbase, height, unlockWindow, dust));
}

TEST(BoltCoreOutputSelector, ExpectsPrefilteredFundingList)
{
  const cn::Currency currency = makeCurrency();
  BoltCore::OutputSelector selector(currency);
  const uint64_t dust = currency.defaultDustThreshold();

  auto good = makeKeyOutput(10 * cn::parameters::COIN);
  auto dustOut = makeKeyOutput(dust);
  auto deposit = makeKeyOutput(5 * cn::parameters::COIN);
  deposit.isDeposit = true;

  const auto result = selector.select(5 * cn::parameters::COIN, {good, dustOut, deposit});
  EXPECT_TRUE(result.enough);
  ASSERT_EQ(result.outputs.size(), 1u);
  EXPECT_EQ(result.outputs.front().amount, good.amount);
}

TEST(BoltCoreTransactionBuilder, ResolveGlobalOutputIndex)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder builder(currency, node, selector, sigBuilder, relay, keys);

  auto out = makeKeyOutput(10 * cn::parameters::COIN);
  out.outputIndex = 1;
  node.globalIndices[out.txHash] = {11, 42, 99};

  uint32_t globalIndex = 0;
  EXPECT_TRUE(builder.resolveGlobalOutputIndex(out, globalIndex));
  EXPECT_EQ(globalIndex, 42u);
}

TEST(BoltCoreTransactionBuilder, OutputSigningIndexMatchesWalletGreen)
{
  BoltCore::OutputInfo legacyKeyOut;
  legacyKeyOut.outputIndex = 3;
  legacyKeyOut.hasKeyDerivationIndex = false;
  EXPECT_EQ(BoltCore::outputSigningIndex(legacyKeyOut), 3u);

  BoltCore::OutputInfo v3Out;
  v3Out.outputIndex = 1;
  v3Out.hasKeyDerivationIndex = true;
  v3Out.keyDerivationIndex = 4;
  EXPECT_EQ(BoltCore::outputSigningIndex(v3Out), 4u);
}

TEST(BoltCoreTransactionBuilder, PrepareKeyInputsUsesTxOutputIndexForKeyOutput)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder builder(currency, node, selector, sigBuilder, relay, keys);

  cn::KeyPair txKeys;
  crypto::generate_keys(txKeys.publicKey, txKeys.secretKey);

  cn::KeyPair eph;
  crypto::KeyImage ki;
  ASSERT_TRUE(cn::generate_key_image_helper(keys, txKeys.publicKey, 2, eph, ki));

  auto funding = makeKeyOutput(10 * cn::parameters::COIN);
  funding.outputIndex = 2;
  funding.txPublicKey = txKeys.publicKey;
  funding.outputKey = eph.publicKey;
  funding.hasGlobalOutputIndex = true;
  funding.globalOutputIndex = 500;
  node.globalIndices[funding.txHash] = {100, 200, 500};

  cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount mixinEntry;
  mixinEntry.amount = funding.amount;
  for (size_t i = 0; i < cn::parameters::MINIMUM_MIXIN + 1; ++i)
  {
    cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry fake;
    fake.global_amount_index = 600 + i;
    crypto::SecretKey sk;
    crypto::generate_keys(fake.out_key, sk);
    mixinEntry.outs.push_back(fake);
  }
  node.mixinOuts = {mixinEntry};

  std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
  tx->addOutput(1 * cn::parameters::COIN, keys.address);

  const auto result = builder.fundKeyInputs(tx, {funding}, cn::parameters::MINIMUM_MIXIN);
  ASSERT_TRUE(result.success) << result.error;

  crypto::KeyImage expectedKi;
  cn::KeyPair verifyEph;
  ASSERT_TRUE(cn::generate_key_image_helper(keys, txKeys.publicKey, 2, verifyEph, expectedKi));
  const cn::KeyInput &keyInput = boost::get<cn::KeyInput>(result.transaction.inputs.front());
  EXPECT_EQ(keyInput.keyImage, expectedKi);
}

TEST(BoltCoreTransactionBuilder, RequestMixinUsesPlusOneOutCount)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder builder(currency, node, selector, sigBuilder, relay, keys);

  auto funding = makeKeyOutput(10 * cn::parameters::COIN);
  funding.outputIndex = 0;
  setStoredGlobalIndex(funding, 500);
  node.globalIndices[funding.txHash] = {500};

  std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
  tx->addOutput(1 * cn::parameters::COIN, keys.address);

  const auto result = builder.fundKeyInputs(tx, {funding}, 0);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(node.lastRandomOutsCount, cn::parameters::MINIMUM_MIXIN + 1);
}

TEST(BoltCoreTransactionBuilder, FundKeyInputsWithMultipleInputsBeforeSign)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder builder(currency, node, selector, sigBuilder, relay, keys);

  auto fundingA = makeKeyOutput(8 * cn::parameters::COIN);
  fundingA.outputIndex = 0;
  fundingA.txHash.data[1] = 1;
  setStoredGlobalIndex(fundingA, 500);
  node.globalIndices[fundingA.txHash] = {500};

  auto fundingB = makeKeyOutput(8 * cn::parameters::COIN);
  fundingB.outputIndex = 0;
  fundingB.txHash.data[1] = 2;
  setStoredGlobalIndex(fundingB, 600);
  node.globalIndices[fundingB.txHash] = {600};

  std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
  // Deposit-like shell: multisig output first, then change.
  tx->addOutput(10 * cn::parameters::COIN, std::vector<cn::AccountPublicAddress>{keys.address}, 1, 100000);
  tx->addOutput(1 * cn::parameters::COIN, keys.address);

  const auto result = builder.fundKeyInputs(tx, {fundingA, fundingB}, 0);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.transaction.inputs.size(), 2u);
  EXPECT_EQ(result.transaction.signatures.size(), 2u);
}

TEST(BoltCoreTransactionBuilder, PrepareKeyInputsRejectsShortMixinRing)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder builder(currency, node, selector, sigBuilder, relay, keys);

  auto funding = makeKeyOutput(10 * cn::parameters::COIN);
  funding.outputIndex = 0;
  setStoredGlobalIndex(funding, 500);
  node.globalIndices[funding.txHash] = {500};

  cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount mixinEntry;
  mixinEntry.amount = funding.amount;
  for (size_t i = 0; i < cn::parameters::MINIMUM_MIXIN + 1; ++i)
  {
    cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry fake;
    fake.global_amount_index = (i < 3) ? 500 : (600 + i);
    crypto::SecretKey sk;
    crypto::generate_keys(fake.out_key, sk);
    mixinEntry.outs.push_back(fake);
  }
  node.mixinOuts = {mixinEntry};

  std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
  tx->addOutput(1 * cn::parameters::COIN, keys.address);

  const auto result = builder.fundKeyInputs(tx, {funding}, cn::parameters::MINIMUM_MIXIN);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Not enough mixin outputs"), std::string::npos);
}

TEST(BoltCoreTransactionBuilder, BuildTransferUsesMixinRing)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder builder(currency, node, selector, sigBuilder, relay, keys);

  auto funding = makeKeyOutput(20 * cn::parameters::COIN);
  funding.outputIndex = 0;
  setStoredGlobalIndex(funding, 500);
  node.globalIndices[funding.txHash] = {500};

  const std::string destAddress = currency.accountAddressAsString(keys.address);

  BoltCore::BuilderParams params;
  params.transfers = {{destAddress, 5 * cn::parameters::COIN}};
  params.fee = currency.minimumFee();
  params.mixin = cn::parameters::MINIMUM_MIXIN;
  params.changeAddress = keys.address;
  params.donationAddress = keys.address;
  params.mainAddress = keys.address;

  const auto result = builder.buildTransfer({funding}, params);
  ASSERT_TRUE(result.success) << result.error;

  ASSERT_FALSE(result.transaction.inputs.empty());
  const cn::KeyInput *keyInput = boost::get<cn::KeyInput>(&result.transaction.inputs.front());
  ASSERT_NE(keyInput, nullptr);
  EXPECT_EQ(keyInput->outputIndexes.size(), cn::parameters::MINIMUM_MIXIN + 1);
}

TEST(BoltCoreDepositManager, WithdrawRejectsLockedDeposit)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder txBuilder(currency, node, selector, sigBuilder, relay, keys);
  BoltCore::DepositManager depositManager(currency, txBuilder, sigBuilder, selector, keys.address);

  auto depositOutput = makeKeyOutput(10 * cn::parameters::COIN);
  depositOutput.isDeposit = true;
  depositOutput.term = 10000;
  depositOutput.blockHeight = 1000;

  BoltCore::DepositInfo deposit = {};
  deposit.amount = depositOutput.amount;
  deposit.term = depositOutput.term;
  deposit.interest = currency.calculateInterest(deposit.amount, deposit.term, depositOutput.blockHeight);
  deposit.locked = true;

  const auto result = depositManager.withdrawDeposit(depositOutput, deposit);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "Deposit is still locked");
}

TEST(BoltCoreDepositManager, WithdrawUsesGlobalOutputIndex)
{
  const cn::Currency currency = makeCurrency();
  TransactionBuilderNodeStub node;
  BoltCore::OutputSelector selector(currency);
  const cn::AccountKeys keys = makeAccountKeys();
  BoltCore::SignatureBuilder sigBuilder(keys.viewSecretKey, keys.spendSecretKey,
                                        keys.address.viewPublicKey, keys.address.spendPublicKey);
  BoltCore::RelayHandler relay(node);
  BoltCore::TransactionBuilder txBuilder(currency, node, selector, sigBuilder, relay, keys);
  BoltCore::DepositManager depositManager(currency, txBuilder, sigBuilder, selector, keys.address);

  auto depositOutput = makeKeyOutput(10 * cn::parameters::COIN);
  depositOutput.isDeposit = true;
  depositOutput.term = 100;
  depositOutput.blockHeight = 1000;
  depositOutput.outputIndex = 0;
  setStoredGlobalIndex(depositOutput, 777);
  node.globalIndices[depositOutput.txHash] = {777};

  BoltCore::DepositInfo deposit = {};
  deposit.amount = depositOutput.amount;
  deposit.term = depositOutput.term;
  deposit.interest = currency.calculateInterest(deposit.amount, deposit.term, depositOutput.blockHeight);
  deposit.locked = false;

  const auto result = depositManager.withdrawDeposit(depositOutput, deposit);
  ASSERT_TRUE(result.success) << result.error;

  ASSERT_FALSE(result.transaction.inputs.empty());
  const cn::MultisignatureInput *msInput =
      boost::get<cn::MultisignatureInput>(&result.transaction.inputs.front());
  ASSERT_NE(msInput, nullptr);
  EXPECT_EQ(msInput->outputIndex, 777u);
  EXPECT_EQ(msInput->amount, deposit.amount);

  const uint64_t fee = currency.minimumFeeV2();
  const uint64_t expectedOut = deposit.amount + deposit.interest - fee;
  EXPECT_EQ(result.fee, fee);
  uint64_t outputTotal = 0;
  for (const auto &output : result.transaction.outputs)
    outputTotal += output.amount;
  EXPECT_EQ(outputTotal, expectedOut);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
