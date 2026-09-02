#pragma once

// Internal minimal WebSocket (RFC 6455) client used by SpeechRecognizer.
//
// Supports the subset the ASR protocol exercises: text/binary frames,
// ping/pong, close, ws:// and wss:// (TLS via OpenSSL with system root CAs
// and hostname verification). Client frames are masked per RFC.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace trtc_asr {
namespace internal {

struct WsFrame {
  int opcode = 0;        // 0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong
  std::string payload;   // binary-safe
  bool fin = true;
};

/// RFC 6455 frame codec, shared with the test mock server.
/// Encode a frame; when mask is true (client role) a random mask is applied.
std::string EncodeWsFrame(int opcode, const std::string& payload, bool mask);

class WsClient {
 public:
  /// Connects to url (ws:// or wss://) and performs the WebSocket handshake.
  /// Returns nullptr and sets err on failure. timeout_ms bounds the TCP
  /// connect, TLS handshake and HTTP upgrade.
  static std::unique_ptr<WsClient> Connect(const std::string& url, int timeout_ms,
                                           std::string* err);

  ~WsClient();

  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  /// Sends a text frame (masked). Serialized internally.
  bool SendText(const std::string& text, std::string* err);
  /// Sends a binary frame (masked). Serialized internally.
  bool SendBinary(const uint8_t* data, size_t len, std::string* err);

  /// Per-write deadline applied by SendText/SendBinary (default 30s).
  void SetWriteTimeoutMs(int timeout_ms) { write_timeout_ms_ = timeout_ms; }

  /// Reads the next message.
  /// Returns 1 on a message, 0 on timeout, -1 on error/connection closed.
  /// Ping frames are answered automatically and skipped. On -1, err carries
  /// details (TLS error code / errno) when available.
  int Read(WsFrame* out, int timeout_ms, std::string* err = nullptr);

  /// Force-closes the connection, unblocking any in-flight Read/Send.
  void Close();

 private:
  WsClient() = default;

  /// Reads exactly n bytes before the absolute deadline (steady_clock ms).
  /// Returns 1 ok, 0 timeout, -1 error.
  int ReadExact(uint8_t* buf, size_t n, int64_t deadline_ms);

  bool WriteAll(const uint8_t* buf, size_t n, int timeout_ms, std::string* err);

  int fd_ = -1;
  SSL* ssl_ = nullptr;
  SSL_CTX* ssl_ctx_ = nullptr;
  std::vector<uint8_t> read_buf_;
  std::mutex write_mu_;
  int write_timeout_ms_ = 30000;
  std::string last_read_err_;
};

/// Generates a random UUIDv4 string (also used for voice_id / RequestId).
std::string GenerateUuid();

}  // namespace internal
}  // namespace trtc_asr
