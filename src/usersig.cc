#include "trtc_asr/usersig.h"

#include <cstring>
#include <stdexcept>
#include <vector>
#include <zlib.h>

#include "trtc_asr/errors.h"

namespace trtc_asr {
namespace {

// --- SHA-256 ----------------------------------------------------------------
// Compact public-domain-style SHA-256 (single-shot), so the SDK does not
// depend on OpenSSL's deprecated low-level HMAC API.

class Sha256 {
 public:
  Sha256() { Reset(); }

  void Update(const uint8_t* data, size_t len) {
    bit_len_ += len * 8;
    while (len > 0) {
      size_t take = std::min(len, sizeof(state_buf_) - buf_len_);
      std::memcpy(state_buf_ + buf_len_, data, take);
      buf_len_ += take;
      data += take;
      len -= take;
      if (buf_len_ == sizeof(state_buf_)) {
        Compress(state_buf_);
        buf_len_ = 0;
      }
    }
  }

  void Final(uint8_t out[32]) {
    uint8_t pad = 0x80;
    Update(&pad, 1);
    // Adjust: Final must not count padding/length into bit_len_.
    bit_len_ -= 8;  // undo the padding byte count
    while (buf_len_ != 56) {
      uint8_t zero = 0;
      UpdateNoCount(&zero, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; i--) {
      len_bytes[i] = static_cast<uint8_t>(bit_len_ & 0xFF);
      bit_len_ >>= 8;
    }
    for (int i = 0; i < 8; i++) UpdateNoCount(&len_bytes[i], 1);
    for (int i = 0; i < 8; i++) {
      out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
  }

  static void Hash(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256 s;
    s.Update(data, len);
    s.Final(out);
  }

  static void Hash(const std::string& s, uint8_t out[32]) {
    Hash(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
  }

 private:
  void Reset() {
    h_[0] = 0x6a09e667;
    h_[1] = 0xbb67ae85;
    h_[2] = 0x3c6ef372;
    h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f;
    h_[5] = 0x9b05688c;
    h_[6] = 0x1f83d9ab;
    h_[7] = 0x5be0cd19;
    buf_len_ = 0;
    bit_len_ = 0;
  }

  void UpdateNoCount(const uint8_t* data, size_t len) {
    while (len > 0) {
      size_t take = std::min(len, sizeof(state_buf_) - buf_len_);
      std::memcpy(state_buf_ + buf_len_, data, take);
      buf_len_ += take;
      data += take;
      len -= take;
      if (buf_len_ == sizeof(state_buf_)) {
        Compress(state_buf_);
        buf_len_ = 0;
      }
    }
  }

  static uint32_t RotR(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void Compress(const uint8_t block[64]) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];

    for (int i = 0; i < 64; i++) {
      uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t state_buf_[64];
  size_t buf_len_ = 0;
  uint64_t bit_len_ = 0;
};

void HmacSha256(const std::string& key, const std::string& message, uint8_t out[32]) {
  uint8_t key_block[64] = {0};
  if (key.size() > 64) {
    Sha256::Hash(key, key_block);  // keys longer than the block size are hashed
  } else {
    std::memcpy(key_block, key.data(), key.size());
  }

  std::vector<uint8_t> inner(64 + message.size());
  for (int i = 0; i < 64; i++) inner[i] = key_block[i] ^ 0x36;
  std::memcpy(inner.data() + 64, message.data(), message.size());
  uint8_t inner_hash[32];
  Sha256::Hash(inner.data(), inner.size(), inner_hash);

  std::vector<uint8_t> outer(64 + 32);
  for (int i = 0; i < 64; i++) outer[i] = key_block[i] ^ 0x5c;
  std::memcpy(outer.data() + 64, inner_hash, 32);
  Sha256::Hash(outer.data(), outer.size(), out);
}

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string ZlibCompress(const std::string& data) {
  uLongf bound = compressBound(data.size());
  std::vector<uint8_t> out(bound);
  int rc = compress2(out.data(), &bound,
                     reinterpret_cast<const Bytef*>(data.data()), data.size(),
                     Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK) {
    throw ASRError(kErrInvalidParam, "zlib compress failed: " + std::to_string(rc));
  }
  out.resize(bound);
  return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

std::string Base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
    out += kB64Alphabet[(v >> 18) & 0x3F];
    out += kB64Alphabet[(v >> 12) & 0x3F];
    out += (i + 1 < len) ? kB64Alphabet[(v >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? kB64Alphabet[v & 0x3F] : '=';
  }
  return out;
}

std::string Base64Decode(const std::string& s) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    int v = B64Value(c);
    if (v < 0) {
      throw ASRError(kErrInvalidParam, "base64 decode failed: invalid character");
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

std::string Base64UrlEncode(const uint8_t* data, size_t len) {
  std::string out = Base64Encode(data, len);
  for (char& c : out) {
    if (c == '+') c = '*';
    else if (c == '/') c = '-';
    else if (c == '=') c = '_';
  }
  return out;
}

std::string Base64UrlEncode(const std::string& data) {
  return Base64UrlEncode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string Base64UrlDecode(const std::string& s) {
  std::string std_form = s;
  for (char& c : std_form) {
    if (c == '_') c = '=';
    else if (c == '-') c = '/';
    else if (c == '*') c = '+';
  }
  return Base64Decode(std_form);
}

std::string GenUserSigAt(int64_t sdk_app_id, const std::string& key,
                         const std::string& user_id, int64_t expire, int64_t now) {
  if (key.empty()) {
    throw ASRError(kErrInvalidParam, "secret key is empty");
  }
  if (user_id.empty()) {
    throw ASRError(kErrInvalidParam, "user id is empty");
  }

  std::string content = "TLS.identifier:" + user_id + "\n" +
                        "TLS.sdkappid:" + std::to_string(sdk_app_id) + "\n" +
                        "TLS.time:" + std::to_string(now) + "\n" +
                        "TLS.expire:" + std::to_string(expire) + "\n";
  uint8_t sig[32];
  HmacSha256(key, content, sig);

  std::string doc = std::string("{") +
      "\"TLS.ver\":\"2.0\"," +
      "\"TLS.identifier\":\"" + JsonEscape(user_id) + "\"," +
      "\"TLS.sdkappid\":" + std::to_string(sdk_app_id) + "," +
      "\"TLS.expire\":" + std::to_string(expire) + "," +
      "\"TLS.time\":" + std::to_string(now) + "," +
      "\"TLS.sig\":\"" + Base64Encode(sig, sizeof(sig)) + "\"" +
      "}\n";

  return Base64UrlEncode(ZlibCompress(doc));
}

std::string GenUserSig(int64_t sdk_app_id, const std::string& key,
                       const std::string& user_id, int64_t expire) {
  if (expire <= 0) {
    expire = kDefaultExpire;
  }
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  return GenUserSigAt(sdk_app_id, key, user_id, expire, now);
}

}  // namespace trtc_asr
