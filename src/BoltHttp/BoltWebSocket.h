// BoltWebSocket.h — WebSocket frame handling for BoltHttp
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace BoltHttp
{
  // One frame received from or to send to a WebSocket client
  struct WebSocketFrame
  {
    bool fin = true;              // Final fragment
    uint8_t opcode = 1;           // 1=text, 2=binary, 8=close, 9=ping, 10=pong
    bool masked = false;          // Client-to-server frames are masked
    uint8_t maskKey[4] = {};      // Masking key (only for masked frames)
    std::vector<uint8_t> payload; // Frame payload data
  };

  // Callback for received text messages
  using WebSocketMessageHandler = std::function<void(const std::string &message)>;
  // Callback for received binary messages
  using WebSocketBinaryHandler = std::function<void(const std::vector<uint8_t> &data)>;
  // Callback for connection close
  using WebSocketCloseHandler = std::function<void()>;

  // WebSocket connection wrapping a TCP socket file descriptor
  class WebSocket
  {
  public:
    explicit WebSocket(int clientFd);
    ~WebSocket();

    // Send a text frame
    bool sendText(const std::string &message);
    // Send a binary frame
    bool sendBinary(const std::vector<uint8_t> &data);
    // Send a close frame and shutdown
    void close();

    // Read available data from the socket and parse frames
    void processIncoming();

    // Handlers
    void onMessage(WebSocketMessageHandler handler) { m_onMessage = std::move(handler); }
    void onBinary(WebSocketBinaryHandler handler) { m_onBinary = std::move(handler); }
    void onClose(WebSocketCloseHandler handler) { m_onClose = std::move(handler); }

    // Peer address for logging
    std::string peerAddress() const { return m_peerAddr; }
    void setPeerAddress(const std::string &addr) { m_peerAddr = addr; }

  private:
    // Perform the server-side WebSocket handshake from HTTP headers
    bool performHandshake(const std::string &httpHeaders);

    // Parse a WebSocket frame from the accumulated buffer
    bool parseFrame();

    int m_fd;
    std::string m_peerAddr;
    std::vector<uint8_t> m_readBuffer;
    WebSocketMessageHandler m_onMessage;
    WebSocketBinaryHandler m_onBinary;
    WebSocketCloseHandler m_onClose;
    bool m_handshakeDone = false;
  };

  // Build the HTTP response for a successful WebSocket upgrade
  std::string buildWebSocketHandshakeResponse(const std::string &clientKey);

} // namespace BoltHttp