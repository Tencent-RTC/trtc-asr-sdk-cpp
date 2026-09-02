#pragma once

// Test doubles: a minimal HTTP/1.1 server and a WebSocket (RFC 6455) server
// so recognizer tests run hermetically on localhost.

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace trtc_asr_test {

// --- Mock HTTP server -------------------------------------------------------

struct CapturedHttpRequest {
  std::string method;
  std::string target;  // path + query
  std::map<std::string, std::string> headers;  // lower-cased names
  std::string body;

  std::string Header(const std::string& name) const;
  /// Decoded query parameter value; nullopt when absent.
  std::optional<std::string> Query(const std::string& key) const;
};

struct MockHttpResponse {
  int status = 200;
  std::string body;
};

class MockHttpServer {
 public:
  using Handler = std::function<MockHttpResponse(const CapturedHttpRequest&)>;

  explicit MockHttpServer(Handler handler);
  ~MockHttpServer();

  std::string Url() const;
  std::vector<CapturedHttpRequest> Requests() const;

 private:
  void Serve();

  Handler handler_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread thread_;
  mutable std::mutex mu_;
  std::vector<CapturedHttpRequest> requests_;
};

// --- Mock WebSocket server --------------------------------------------------

class MockWsSession {
 public:
  explicit MockWsSession(int fd) : fd_(fd) {}

  /// Reads one frame; returns false on EOF/connection error.
  bool Read(int* opcode, std::string* payload);
  bool SendText(const std::string& text);
  bool SendBinary(const std::string& data);
  void Close();

 private:
  int fd_;
};

class MockWsServer {
 public:
  using Handler = std::function<void(MockWsSession&)>;

  explicit MockWsServer(Handler handler);
  ~MockWsServer();

  std::string Url() const;
  /// The handshake request target (path + query) for assertions.
  std::string RequestTarget() const;
  void Join();

 private:
  void Run();

  Handler handler_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread thread_;
  mutable std::mutex mu_;
  std::string request_target_;
};

/// Percent-decodes a query value ('+' → space, %XX → byte). Test utility.
std::string PercentDecode(const std::string& s);

}  // namespace trtc_asr_test
