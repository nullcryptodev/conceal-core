// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>

#include "CryptoNoteCore/DomainIndex.h"

using namespace cn;

TEST(DomainIndex, empty_index)
{
  DomainIndex index;
  EXPECT_EQ(index.size(), 0u);
  EXPECT_TRUE(index.resolve("anything.conceal") == NULL);
  EXPECT_FALSE(index.isRegistered("anything.conceal"));
}

TEST(DomainIndex, resolve_nonexistent)
{
  DomainIndex index;

  const DomainIndex::DomainEntry *result = index.resolve("nonexistent.conceal");
  EXPECT_TRUE(result == NULL);
}

TEST(DomainIndex, serialization_empty)
{
  DomainIndex index;
  std::vector<uint8_t> data = index.serialize();

  DomainIndex restored;
  EXPECT_TRUE(restored.deserialize(data));
  EXPECT_EQ(restored.size(), 0u);
}

TEST(DomainIndex, clear)
{
  DomainIndex index;
  EXPECT_EQ(index.size(), 0u);

  index.clear();
  EXPECT_EQ(index.size(), 0u);
}