#include "mock_servers.h"

#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>

#include "trtc_asr/usersig.h"  // Base64Encode
#include "ws_client.h"         // EncodeWsFrame

namespace trtc_asr_test {
namespace {

std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }
  return s;
}

std::string Trim(std::string s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

int BindLoopback(int* port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
  *port = ntohs(addr.sin_port);
  if (listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool WriteAll(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool ReadExact(int fd, uint8_t* buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::recv(fd, buf + off, n - off, 0);
    if (r <= 0) return false;
    off += static_cast<size_t>(r);
  }
  return true;
}

}  // namespace

// --- Mock HTTP server -------------------------------------------------------

std::string CapturedHttpRequest::Header(const std::string& name) const {
  auto it = headers.find(ToLower(name));
  return it == headers.end() ? "" : it->second;
}

std::optional<std::string> CapturedHttpRequest::Query(const std::string& key) const {
  size_t q = target.find('?');
  if (q == std::string::npos) return std::nullopt;
  std::string query = target.substr(q + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos
                                                                  : amp - pos);
    size_t eq = pair.find('=');
    std::string k = PercentDecode(pair.substr(0, eq));
    if (k == key) {
      return eq == std::string::npos ? "" : PercentDecode(pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return std::nullopt;
}

MockHttpServer::MockHttpServer(Handler handler) : handler_(std::move(handler)) {
  listen_fd_ = BindLoopback(&port_);
  thread_ = std::thread([this] { Serve(); });
}

MockHttpServer::~MockHttpServer() {
  shutdown(listen_fd_, SHUT_RDWR);
  close(listen_fd_);
  if (thread_.joinable()) thread_.join();
}

std::string MockHttpServer::Url() const {
  return "http://127.0.0.1:" + std::to_string(port_);
}

std::vector<CapturedHttpRequest> MockHttpServer::Requests() const {
  std::lock_guard<std::mutex> lock(mu_);
  return requests_;
}

void MockHttpServer::Serve() {
  while (true) {
    int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;

    // Read request head.
    std::string head;
    char c;
    bool complete = false;
    while (head.size() < 256 * 1024) {
      if (::recv(fd, &c, 1, 0) != 1) {
        complete = false;
        break;
      }
      head += c;
      if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) {
        complete = true;
        break;
      }
    }
    if (!complete) {
      close(fd);
      continue;
    }

    CapturedHttpRequest req;
    std::map<std::string, std::string> headers;
    size_t line_end = head.find("\r\n");
    {
      std::string request_line = head.substr(0, line_end);
      size_t sp1 = request_line.find(' ');
      size_t sp2 = request_line.find(' ', sp1 + 1);
      req.method = request_line.substr(0, sp1);
      req.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    size_t pos = line_end + 2;
    size_t content_length = 0;
    while (pos + 2 <= head.size()) {
      size_t end = head.find("\r\n", pos);
      if (end == pos) break;
      std::string line = head.substr(pos, end - pos);
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string name = ToLower(Trim(line.substr(0, colon)));
        std::string value = Trim(line.substr(colon + 1));
        headers[name] = value;
        if (name == "content-length") {
          content_length = std::stoul(value);
        }
      }
      pos = end + 2;
    }
    req.headers = headers;
    if (content_length > 0) {
      req.body.resize(content_length);
      if (!ReadExact(fd, reinterpret_cast<uint8_t*>(req.body.data()), content_length)) {
        close(fd);
        continue;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      requests_.push_back(req);
    }
    MockHttpResponse resp = handler_(req);

    std::string reason = resp.status == 200   ? "OK"
                         : resp.status == 400 ? "Bad Request"
                         : resp.status == 500 ? "Internal Server Error"
                                              : "Status";
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + reason +
                      "\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(resp.body.size()) +
                      "\r\nConnection: close\r\n\r\n" + resp.body;
    WriteAll(fd, out);
    close(fd);
  }
}

// --- Mock WebSocket server --------------------------------------------------

bool MockWsSession::Read(int* opcode, std::string* payload) {
  while (true) {
    uint8_t head[2];
    if (!ReadExact(fd_, head, 2)) return false;
    *opcode = head[0] & 0x0F;
    bool masked = (head[1] & 0x80) != 0;
    uint64_t len = head[1] & 0x7F;
    if (len == 126) {
      uint8_t ext[2];
      if (!ReadExact(fd_, ext, 2)) return false;
      len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (!ReadExact(fd_, ext, 8)) return false;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    uint8_t mask_key[4] = {0};
    if (masked && !ReadExact(fd_, mask_key, 4)) return false;
    payload->resize(len);
    if (len > 0 && !ReadExact(fd_, reinterpret_cast<uint8_t*>(payload->data()), len)) {
      return false;
    }
    if (masked) {
      for (size_t i = 0; i < len; i++) {
        (*payload)[i] ^= static_cast<char>(mask_key[i % 4]);
      }
    }
    if (*opcode == 0x9) {  // ping → pong
      std::string pong = trtc_asr::internal::EncodeWsFrame(0xA, *payload, false);
      if (!WriteAll(fd_, pong)) return false;
      continue;
    }
    if (*opcode == 0xA) continue;  // pong
    return true;
  }
}

bool MockWsSession::SendText(const std::string& text) {
  return WriteAll(fd_, trtc_asr::internal::EncodeWsFrame(0x1, text, false));
}

bool MockWsSession::SendBinary(const std::string& data) {
  return WriteAll(fd_, trtc_asr::internal::EncodeWsFrame(0x2, data, false));
}

void MockWsSession::Close() {
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
    fd_ = -1;
  }
}

MockWsServer::MockWsServer(Handler handler) : handler_(std::move(handler)) {
  listen_fd_ = BindLoopback(&port_);
  thread_ = std::thread([this] { Run(); });
}

MockWsServer::~MockWsServer() {
  shutdown(listen_fd_, SHUT_RDWR);
  close(listen_fd_);
  if (thread_.joinable()) thread_.join();
}

std::string MockWsServer::Url() const {
  return "ws://127.0.0.1:" + std::to_string(port_);
}

std::string MockWsServer::RequestTarget() const {
  std::lock_guard<std::mutex> lock(mu_);
  return request_target_;
}

void MockWsServer::Join() {
  if (thread_.joinable()) thread_.join();
}

void MockWsServer::Run() {
  int fd = accept(listen_fd_, nullptr, nullptr);
  if (fd < 0) return;

  // Handshake: read head, answer 101.
  std::string head;
  char c;
  while (head.size() < 256 * 1024) {
    if (::recv(fd, &c, 1, 0) != 1) {
      close(fd);
      return;
    }
    head += c;
    if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
  }

  std::string key;
  {
    size_t line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.find(' ', sp1 + 1);
    {
      std::lock_guard<std::mutex> lock(mu_);
      request_target_ = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    size_t pos = line_end + 2;
    while (pos + 2 <= head.size()) {
      size_t end = head.find("\r\n", pos);
      if (end == pos) break;
      std::string line = head.substr(pos, end - pos);
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string name = ToLower(Trim(line.substr(0, colon)));
        if (name == "sec-websocket-key") {
          key = Trim(line.substr(colon + 1));
        }
      }
      pos = end + 2;
    }
  }

  std::string accept_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const uint8_t*>(accept_src.data()), accept_src.size(), digest);
  std::string accept = trtc_asr::Base64Encode(digest, sizeof(digest));

  std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " +
                     accept + "\r\n\r\n";
  if (!WriteAll(fd, resp)) {
    close(fd);
    return;
  }

  MockWsSession session(fd);
  try {
    handler_(session);
  } catch (...) {
    // A test handler may throw when the client disconnects mid-flow; the
    // session is over either way.
  }
  session.Close();
}

std::string PercentDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.size()) {
      int v = 0;
      std::sscanf(s.substr(i + 1, 2).c_str(), "%02x", &v);
      out += static_cast<char>(v);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace trtc_asr_test
