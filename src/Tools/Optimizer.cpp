// Copyright (c) 2019 Helder Garcia <helder.garcia@gmail.com>
// Copyright (c) 2019, The Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Modified to use BoltRPC (conceal-rpc) instead of legacy walletd

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <chrono>
#include <thread>
#include <vector>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>

#include "Common/CommandLine.h"
#include "Common/JsonValue.h"
#include "Common/StringTools.h"
#include "Logging/LoggerRef.h"
#include "Logging/ConsoleLogger.h"
#include "BoltHttp/BoltHttpClient.h"
#include <System/Dispatcher.h>

namespace po = boost::program_options;
using common::JsonValue;
using namespace logging;

#ifndef ENDL
#define ENDL std::endl
#endif

const uint64_t DEFAULT_THRESHOLD = UINT64_C(100);
const uint16_t DEFAULT_RPC_PORT = 8070;

namespace
{
  const command_line::arg_descriptor<std::string> arg_address = {"address", "Address of the wallet to optimize inputs. Default: current wallet", "", true};
  const command_line::arg_descriptor<std::string> arg_ip = {"rpc-ip", "IP address of conceal-rpc. Default: 127.0.0.1", "127.0.0.1"};
  const command_line::arg_descriptor<uint16_t> arg_rpc_port = {"rpc-port", "RPC port of conceal-rpc. Default: 8070", DEFAULT_RPC_PORT};
  const command_line::arg_descriptor<uint16_t> arg_interval = {"interval", "polling interval in seconds. Default: 5. Minimum: 1. Maximum: 120.", 5, true};
  const command_line::arg_descriptor<uint16_t> arg_duration = {"duration", "maximum execution time, in minutes. Default: 0 (unlimited)", 0, true};
  const command_line::arg_descriptor<uint64_t> arg_threshold = {"threshold", "Only outputs lesser than the threshold value will be included into optimization. Default: 100 (0.000100 CCX)", DEFAULT_THRESHOLD, true};
  const command_line::arg_descriptor<uint16_t> arg_anonymity = {"anonymity", "Privacy level (mixin). Higher values give more privacy but bigger transactions. Default: 0", 0, true};
  const command_line::arg_descriptor<bool> arg_preview = {"preview", "print on screen what it would be doing, but not really doing it", false, true};

  logging::ConsoleLogger consoleLogger;
  logging::LoggerRef logger(consoleLogger, "optimizer");
  platform_system::Dispatcher dispatcher;
}

// Simple RPC client for BoltRPC
class BoltRpcClient
{
public:
  BoltRpcClient(const std::string &host, uint16_t port)
      : m_httpClient(host, port) {}

  bool call(const std::string &method, const std::string &params, std::string &result)
  {
    try
    {
      std::string body = R"({"jsonrpc":"2.0","method":")" + method + R"(","params":)" + params + R"(,"id":1})";
      auto httpRes = m_httpClient.post("/json_rpc", body);

      if (!httpRes.success || httpRes.statusCode != 200)
      {
        logger(ERROR, RED) << "HTTP request failed: " << httpRes.error;
        return false;
      }

      JsonValue val = JsonValue::fromString(httpRes.body);
      if (val.contains("error"))
      {
        logger(ERROR, RED) << "RPC error: " << val("error")("message").getString();
        return false;
      }

      if (val.contains("result"))
      {
        result = val("result").toString();
        return true;
      }

      return false;
    }
    catch (const std::exception &e)
    {
      logger(ERROR, RED) << "RPC call failed: " << e.what();
      return false;
    }
  }

private:
  BoltHttp::HttpClient m_httpClient;
};

bool canConnect(BoltRpcClient &client)
{
  std::string result;
  return client.call("getVersion", "{}", result);
}

uint64_t getBalance(BoltRpcClient &client)
{
  std::string result;
  if (client.call("getBalance", "{}", result))
  {
    try
    {
      JsonValue val = JsonValue::fromString(result);
      if (val.contains("availableBalance"))
      {
        return val("availableBalance").getInteger();
      }
    }
    catch (...)
    {
    }
  }
  return 0;
}

uint32_t estimateFusion(BoltRpcClient &client, uint64_t threshold)
{
  std::string params = R"({"threshold":)" + std::to_string(threshold) + R"(})";
  std::string result;
  if (client.call("estimateFusion", params, result))
  {
    try
    {
      JsonValue val = JsonValue::fromString(result);
      if (val.contains("fusionReadyCount"))
      {
        return static_cast<uint32_t>(val("fusionReadyCount").getInteger());
      }
    }
    catch (...)
    {
    }
  }
  return 0;
}

bool sendFusion(BoltRpcClient &client, uint64_t threshold, uint16_t anonymity)
{
  std::string params = R"({"threshold":)" + std::to_string(threshold) +
                       R"(,"mixin":)" + std::to_string(anonymity) + R"(})";
  std::string result;
  if (client.call("sendFusionTransaction", params, result))
  {
    try
    {
      JsonValue val = JsonValue::fromString(result);
      if (val.contains("transactionHash"))
      {
        logger(INFO, GREEN) << "Transaction hash: " << val("transactionHash").getString();
        return true;
      }
    }
    catch (...)
    {
    }
  }
  return false;
}

