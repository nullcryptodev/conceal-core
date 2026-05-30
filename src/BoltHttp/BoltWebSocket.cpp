// BoltWebSocket.cpp — WebSocket frame handling implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltWebSocket.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>

// Inline SHA1 implementation for WebSocket handshake (no external dependency)
struct Sha1Context
{
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[64];
};

void sha1Init(Sha1Context *ctx)
{
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count = 0;
}

void sha1Transform(uint32_t state[5], const uint8_t block[64])
{
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  for (int i = 16; i < 80; ++i)
    w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]);
  // rotate left helper — not available as a keyword
  auto rotl = [](uint32_t v, int b)
  { return (v << b) | (v >> (32 - b)); };

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; ++i)
  {
    uint32_t f, k;
    if (i < 20)
    {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    }
    else if (i < 40)
    {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    }
    else if (i < 60)
    {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    }
    else
    {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void sha1Update(Sha1Context *ctx, const uint8_t *data, size_t len)
{
  size_t i = static_cast<size_t>((ctx->count >> 3) & 63);
  ctx->count += static_cast<uint64_t>(len) << 3;
  while (len--)
  {
    ctx->buffer[i++] = *data++;
    if (i == 64)
    {
      sha1Transform(ctx->state, ctx->buffer);
      i = 0;
    }
  }
}

void sha1Final(Sha1Context *ctx, uint8_t digest[20])
{
  size_t i = static_cast<size_t>((ctx->count >> 3) & 63);
  ctx->buffer[i++] = 0x80;
  if (i > 56)
  {
    while (i < 64)
      ctx->buffer[i++] = 0;
    sha1Transform(ctx->state, ctx->buffer);
    i = 0;
  }
  while (i < 56)
    ctx->buffer[i++] = 0;
  uint64_t bits = ctx->count;
  for (int j = 7; j >= 0; --j)
    ctx->buffer[56 + j] = static_cast<uint8_t>(bits >> (j * 8));
  sha1Transform(ctx->state, ctx->buffer);
  for (int j = 0; j < 5; ++j)
  {
    digest[j * 4] = static_cast<uint8_t>(ctx->state[j] >> 24);
    digest[j * 4 + 1] = static_cast<uint8_t>(ctx->state[j] >> 16);
    digest[j * 4 + 2] = static_cast<uint8_t>(ctx->state[j] >> 8);
    digest[j * 4 + 3] = static_cast<uint8_t>(ctx->state[j]);
  }
}

namespace BoltHttp
{

  namespace
  {
    // Base64 encode (for WebSocket accept key)
    std::string base64Encode(const unsigned char *data, size_t len)
    {
      static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string result;
      int val = 0, valb = -6;
      for (size_t i = 0; i < len; ++i)
      {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0)
        {
          result.push_back(chars[(val >> valb) & 0x3F]);
          valb -= 6;
        }
      }
      if (valb > -6)
        result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
      while (result.size() % 4)
        result.push_back('=');
      return result;
    }
  }

  // Build the HTTP 101 response for WebSocket upgrade
  std::string buildWebSocketHandshakeResponse(const std::string &clientKey)
  {
    std::string magic = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    Sha1Context ctx;
    sha1Init(&ctx);
    sha1Update(&ctx, reinterpret_cast<const uint8_t *>(magic.c_str()), magic.size());
    uint8_t digest[20];
    sha1Final(&ctx, digest);

    std::string acceptKey = base64Encode(digest, 20);

    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << acceptKey << "\r\n"
             << "\r\n";
    return response.str();
  }

  WebSocket::WebSocket(int clientFd)
      : m_fd(clientFd) {}

  WebSocket::~WebSocket()
  {
    close();
  }

  void WebSocket::close()
  {
    if (m_fd >= 0)
    {
      // Send close frame
      uint8_t closeFrame[] = {0x88, 0x00}; // FIN + close opcode, no payload
      send(m_fd, closeFrame, sizeof(closeFrame), MSG_NOSIGNAL);
      shutdown(m_fd, SHUT_RDWR);
      ::close(m_fd);
      m_fd = -1;
    }
  }

  bool WebSocket::sendText(const std::string &message)
  {
    if (m_fd < 0)
      return false;

    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + text opcode

    // Payload length
    size_t len = message.size();
    if (len <= 125)
    {
      frame.push_back(static_cast<uint8_t>(len));
    }
    else if (len <= 65535)
    {
      frame.push_back(126);
      frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
      frame.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    else
    {
      frame.push_back(127);
      for (int i = 7; i >= 0; --i)
        frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }

    frame.insert(frame.end(), message.begin(), message.end());

    ssize_t sent = send(m_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(frame.size());
  }

  bool WebSocket::sendBinary(const std::vector<uint8_t> &data)
  {
    if (m_fd < 0)
      return false;

    std::vector<uint8_t> frame;
    frame.push_back(0x82); // FIN + binary opcode

    size_t len = data.size();
    if (len <= 125)
    {
      frame.push_back(static_cast<uint8_t>(len));
    }
    else if (len <= 65535)
    {
      frame.push_back(126);
      frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
      frame.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    else
    {
      frame.push_back(127);
      for (int i = 7; i >= 0; --i)
        frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }

    frame.insert(frame.end(), data.begin(), data.end());

    ssize_t sent = send(m_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(frame.size());
  }

  void WebSocket::processIncoming()
  {
    if (!m_handshakeDone)
    {
      // Read HTTP headers for handshake
      char buf[4096];
      ssize_t n = recv(m_fd, buf, sizeof(buf) - 1, 0);
      if (n <= 0)
      {
        if (m_onClose)
          m_onClose();
        return;
      }
      buf[n] = '\0';
      std::string headers(buf, n);

      // Extract Sec-WebSocket-Key
      std::string key;
      size_t keyPos = headers.find("Sec-WebSocket-Key: ");
      if (keyPos != std::string::npos)
      {
        keyPos += 19;
        size_t keyEnd = headers.find("\r\n", keyPos);
        if (keyEnd != std::string::npos)
          key = headers.substr(keyPos, keyEnd - keyPos);
      }

      if (key.empty())
      {
        if (m_onClose)
          m_onClose();
        return;
      }

      // Send handshake response
      std::string response = buildWebSocketHandshakeResponse(key);
      send(m_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
      m_handshakeDone = true;
      return;
    }

    // Read frame data
    char buf[65536];
    ssize_t n = recv(m_fd, buf, sizeof(buf), 0);
    if (n <= 0)
    {
      if (m_onClose)
        m_onClose();
      return;
    }

    m_readBuffer.insert(m_readBuffer.end(), buf, buf + n);

    // Parse frames from buffer
    while (parseFrame())
    {
      // Continue parsing
    }
  }

  bool WebSocket::parseFrame()
  {
    if (m_readBuffer.size() < 2)
      return false;

    const uint8_t *data = m_readBuffer.data();
    size_t pos = 0;

    uint8_t opcode = data[pos] & 0x0F;
    bool masked = (data[pos + 1] & 0x80) != 0;
    uint64_t payloadLen = data[pos + 1] & 0x7F;
    pos += 2;

    if (payloadLen == 126)
    {
      if (m_readBuffer.size() < pos + 2)
        return false;
      payloadLen = (static_cast<uint64_t>(data[pos]) << 8) | data[pos + 1];
      pos += 2;
    }
    else if (payloadLen == 127)
    {
      if (m_readBuffer.size() < pos + 8)
        return false;
      payloadLen = 0;
      for (int i = 0; i < 8; ++i)
        payloadLen = (payloadLen << 8) | data[pos + i];
      pos += 8;
    }

    uint8_t maskKey[4] = {};
    if (masked)
    {
      if (m_readBuffer.size() < pos + 4)
        return false;
      memcpy(maskKey, &data[pos], 4);
      pos += 4;
    }

    if (m_readBuffer.size() < pos + payloadLen)
      return false;

    std::vector<uint8_t> payload(payloadLen);
    for (uint64_t i = 0; i < payloadLen; ++i)
      payload[i] = data[pos + i] ^ (masked ? maskKey[i % 4] : 0);

    // Handle opcode
    switch (opcode)
    {
    case 0x1: // Text
      if (m_onMessage)
        m_onMessage(std::string(payload.begin(), payload.end()));
      break;
    case 0x2: // Binary
      if (m_onBinary)
        m_onBinary(payload);
      break;
    case 0x8: // Close
      if (m_onClose)
        m_onClose();
      break;
    case 0x9: // Ping — respond with pong
    {
      std::vector<uint8_t> pong = {0x8A};
      pong.push_back(static_cast<uint8_t>(payloadLen));
      pong.insert(pong.end(), payload.begin(), payload.end());
      send(m_fd, pong.data(), pong.size(), MSG_NOSIGNAL);
      break;
    }
    case 0xA: // Pong — ignore
      break;
    }

    // Remove parsed data from buffer
    m_readBuffer.erase(m_readBuffer.begin(), m_readBuffer.begin() + pos + payloadLen);
    return true;
  }

} // namespace BoltHttp