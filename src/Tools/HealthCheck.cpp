// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>
#include <cstring>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

std::string http_get(const std::string &host, int port, const std::string &path, int timeout_sec = 10)
{
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return "";

  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  struct hostent *server = gethostbyname(host.c_str());
  if (!server)
  {
    close(sock);
    return "";
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
  addr.sin_port = htons(port);

  int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS)
  {
    close(sock);
    return "";
  }

  if (ret < 0)
  {
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ret = select(sock + 1, NULL, &fdset, NULL, &tv);
    if (ret <= 0)
    {
      close(sock);
      return "";
    }

    int so_error;
    socklen_t len = sizeof(so_error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0)
    {
      close(sock);
      return "";
    }
  }

  fcntl(sock, F_SETFL, flags);

  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::string request = "GET " + path + " HTTP/1.1\r\n" +
                        "Host: " + host + "\r\n" +
                        "Connection: close\r\n" +
                        "\r\n";

  if (send(sock, request.c_str(), request.size(), 0) <= 0)
  {
    close(sock);
    return "";
  }

  std::string response;
  char buffer[4096];
  while (true)
  {
    int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0)
      break;
    buffer[n] = '\0';
    response += buffer;
  }

  close(sock);

  size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos)
    return "";

  return response.substr(header_end + 4);
}

void print_usage(const char *prog)
{
  std::cout << "Conceal Network Daemon Health Checker\n\n";
  std::cout << "Usage: " << prog << " [OPTIONS]\n\n";
  std::cout << "Options:\n";
  std::cout << "  --daemon <host:port>   Daemon RPC address (default: 127.0.0.1:16000)\n";
  std::cout << "  --watch [seconds]      Watch mode, refresh every N seconds (default: 5)\n";
  std::cout << "  --help                 Show this help\n\n";
  std::cout << "Examples:\n";
  std::cout << "  " << prog << "\n";
  std::cout << "  " << prog << " --daemon seed1.conceal.network:16000\n";
  std::cout << "  " << prog << " --watch 10\n";
}

