// FusionScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "FusionScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteConfig.h"
#include <chrono>

namespace ClientWallet
{
  namespace
  {
    bool adjustPickerIndex(int key, int &index, int count)
    {
      if (key == Tui::KEY_UP && index < count - 1)
      {
        ++index;
        return true;
      }
      if (key == Tui::KEY_DOWN && index > 0)
      {
        --index;
        return true;
      }
      return false;
    }

    bool isBackKey(int key)
    {
      return key == Tui::KEY_BACKSPACE || key == Tui::KEY_DEL;
    }

    bool isActivateKey(int key)
    {
      return key == Tui::KEY_ENTER || key == Tui::KEY_LF;
    }

    bool fusionIsPossible(const BoltCore::FusionEstimate &estimate)
    {
      return estimate.readyBucketCount > 0;
    }

    bool fusionIsRecommended(const cn::Currency &currency,
                             const BoltCore::FusionEstimate &estimate)
    {
      return estimate.totalOutputCount >= currency.fusionOptimizationMinUnspentCount() &&
             estimate.fusionReadyCount > currency.fusionOptimizationReadyCount();
    }

    std::string fusionAdvisory(const cn::Currency &currency,
                               const BoltCore::FusionEstimate &estimate)
    {
      if (!fusionIsPossible(estimate) || fusionIsRecommended(currency, estimate))
        return {};

      if (estimate.totalOutputCount < currency.fusionOptimizationMinUnspentCount())
        return "Fusion is possible but usually not essential with fewer than " +
               std::to_string(currency.fusionOptimizationMinUnspentCount()) + " UTXOs";

      return "Fusion is possible but optional — eligible outputs below " +
             std::to_string(currency.fusionOptimizationReadyCount() + 1) +
             " (WalletGreen auto-optimize threshold)";
    }
  }

  FusionScreen::FusionScreen(std::shared_ptr<BoltCore::Wallet> wallet, const cn::Currency &currency)
      : m_wallet(std::move(wallet)), m_currency(currency) {}

  std::vector<uint64_t> FusionScreen::buildThresholdList(const cn::Currency &currency)
  {
    std::vector<uint64_t> list;
    list.push_back(currency.fusionStartingThreshold());
    for (uint64_t v = currency.fusionStartingThreshold() * 10;
         v <= cn::parameters::FUSION_THRESHOLD_LIST_MAX; v *= 10)
      list.push_back(v);
    return list;
  }

  std::vector<uint64_t> FusionScreen::buildMixinList()
  {
    std::vector<uint64_t> list;
    list.reserve(cn::parameters::MINIMUM_MIXIN + 1);
    for (uint64_t v = 0; v <= cn::parameters::MINIMUM_MIXIN; ++v)
      list.push_back(v);
    return list;
  }

  void FusionScreen::onEnter()
  {
    m_state = State::SelectThreshold;
    m_thresholds = buildThresholdList(m_currency);
    m_mixins = buildMixinList();
    m_thresholdIndex = 0;
    m_mixinIndex = static_cast<int>(m_mixins.size()) - 1;
    m_estimate = {};
    m_error.clear();
    m_txHash.clear();
  }

  void FusionScreen::refreshEstimate()
  {
    if (m_thresholdIndex < 0 || m_thresholdIndex >= static_cast<int>(m_thresholds.size()) ||
        m_mixinIndex < 0 || m_mixinIndex >= static_cast<int>(m_mixins.size()))
      return;

    m_estimate = m_wallet->estimateFusion(m_thresholds[static_cast<size_t>(m_thresholdIndex)],
                                          m_mixins[static_cast<size_t>(m_mixinIndex)]);
  }