bool run_optimizer(po::variables_map &vm)
{
  std::string rpcHost = command_line::get_arg(vm, arg_ip);
  uint16_t rpcPort = command_line::get_arg(vm, arg_rpc_port);

  BoltRpcClient client(rpcHost, rpcPort);

  if (!canConnect(client))
  {
    logger(ERROR, RED) << "Failed to connect to conceal-rpc at " << rpcHost << ":" << rpcPort;
    logger(ERROR, RED) << "Make sure conceal-rpc is running and the wallet is loaded.";
    return false;
  }

  logger(INFO, GREEN) << "Connected to conceal-rpc at " << rpcHost << ":" << rpcPort;

  // Get initial balance
  uint64_t initialBalance = getBalance(client);
  logger(INFO, GREEN) << "Current balance: " << initialBalance << " atomic units";

  uint64_t threshold = DEFAULT_THRESHOLD;
  if (command_line::has_arg(vm, arg_threshold))
  {
    threshold = command_line::get_arg(vm, arg_threshold);
  }

  uint16_t anonymity = 0;
  if (command_line::has_arg(vm, arg_anonymity))
  {
    anonymity = command_line::get_arg(vm, arg_anonymity);
  }

  uint16_t timeInterval = 5;
  if (command_line::has_arg(vm, arg_interval))
  {
    timeInterval = command_line::get_arg(vm, arg_interval);
    if (timeInterval > 120)
      timeInterval = 120;
    if (timeInterval < 1)
      timeInterval = 1;
  }

  int32_t maxDuration = 0;
  if (command_line::has_arg(vm, arg_duration))
  {
    maxDuration = command_line::get_arg(vm, arg_duration);
  }

  bool previewMode = command_line::get_arg(vm, arg_preview);

  auto start = std::chrono::steady_clock::now();

  logger(INFO, YELLOW) << "Starting optimization...";
  logger(INFO, YELLOW) << "Threshold: " << threshold << ", Anonymity: " << anonymity;

  // Check if fusion is needed
  uint32_t fusionReadyCount = estimateFusion(client, threshold);

  if (fusionReadyCount > 0)
  {
    logger(INFO, GREEN) << "Found " << fusionReadyCount << " outputs ready for fusion";

    if (previewMode)
    {
      logger(INFO, YELLOW) << "PREVIEW MODE: Would optimize " << fusionReadyCount << " outputs";
    }
    else
    {
      logger(INFO, GREEN) << "Starting fusion transaction...";

      if (sendFusion(client, threshold, anonymity))
      {
        logger(INFO, GREEN) << "Fusion transaction sent successfully";

        // Wait for confirmation if interval is set
        if (timeInterval > 0)
        {
          logger(INFO, GREEN) << "Waiting " << timeInterval << " seconds for confirmation...";
          std::this_thread::sleep_for(std::chrono::seconds(timeInterval));
        }

        // Check new balance
        uint64_t newBalance = getBalance(client);
        logger(INFO, GREEN) << "New balance: " << newBalance << " atomic units";
      }
      else
      {
        logger(ERROR, RED) << "Failed to send fusion transaction";
        return false;
      }
    }
  }
  else
  {
    logger(INFO, YELLOW) << "No outputs ready for fusion";
  }

  auto dur = std::chrono::steady_clock::now() - start;

  std::cout << "============== SUMMARY =============" << ENDL;
  std::cout << "   Threshold              : " << threshold << ENDL;
  std::cout << "   Anonymity              : " << anonymity << ENDL;
  std::cout << "   Fusion-ready outputs   : " << fusionReadyCount << ENDL;
  if (!previewMode && fusionReadyCount > 0)
  {
    std::cout << "   Fusion sent            : YES" << ENDL;
  }
  std::cout << "   Processing time (sec)  : " << std::chrono::duration_cast<std::chrono::seconds>(dur).count() << ENDL;
  std::cout << "====================================" << ENDL;

  return true;
}

int main(int argc, char *argv[])
{
  po::options_description desc_general("General options");
  command_line::add_arg(desc_general, command_line::arg_help);

  po::options_description desc_params("Command options");
  command_line::add_arg(desc_params, arg_address);
  command_line::add_arg(desc_params, arg_ip);
  command_line::add_arg(desc_params, arg_rpc_port);
  command_line::add_arg(desc_params, arg_interval);
  command_line::add_arg(desc_params, arg_duration);
  command_line::add_arg(desc_params, arg_threshold);
  command_line::add_arg(desc_params, arg_anonymity);
  command_line::add_arg(desc_params, arg_preview);

  po::options_description desc_all;
  desc_all.add(desc_general).add(desc_params);

  po::variables_map vm;
  bool r = command_line::handle_error_helper(desc_all, [&]()
                                             {
    po::store(command_line::parse_command_line(argc, argv, desc_general, true), vm);
    if (command_line::get_arg(vm, command_line::arg_help)) {
      std::cout << "Optimizer for Conceal (conceal-rpc)" << ENDL;
      std::cout << "This program comes with ABSOLUTELY NO WARRANTY;" << ENDL;
      std::cout << desc_all << ENDL;
      return false;
    }
    
    po::store(command_line::parse_command_line(argc, argv, desc_params, false), vm);
    po::notify(vm);
    return true; });

  if (!r)
    return 1;

  return run_optimizer(vm) ? 0 : 1;
}