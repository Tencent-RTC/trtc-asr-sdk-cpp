#include "ws_client.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <random>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "trtc_asr/usersig.h"  // Base64Encode

namespace trtc_asr {
namespace internal {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// Parses ws://host[:port]/path or wss://... into components.
struct UrlParts {
  bool secure = false;
  std::string host;
  int port = 0;
  std::string target = "/";
};

bool ParseWsUrl(const std::string& url, UrlParts* out, std::string* err) {
  std::string rest;
  if (url.rfind("wss://", 0) == 0) {
    out->secure = true;
    out->port = 443;
    rest = url.substr(6);
  } else if (url.rfind("ws://", 0) == 0) {
    out->secure = false;
    out->port = 80;
    rest = url.substr(5);
  } else {
    *err = "unsupported scheme, want ws or wss";
    return false;
  }
  size_t slash = rest.find('/');
  std::string authority = rest.substr(0, slash);
  if (slash != std::string::npos) {
    out->target = rest.substr(slash);
  }
  if (authority.empty()) {
    *err = "url has no host";
    return false;
  }
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    out->host = authority.substr(0, colon);
    try {
      out->port = std::stoi(authority.substr(colon + 1));
    } catch (...) {
      *err = "invalid port";
      return false;
    }
  } else {
    out->host = authority;
  }
  return true;
}

/// Connects a TCP socket with a deadline. Returns -1 on failure.
int TcpConnect(const std::string& host, int port, int timeout_ms, std::string* err) {
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  std::string port_str = std::to_string(port);
  int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (rc != 0) {
    *err = std::string("dns resolve failed: ") + gai_strerror(rc);
    return -1;
  }

  int fd = -1;
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;

    // Non-blocking connect with poll timeout.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      struct pollfd pfd = {fd, POLLOUT, 0};
      int poll_rc = poll(&pfd, 1, timeout_ms);
      if (poll_rc > 0) {
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
          rc = -1;
          errno = soerr;
        } else {
          rc = 0;  // connect completed
        }
      } else {
        rc = -1;
        errno = ETIMEDOUT;
      }
    }
    fcntl(fd, F_SETFL, flags);  // back to blocking
    if (rc == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    *err = std::string("tcp connect failed: ") + std::strerror(errno);
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

/// Polls a socket for readability/writability. Returns 1 ready, 0 timeout,
/// -1 error.
///
/// POLLIN|POLLHUP (peer sent data then closed) must still report readable so
/// the pending bytes are drained; the subsequent recv yields EOF.
int PollFd(int fd, short events, int timeout_ms) {
  struct pollfd pfd = {fd, events, 0};
  int rc;
  do {
    rc = poll(&pfd, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);
  if (rc <= 0) return rc;
  if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
  if ((pfd.revents & events) == 0) return -1;  // pure HUP with nothing to do
  return 1;
}

}  // namespace

std::string EncodeWsFrame(int opcode, const std::string& payload, bool mask) {
  std::string frame;
  frame.reserve(payload.size() + 14);
  frame += static_cast<char>(0x80 | opcode);
  uint64_t len = payload.size();
  uint8_t mask_bit = mask ? 0x80 : 0x00;
  if (len < 126) {
    frame += static_cast<char>(mask_bit | len);
  } else if (len <= 0xFFFF) {
    frame += static_cast<char>(mask_bit | 126);
    frame += static_cast<char>((len >> 8) & 0xFF);
    frame += static_cast<char>(len & 0xFF);
  } else {
    frame += static_cast<char>(mask_bit | 127);
    for (int i = 7; i >= 0; i--) {
      frame += static_cast<char>((len >> (8 * i)) & 0xFF);
    }
  }
  if (mask) {
    uint8_t key[4];
    static thread_local std::mt19937 rng(std::random_device{}());
    for (int i = 0; i < 4; i++) key[i] = static_cast<uint8_t>(rng());
    frame.append(reinterpret_cast<const char*>(key), 4);
    for (size_t i = 0; i < payload.size(); i++) {
      frame += static_cast<char>(payload[i] ^ key[i % 4]);
    }
  } else {
    frame += payload;
  }
  return frame;
}

std::unique_ptr<WsClient> WsClient::Connect(const std::string& url, int timeout_ms,
                                            std::string* err) {
  UrlParts parts;
  if (!ParseWsUrl(url, &parts, err)) {
    *err = "invalid endpoint url: " + *err;
    return nullptr;
  }

  int fd = TcpConnect(parts.host, parts.port, timeout_ms, err);
  if (fd < 0) return nullptr;

  auto client = std::unique_ptr<WsClient>(new WsClient());
  client->fd_ = fd;

  if (parts.secure) {
    client->ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (client->ssl_ctx_ == nullptr) {
      *err = "tls ctx init failed";
      client->Close();
      return nullptr;
    }
    SSL_CTX_set_default_verify_paths(client->ssl_ctx_);
    client->ssl_ = SSL_new(client->ssl_ctx_);
    SSL_set_fd(client->ssl_, fd);
    SSL_set_tlsext_host_name(client->ssl_, parts.host.c_str());
    // Verify the peer certificate and hostname.
    SSL_set_verify(client->ssl_, SSL_VERIFY_PEER, nullptr);
    X509_VERIFY_PARAM* param = SSL_get0_param(client->ssl_);
    X509_VERIFY_PARAM_set1_host(param, parts.host.c_str(), parts.host.size());

    // Handshake with poll-driven deadline.
    int64_t deadline = NowMs() + timeout_ms;
    while (true) {
      int rc = SSL_connect(client->ssl_);
      if (rc == 1) break;
      int ssl_err = SSL_get_error(client->ssl_, rc);
      if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
        int remaining = static_cast<int>(deadline - NowMs());
        if (remaining <= 0 ||
            PollFd(fd, ssl_err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
                   remaining) != 1) {
          *err = "tls handshake timeout";
          client->Close();
          return nullptr;
        }
        continue;
      }
      *err = "tls handshake failed: " +
             std::string(ERR_error_string(ERR_get_error(), nullptr));
      client->Close();
      return nullptr;
    }
  }

  // HTTP Upgrade handshake.
  std::string key;
  {
    uint8_t nonce[16];
    static thread_local std::mt19937 rng(std::random_device{}());
    for (auto& b : nonce) b = static_cast<uint8_t>(rng());
    key = Base64Encode(nonce, sizeof(nonce));
  }
  std::string req = "GET " + parts.target + " HTTP/1.1\r\n" +
                    "Host: " + parts.host + ":" + std::to_string(parts.port) + "\r\n" +
                    "Upgrade: websocket\r\n" +
                    "Connection: Upgrade\r\n" +
                    "Sec-WebSocket-Key: " + key + "\r\n" +
                    "Sec-WebSocket-Version: 13\r\n\r\n";
  if (!client->WriteAll(reinterpret_cast<const uint8_t*>(req.data()), req.size(),
                        timeout_ms, err)) {
    client->Close();
    return nullptr;
  }

  // Read the response headers.
  std::string resp;
  {
    int64_t deadline = NowMs() + timeout_ms;
    uint8_t buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos) {
      int remaining = static_cast<int>(deadline - NowMs());
      if (remaining <= 0) {
        *err = "websocket handshake timeout";
        client->Close();
        return nullptr;
      }
      int rc = client->ReadExact(buf, 1, deadline);
      if (rc != 1) {
        *err = "websocket handshake read failed";
        client->Close();
        return nullptr;
      }
      resp += static_cast<char>(buf[0]);
      if (resp.size() > 64 * 1024) {
        *err = "websocket handshake response too large";
        client->Close();
        return nullptr;
      }
    }
    (void)buf;
  }
  if (resp.find(" 101") == std::string::npos) {
    *err = "websocket handshake rejected: " + resp.substr(0, resp.find("\r\n"));
    client->Close();
    return nullptr;
  }
  // Verify Sec-WebSocket-Accept = base64(SHA1(key + GUID)).
  {
    uint8_t digest[SHA_DIGEST_LENGTH];
    std::string accept_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    SHA1(reinterpret_cast<const uint8_t*>(accept_src.data()), accept_src.size(), digest);
    std::string want = "Sec-WebSocket-Accept: " + Base64Encode(digest, sizeof(digest));
    if (resp.find(want) == std::string::npos) {
      *err = "websocket handshake accept key mismatch";
      client->Close();
      return nullptr;
    }
  }
  return client;
}