  void FusionScreen::doFusion()
  {
    if (!m_submitWalletTask)
    {
      m_state = State::Error;
      m_error = "Internal error: wallet task runner not available";
      return;
    }

    m_state = State::Fusing;
    m_error.clear();
    m_taskStarted = std::chrono::steady_clock::now();

    const uint64_t threshold = m_thresholds[static_cast<size_t>(m_thresholdIndex)];
    const uint64_t mixin = m_mixins[static_cast<size_t>(m_mixinIndex)];
    auto wallet = m_wallet;

    m_submitWalletTask(
        [wallet, threshold, mixin]() { return wallet->createFusion(threshold, mixin); },
        [this](BoltCore::TransferResult result)
        {
          if (result.success)
          {
            m_state = State::Done;
            m_txHash = result.txHash;
          }
          else
          {
            m_state = State::Error;
            m_error = result.error.empty() ? "Fusion failed" : result.error;
          }
        });
  }

  void FusionScreen::onKey(int key)
  {
    if (m_wallet->getType() == BoltCore::WalletType::ViewOnly)
    {
      if (key == Tui::KEY_ESC && m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (m_state == State::Fusing)
      return;

    if (m_state == State::Done || m_state == State::Error)
    {
      if (isActivateKey(key) || key == ' ')
        onEnter();
      else if (key == Tui::KEY_ESC && m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (key == Tui::KEY_ESC)
    {
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (isBackKey(key))
    {
      switch (m_state)
      {
      case State::Confirm:
      case State::Preview:
        m_state = State::SelectMixin;
        m_error.clear();
        break;
      case State::SelectMixin:
        m_state = State::SelectThreshold;
        m_error.clear();
        break;
      default:
        break;
      }
      return;
    }

    switch (m_state)
    {
    case State::SelectThreshold:
      if (adjustPickerIndex(key, m_thresholdIndex, static_cast<int>(m_thresholds.size())))
        break;
      if (isActivateKey(key))
        m_state = State::SelectMixin;
      break;

    case State::SelectMixin:
      if (adjustPickerIndex(key, m_mixinIndex, static_cast<int>(m_mixins.size())))
        break;
      if (isActivateKey(key))
      {
        refreshEstimate();
        m_state = State::Preview;
        m_error.clear();
      }
      break;

    case State::Preview:
      if (isActivateKey(key))
      {
        if (!fusionIsPossible(m_estimate))
          m_error = "Nothing to optimize below this size/mixin";
        else
        {
          m_state = State::Confirm;
          m_error.clear();
        }
      }
      break;

    case State::Confirm:
      if (key == 'y' || key == 'Y' || isActivateKey(key))
        doFusion();
      else if (key == 'n' || key == 'N')
      {
        m_state = State::SelectMixin;
        m_error.clear();
      }
      break;

    default:
      break;
    }
  }

  void FusionScreen::render(Tui::ScreenBuffer &buf)
  {
    const auto balance = m_wallet->getBalance();
    const uint64_t available = BoltCore::spendableAmountBeforeFee(balance);
    const std::string walletType =
        m_wallet->getType() == BoltCore::WalletType::Full ? "Full" : "View-only";

    drawHeader(buf, title(), balance.currentHeight, available, walletType);

    if (m_wallet->getType() == BoltCore::WalletType::ViewOnly)
    {
      buf.writeAt(5, 4, Tui::red() + "Fusion is not available in view-only mode." + Tui::reset());
      drawMenuBar(buf, {"Overview"}, {"Esc"}, 3);
      return;
    }

    const int boxTop = 5;
    const int boxH = 16;
    buf.write(Tui::drawBox(boxTop, 2, boxH, 76, "Optimize (Fusion)"));

    const uint64_t threshold = m_thresholds.empty()
                                   ? m_currency.fusionStartingThreshold()
                                   : m_thresholds[static_cast<size_t>(m_thresholdIndex)];
    const uint64_t mixin = m_mixins.empty()
                               ? cn::parameters::MINIMUM_MIXIN
                               : m_mixins[static_cast<size_t>(m_mixinIndex)];

    const bool focusThreshold = m_state == State::SelectThreshold;
    const bool focusMixin = m_state == State::SelectMixin;

    auto row = [&](int r, const std::string &label, const std::string &value, bool focus)
    {
      std::string line = label + value;
      if (focus)
        line = Tui::brightWhite() + "> " + line + Tui::reset();
      buf.writeAt(boxTop + r, 4, line);
    };

    row(1, "Include outputs below: ", m_currency.formatAmount(threshold) + " CCX  [Up/Down]", focusThreshold);
    row(2, "Anonymity:  ", std::to_string(mixin) + "              [Up/Down]", focusMixin);

    if (m_state != State::SelectThreshold && m_state != State::SelectMixin)
    {
      const size_t minInputs = m_currency.fusionTxMinInputCount();
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Ready buckets:      " + Tui::reset() +
                                    Tui::green() + std::to_string(m_estimate.readyBucketCount) +
                                    Tui::reset() +
                                    Tui::dim() + "  (need >= " + std::to_string(minInputs) +
                                    " in one bucket)" + Tui::reset());
      buf.writeAt(boxTop + 5, 4, Tui::dim() + "Largest bucket:     " + Tui::reset() +
                                    Tui::green() + std::to_string(m_estimate.largestReadyBucket) +
                                    Tui::reset() +
                                    Tui::dim() + " outputs per tx" + Tui::reset());
      buf.writeAt(boxTop + 6, 4, Tui::dim() + "Total key outputs:  " + Tui::reset() +
                                    std::to_string(m_estimate.totalOutputCount) +
                                    Tui::dim() + "   Eligible: " + Tui::reset() +
                                    std::to_string(m_estimate.fusionReadyCount));
      buf.writeAt(boxTop + 7, 4, Tui::dim() + "Fee:                " + Tui::reset() +
                                    m_currency.formatAmount(m_currency.minimumFeeV2()) + " CCX");
    }

    if (mixin == 0)
      buf.writeAt(boxTop + 8, 4, Tui::dim() + "Anonymity 1 — includes outputs below dust threshold" + Tui::reset());
    else
      buf.writeAt(boxTop + 8, 4, Tui::dim() + "Outputs below dust threshold are excluded" + Tui::reset());

    if (m_state == State::Preview || m_state == State::Confirm)
    {
      const std::string advisory = fusionAdvisory(m_currency, m_estimate);
      if (!advisory.empty())
        buf.writeAt(boxTop + 9, 4, Tui::yellow() + advisory + Tui::reset());
    }

    if (m_state == State::Confirm)
      buf.writeAt(boxTop + 11, 4, Tui::brightYellow() + "Proceed with Fusion?  [Y] yes  [N] no" + Tui::reset());

    if (m_state == State::Fusing)
    {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - m_taskStarted)
                               .count();
      buf.writeAt(boxTop + 10, 4, Tui::yellow() + "Creating fusion transaction... " +
                                      std::to_string(elapsed) + "s" + Tui::reset());
      buf.writeAt(boxTop + 11, 4, Tui::dim() + "Waiting for daemon to relay..." + Tui::reset());
    }

    if (m_state == State::Done)
    {
      buf.writeAt(boxTop + 10, 4, Tui::green() + "Fusion sent!" + Tui::reset());
      buf.writeAt(boxTop + 11, 4, Tui::dim() + "Tx: " + Tui::reset() + m_txHash);
      buf.writeAt(boxTop + 12, 4, Tui::dim() + "Press Enter to run again" + Tui::reset());
    }

    if (!m_error.empty())
      buf.writeAt(boxTop + 14, 4, Tui::red() + m_error + Tui::reset());

    if (m_state == State::SelectThreshold)
      drawMenuBar(buf, {"Next", "Overview"}, {"Enter", "Esc"}, 3);
    else if (m_state == State::SelectMixin)
      drawMenuBar(buf, {"Next", "Back", "Overview"}, {"Enter", "Bksp", "Esc"}, 3);
    else if (m_state == State::Preview)
      drawMenuBar(buf, {"Continue", "Back", "Overview"}, {"Enter", "Bksp", "Esc"}, 3);
    else if (m_state == State::Confirm)
      drawMenuBar(buf, {"Yes", "No", "Back", "Overview"}, {"Y", "N", "Bksp", "Esc"}, 3);
    else if (m_state == State::Done || m_state == State::Error)
      drawMenuBar(buf, {"Again", "Overview"}, {"Enter", "Esc"}, 3);
    else
      drawMenuBar(buf, {"Overview"}, {"Esc"}, 3);
  }

} // namespace ClientWallet
