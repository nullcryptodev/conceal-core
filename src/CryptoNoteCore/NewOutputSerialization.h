// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "NewOutputTypes.h"
#include "Serialization/ISerializer.h"

namespace cn
{

  void serialize(StandardPaymentOutput &out, ISerializer &serializer);
  void serialize(MultisigPaymentOutput &out, ISerializer &serializer);
  void serialize(DomainRegistrationOutput &out, ISerializer &serializer);
  void serialize(DomainDeletionOutput &out, ISerializer &serializer);

} // namespace cn