WsClient::~WsClient() {
  Close();
  if (ssl_ != nullptr) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ssl_ctx_ != nullptr) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void WsClient::Close() {
  // Only shutdown the socket here: a blocked Read/Send on another thread
  // unblocks with an error. TLS/fd resources are released in the destructor,
  // which runs once the last shared owner (the reader thread) is gone — this
  // keeps Close safe to call while a read is in flight.
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
  }
}

bool WsClient::WriteAll(const uint8_t* buf, size_t n, int timeout_ms, std::string* err) {
  int64_t deadline = NowMs() + timeout_ms;
  size_t off = 0;
  while (off < n) {
    int remaining = static_cast<int>(deadline - NowMs());
    if (remaining <= 0) {
      *err = "write timeout";
      return false;
    }
    if (PollFd(fd_, POLLOUT, remaining) != 1) {
      *err = "write poll failed or timed out";
      return false;
    }
    ssize_t rc;
    if (ssl_ != nullptr) {
      rc = SSL_write(ssl_, buf + off, static_cast<int>(n - off));
      if (rc <= 0) {
        int ssl_err = SSL_get_error(ssl_, rc);
        if (ssl_err == SSL_ERROR_WANT_WRITE) continue;
        *err = "tls write failed";
        return false;
      }
    } else {
      rc = ::send(fd_, buf + off, n - off, 0);
      if (rc < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        *err = std::string("socket write failed: ") + std::strerror(errno);
        return false;
      }
    }
    off += static_cast<size_t>(rc);
  }
  return true;
}

