// BoltCore BalanceTracker unit tests
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <gtest/gtest.h>

#include <cstring>

#include "BoltCore/BalanceTracker.h"
#include "BoltCore/OutputUtils.h"
#include "crypto/crypto.h"

namespace
{
  BoltCore::OutputInfo makeKeyOutput(uint64_t amount, bool spent = false)
  {
    BoltCore::OutputInfo out = {};
    out.blockHeight = 100;
    out.amount = amount;
    out.outputIndex = 0;
    out.spent = spent;
    out.isDeposit = false;
    out.term = 0;
    std::memset(&out.txHash, 0, sizeof(out.txHash));
    crypto::SecretKey sk;
    crypto::generate_keys(out.outputKey, sk);
    crypto::generate_key_image(out.outputKey, sk, out.keyImage);
    return out;
  }

  BoltCore::OutputInfo makeDepositOutput(uint64_t amount, uint32_t term, uint32_t blockHeight)
  {
    BoltCore::OutputInfo out = {};
    out.blockHeight = blockHeight;
    out.amount = amount;
    out.outputIndex = 1;
    out.spent = false;
    out.isDeposit = true;
    out.term = term;
    std::memset(&out.txHash, 0, sizeof(out.txHash));
    return out;
  }
}

TEST(BoltCoreBalanceTracker, DepositOutputExcludedFromSpendablePool)
{
  auto keyOut = makeKeyOutput(1000000);
  auto depOut = makeDepositOutput(5000000, 1000, 100);

  EXPECT_TRUE(BoltCore::isSpendableKeyOutput(keyOut, 200));
  EXPECT_FALSE(BoltCore::isSpendableKeyOutput(depOut, 200));
}

TEST(BoltCoreBalanceTracker, MarkSpentRegularOutput)
{
  BoltCore::BalanceTracker tracker;
  auto out = makeKeyOutput(3000000);
  tracker.loadOutputs({out}, 200);

  EXPECT_EQ(tracker.getTotalBalance().actual, 3000000u);
  EXPECT_TRUE(tracker.markSpent(out.keyImage));
  EXPECT_EQ(tracker.getTotalBalance().actual, 0u);
}

TEST(BoltCoreBalanceTracker, MarkSpentLockedDeposit)
{
  BoltCore::BalanceTracker tracker;
  auto dep = makeDepositOutput(2000000, 1000, 100);
  tracker.loadOutputs({dep}, 200);

  EXPECT_EQ(tracker.getTotalBalance().lockedDeposit, 2000000u);
  EXPECT_TRUE(tracker.markDepositSpent(dep.txHash, dep.outputIndex));
  EXPECT_EQ(tracker.getTotalBalance().lockedDeposit, 0u);
}

TEST(BoltCoreBalanceTracker, MarkSpentUnlockedDeposit)
{
  BoltCore::BalanceTracker tracker;
  auto dep = makeDepositOutput(1500000, 100, 100);
  tracker.loadOutputs({dep}, 250);

  EXPECT_EQ(tracker.getTotalBalance().unlockedDeposit, 1500000u);
  EXPECT_TRUE(tracker.markDepositSpent(dep.txHash, dep.outputIndex));
  EXPECT_EQ(tracker.getTotalBalance().unlockedDeposit, 0u);
}

TEST(BoltCoreBalanceTracker, MarkSpentIsIdempotent)
{
  BoltCore::BalanceTracker tracker;
  auto out = makeKeyOutput(1000000);
  tracker.loadOutputs({out}, 200);

  EXPECT_TRUE(tracker.markSpent(out.keyImage));
  EXPECT_FALSE(tracker.markSpent(out.keyImage));
  EXPECT_EQ(tracker.getTotalBalance().actual, 0u);
}

TEST(BoltCoreBalanceTracker, HeightRefreshMovesDepositToUnlocked)
{
  BoltCore::BalanceTracker tracker;
  auto dep = makeDepositOutput(4000000, 100, 100);
  tracker.loadOutputs({dep}, 150);

  EXPECT_EQ(tracker.getTotalBalance().lockedDeposit, 4000000u);
  EXPECT_EQ(tracker.getTotalBalance().unlockedDeposit, 0u);

  tracker.setCurrentHeight(201);

  EXPECT_EQ(tracker.getTotalBalance().lockedDeposit, 0u);
  EXPECT_EQ(tracker.getTotalBalance().unlockedDeposit, 4000000u);
}

TEST(BoltCoreBalanceTracker, AddOutputDedup)
{
  BoltCore::BalanceTracker tracker;
  auto out = makeKeyOutput(500000);
  out.txHash.data[0] = 42;
  tracker.loadOutputs({out}, 100);

  tracker.addOutput(out);
  EXPECT_EQ(tracker.getTotalBalance().actual, 500000u);
  EXPECT_EQ(tracker.getOutputs().size(), 1u);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
