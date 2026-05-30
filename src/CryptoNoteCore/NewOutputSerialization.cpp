// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "NewOutputSerialization.h"

#include <stdexcept>

#include "CryptoNoteSerialization.h"
#include "Serialization/SerializationOverloads.h"

namespace cn
{

  void serialize(StandardPaymentOutput &out, ISerializer &serializer)
  {
    serializer(out.view_tag, "view_tag");
    serializer(out.key_index, "key_index");
    serializer(out.memo_size, "memo_size");

    if (out.memo_size > 0)
    {
      serializer(out.encrypted_memo, "encrypted_memo");
    }

    serializer(out.key, "key");
  }

  void serialize(MultisigPaymentOutput &out, ISerializer &serializer)
  {
    serializer(out.view_tag, "view_tag");
    serializer(out.key_index, "key_index");
    serializer(out.num_keys, "num_keys");
    serializer(out.flags, "flags");
    serializer(out.term, "term");
    serializer(out.memo_size, "memo_size");

    if (out.memo_size > 0)
    {
      serializer(out.encrypted_memo, "encrypted_memo");
    }

    serializer(out.keys, "keys");

    // Validate consistency
    if (serializer.type() == ISerializer::INPUT)
    {
      if (out.keys.size() != out.num_keys)
      {
        throw std::runtime_error("MultisigPaymentOutput: num_keys does not match keys size");
      }
    }
  }

  void serialize(DomainRegistrationOutput &out, ISerializer &serializer)
  {
    serializer(out.view_tag, "view_tag");
    serializer(out.key_index, "key_index");
    serializer(out.domain_len, "domain_len");
    serializer(out.domain, "domain");
    serializer(out.tier, "tier");
    serializer(out.domain_pub, "domain_pub");
    serializer(out.domain_view_pub, "domain_view_pub");
    serializer(out.encrypted_addr_size, "encrypted_addr_size");

    // Serialize encrypted_addr as binary blob (fixed-size array)
    serializer.binary(out.encrypted_addr.data(), out.encrypted_addr.size(), "encrypted_addr");

    serializer(out.metadata_len, "metadata_len");

    if (out.metadata_len > 0)
    {
      serializer(out.metadata, "metadata");
    }
  }

  void serialize(DomainDeletionOutput &out, ISerializer &serializer)
  {
    serializer(out.view_tag, "view_tag");
    serializer(out.key_index, "key_index");
    serializer(out.domain_len, "domain_len");
    serializer(out.domain, "domain");
    serializer(out.sig, "sig");
  }

} // namespace cn