int WsClient::ReadExact(uint8_t* buf, size_t n, int64_t deadline_ms) {
  size_t off = 0;
  while (off < n) {
    // Drain the buffered leftovers from previous reads first.
    if (!read_buf_.empty()) {
      size_t take = std::min(n - off, read_buf_.size());
      std::memcpy(buf + off, read_buf_.data(), take);
      read_buf_.erase(read_buf_.begin(), read_buf_.begin() + take);
      off += take;
      continue;
    }
    int remaining = static_cast<int>(deadline_ms - NowMs());
    if (remaining <= 0) return 0;
    int prc = PollFd(fd_, POLLIN, remaining);
    if (prc == 0) return 0;
    if (prc < 0) return -1;
    ssize_t rc;
    if (ssl_ != nullptr) {
      rc = SSL_read(ssl_, buf + off, static_cast<int>(n - off));
      if (rc <= 0) {
        int ssl_err = SSL_get_error(ssl_, rc);
        if (ssl_err == SSL_ERROR_WANT_READ) continue;
        return -1;
      }
    } else {
      rc = ::recv(fd_, buf + off, n - off, 0);
      if (rc == 0) return -1;  // orderly shutdown
      if (rc < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return -1;
      }
    }
    off += static_cast<size_t>(rc);
  }
  return 1;
}

int WsClient::Read(WsFrame* out, int timeout_ms) {
  int64_t deadline = NowMs() + timeout_ms;
  while (true) {
    uint8_t head[2];
    int rc = ReadExact(head, 2, deadline);
    if (rc != 1) return rc;

    out->fin = (head[0] & 0x80) != 0;
    out->opcode = head[0] & 0x0F;
    bool masked = (head[1] & 0x80) != 0;
    uint64_t len = head[1] & 0x7F;
    if (len == 126) {
      uint8_t ext[2];
      if (ReadExact(ext, 2, deadline) != 1) return -1;
      len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (ReadExact(ext, 8, deadline) != 1) return -1;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > 64 * 1024 * 1024) return -1;  // sanity bound

    uint8_t mask_key[4] = {0};
    if (masked && ReadExact(mask_key, 4, deadline) != 1) return -1;

    out->payload.resize(len);
    if (len > 0 &&
        ReadExact(reinterpret_cast<uint8_t*>(out->payload.data()), len, deadline) != 1) {
      return -1;
    }
    if (masked) {
      for (size_t i = 0; i < len; i++) {
        out->payload[i] ^= static_cast<char>(mask_key[i % 4]);
      }
    }

    if (out->opcode == 0x9) {
      // Ping: answer with a pong and keep reading.
      std::string err;
      std::string pong = EncodeWsFrame(0xA, out->payload, true);
      std::lock_guard<std::mutex> lock(write_mu_);
      WriteAll(reinterpret_cast<const uint8_t*>(pong.data()), pong.size(),
               static_cast<int>(deadline - NowMs()), &err);
      continue;
    }
    if (out->opcode == 0xA) continue;  // pong: ignore
    return 1;
  }
}

bool WsClient::SendText(const std::string& text, std::string* err) {
  std::string frame = EncodeWsFrame(0x1, text, true);
  std::lock_guard<std::mutex> lock(write_mu_);
  return WriteAll(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
                  write_timeout_ms_, err);
}

bool WsClient::SendBinary(const uint8_t* data, size_t len, std::string* err) {
  std::string frame =
      EncodeWsFrame(0x2, std::string(reinterpret_cast<const char*>(data), len), true);
  std::lock_guard<std::mutex> lock(write_mu_);
  return WriteAll(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
                  write_timeout_ms_, err);
}

std::string GenerateUuid() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  uint8_t b[16];
  for (int i = 0; i < 16; i += 8) {
    uint64_t v = rng();
    std::memcpy(b + i, &v, 8);
  }
  b[6] = (b[6] & 0x0F) | 0x40;  // version 4
  b[8] = (b[8] & 0x3F) | 0x80;  // variant 1
  char out[37];
  std::snprintf(out, sizeof(out),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
                b[11], b[12], b[13], b[14], b[15]);
  return out;
}

}  // namespace internal
}  // namespace trtc_asr