int main(int argc, char *argv[])
{
  std::string host = "127.0.0.1";
  int port = 16000;
  bool watch_mode = false;
  int watch_interval = 5;

  for (int i = 1; i < argc; i++)
  {
    std::string arg = argv[i];
    if (arg == "--help")
    {
      print_usage(argv[0]);
      return 0;
    }
    else if (arg == "--watch")
    {
      watch_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
      {
        watch_interval = std::atoi(argv[++i]);
        if (watch_interval < 1)
          watch_interval = 1;
      }
    }
    else if (arg == "--daemon" && i + 1 < argc)
    {
      std::string addr = argv[++i];
      size_t colon = addr.find(':');
      if (colon != std::string::npos)
      {
        host = addr.substr(0, colon);
        port = std::stoi(addr.substr(colon + 1));
      }
      else
      {
        host = addr;
      }
    }
  }

  do
  {
    std::cout << "Fetching /getinfo... could take ~10 seconds.\n";

    std::string json = http_get(host, port, "/getinfo", 10);

    if (json.empty())
    {
      std::cerr << "!! Failed to connect to " << host << ":" << port << std::endl;
      std::cerr << "   Make sure the daemon is running with RPC enabled" << std::endl;
      return 1;
    }

    // Parse values directly from the JSON string
    auto find_value = [&](const std::string &key) -> std::string
    {
      std::string search = "\"" + key + "\":";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return "";
      pos += search.length();

      // Skip whitespace
      while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t'))
        pos++;

      if (json[pos] == '"')
      {
        // String value
        pos++;
        size_t end = json.find('"', pos);
        if (end == std::string::npos)
          return "";
        return json.substr(pos, end - pos);
      }
      else
      {
        // Number value
        size_t end = pos;
        while (end < json.length() && (isdigit(json[end]) || json[end] == '-'))
          end++;
        return json.substr(pos, end - pos);
      }
    };

    uint64_t height = std::stoull(find_value("height"));
    uint64_t network_height = std::stoull(find_value("last_known_block_index"));
    uint64_t difficulty = std::stoull(find_value("difficulty"));
    uint64_t tx_pool = std::stoull(find_value("tx_pool_size"));
    uint64_t reward_atomic = std::stoull(find_value("last_block_reward"));
    uint64_t timestamp = std::stoull(find_value("last_block_timestamp"));
    uint32_t outgoing = std::stoi(find_value("outgoing_connections_count"));
    uint32_t incoming = std::stoi(find_value("incoming_connections_count"));
    uint32_t white = std::stoi(find_value("white_peerlist_size"));
    uint32_t grey = std::stoi(find_value("grey_peerlist_size"));
    std::string version = find_value("version");
    std::string top_hash = find_value("top_block_hash");

    // Convert reward from atomic units to CCX (6 decimals)
    double reward_ccx = reward_atomic / 1000000.0;

    int64_t behind = network_height - height;
    bool synced = (behind <= 1);
    double progress = network_height > 0 ? (double)height / network_height : 0;

    if (watch_mode)
    {
      std::cout << "\033[2J\033[1;1H";
    }

    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              Conceal Network Daemon Health Check             ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    std::cout << "DAEMON: " << host << ":" << port << "\n";
    std::cout << "VERSION: " << version << "\n\n";

    // Progress bar
    int bar_width = 50;
    int filled = static_cast<int>(progress * bar_width);

    std::cout << "SYNC STATUS:\n";
    std::cout << "   Height:        " << height << " / " << network_height;
    if (synced)
    {
      std::cout << " 100% SYNCED\n";
    }
    else
    {
      std::cout << " (behind by " << behind << " blocks)\n";
    }

    std::cout << "   Progress:      " << std::fixed << std::setprecision(2) << (progress * 100) << "% ";
    std::cout << "[";
    for (int i = 0; i < bar_width; i++)
    {
      std::cout << (i < filled ? "=" : (i == filled ? ">" : " "));
    }
    std::cout << "]\n";
    std::cout << "   Difficulty:    " << difficulty << "\n\n";

    std::cout << "NETWORK:\n";
    uint32_t total_conn = outgoing + incoming;
    std::cout << "   Connections:   " << total_conn << " (" << outgoing << " out";
    if (incoming > 0)
    {
      std::cout << ", " << incoming << " in";
    }
    std::cout << ")\n";
    std::cout << "   Peerlist:      " << white << " white, " << grey << " grey\n";
    std::cout << "   Mempool:       " << tx_pool << " transactions\n\n";

    if (timestamp > 0)
    {
      time_t ts = timestamp;
      struct tm *tm_info = localtime(&ts);
      char time_buffer[64];
      strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

      std::cout << "LAST BLOCK:\n";
      std::cout << "   Hash:         " << top_hash << "\n";
      std::cout << "   Reward:       " << std::fixed << std::setprecision(6) << reward_ccx << " $CCX\n";
      std::cout << "   Time:         " << time_buffer << "\n\n";
    }

    // Calculate health score
    int health_score = 100;
    if (!synced)
      health_score -= 30;
    if (total_conn < 5)
      health_score -= 20;
    if (tx_pool > 1000)
      health_score -= 10;
    if (health_score < 0)
      health_score = 0;

    std::cout << "HEALTH SCORE: " << health_score << "/100\n";

    if (!synced)
    {
      std::cout << "\n!! NODE SYNCING: " << std::fixed << std::setprecision(2) << (progress * 100) << "% complete\n";
      std::cout << "   Estimated blocks remaining: " << behind << "\n";
    }

    std::cout << std::endl;

    if (watch_mode)
    {
      std::this_thread::sleep_for(std::chrono::seconds(watch_interval));
    }

  } while (watch_mode);

  return 0;
}