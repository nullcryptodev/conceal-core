// FusionScreen - optimize (fusion) small UTXOs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Screen.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace BoltCore
{
  class Wallet;
  struct FusionEstimate;
}
namespace cn
{
  class Currency;
}

namespace ClientWallet
{

  class FusionScreen : public Screen
  {
  public:
    FusionScreen(std::shared_ptr<BoltCore::Wallet> wallet, const cn::Currency &currency);

    void onEnter() override;
    void onKey(int key) override;
    void render(Tui::ScreenBuffer &buf) override;
    std::string title() const override { return "Optimize (Fusion)"; }

  private:
    enum class State
    {
      SelectThreshold,
      SelectMixin,
      Preview,
      Confirm,
      Fusing,
      Done,
      Error
    };

    void refreshEstimate();
    void doFusion();
    static std::vector<uint64_t> buildThresholdList(const cn::Currency &currency);
    static std::vector<uint64_t> buildMixinList();

    std::shared_ptr<BoltCore::Wallet> m_wallet;
    const cn::Currency &m_currency;

    State m_state = State::SelectThreshold;
    std::vector<uint64_t> m_thresholds;
    std::vector<uint64_t> m_mixins;
    int m_thresholdIndex = 0;
    int m_mixinIndex = 0;

    BoltCore::FusionEstimate m_estimate{};
    std::string m_error;
    std::string m_txHash;
    std::chrono::steady_clock::time_point m_taskStarted{};
  };

} // namespace ClientWallet
