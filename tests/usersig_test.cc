#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>  // for EVP-based reference computation below
#include <zlib.h>

#include "json_helper.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/usersig.h"

namespace {

using nlohmann::json;

// Inflates zlib data (what the server does after base64url-decoding a
// UserSig).
std::string ZlibInflate(const std::string& data) {
  z_stream zs = {};
  inflateInit(&zs);
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  std::string out;
  char buf[4096];
  int rc;
  do {
    zs.next_out = reinterpret_cast<Bytef*>(buf);
    zs.avail_out = sizeof(buf);
    rc = inflate(&zs, Z_NO_FLUSH);
    out.append(buf, sizeof(buf) - zs.avail_out);
  } while (rc == Z_OK);
  inflateEnd(&zs);
  EXPECT_EQ(rc, Z_STREAM_END);
  return out;
}

json DecodeSig(const std::string& sig) {
  return json::parse(ZlibInflate(trtc_asr::Base64UrlDecode(sig)));
}

// Reference HMAC-SHA256 via OpenSSL EVP_MAC (cross-checks the SDK's
// implementation instead of re-implementing the same code in the test).
std::string ReferenceHmacSha256(const std::string& key, const std::string& msg) {
  EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string("digest", const_cast<char*>("SHA256"), 0),
      OSSL_PARAM_construct_end()};
  EXPECT_TRUE(EVP_MAC_init(ctx, reinterpret_cast<const unsigned char*>(key.data()),
                           key.size(), params));
  EXPECT_TRUE(EVP_MAC_update(ctx, reinterpret_cast<const unsigned char*>(msg.data()),
                             msg.size()));
  unsigned char out[EVP_MAX_MD_SIZE];
  size_t out_len = 0;
  EXPECT_TRUE(EVP_MAC_final(ctx, out, &out_len, sizeof(out)));
  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);
  return std::string(reinterpret_cast<char*>(out), out_len);
}

TEST(UserSig, StructureAndSignature) {
  int64_t sdk_app_id = 1400000000;
  std::string key = "test-secret-key-for-unit-testing";
  std::string user_id = "test-user-001";
  int64_t expire = 86400;
  int64_t now = 1756800000;  // fixed timestamp for determinism

  std::string sig = trtc_asr::GenUserSigAt(sdk_app_id, key, user_id, expire, now);
  json doc = DecodeSig(sig);

  EXPECT_EQ(doc["TLS.ver"], "2.0");
  EXPECT_EQ(doc["TLS.identifier"], user_id);
  EXPECT_EQ(doc["TLS.sdkappid"], sdk_app_id);
  EXPECT_EQ(doc["TLS.expire"], expire);
  EXPECT_EQ(doc["TLS.time"], now);
  EXPECT_FALSE(doc.contains("TLS.userbuf"));

  // TLS.sig is the standard base64 of the HMAC-SHA256 over the documented
  // content string — cross-checked against OpenSSL.
  std::string content = "TLS.identifier:" + user_id + "\n" +
                        "TLS.sdkappid:" + std::to_string(sdk_app_id) + "\n" +
                        "TLS.time:" + std::to_string(now) + "\n" +
                        "TLS.expire:" + std::to_string(expire) + "\n";
  std::string want = trtc_asr::Base64Encode(
      reinterpret_cast<const uint8_t*>(ReferenceHmacSha256(key, content).data()), 32);
  EXPECT_EQ(doc["TLS.sig"], want);
}

TEST(UserSig, DeterministicForFixedTime) {
  std::string a = trtc_asr::GenUserSigAt(1400000000, "key", "user", 86400, 1756800000);
  std::string b = trtc_asr::GenUserSigAt(1400000000, "key", "user", 86400, 1756800000);
  EXPECT_EQ(a, b);
}

TEST(UserSig, DefaultExpire) {
  std::string sig = trtc_asr::GenUserSig(1400000000, "key", "user", 0);
  json doc = DecodeSig(sig);
  EXPECT_EQ(doc["TLS.expire"], trtc_asr::kDefaultExpire);
}

TEST(UserSig, VariousInputs) {
  struct Case {
    int64_t app_id;
    std::string key;
    std::string user;
  };
  for (const auto& c : std::vector<Case>{
           {1400000001, "key1", "user1"},
           {1400000002, "key2", "user2"},
           {1400000003, "key-with-special-chars!@#$%", "user-with-dashes"},
       }) {
    json doc = DecodeSig(trtc_asr::GenUserSig(c.app_id, c.key, c.user, 86400));
    EXPECT_EQ(doc["TLS.sdkappid"], c.app_id);
    EXPECT_EQ(doc["TLS.identifier"], c.user);
  }
}

TEST(UserSig, RejectsEmptyKeyOrUser) {
  EXPECT_THROW(trtc_asr::GenUserSig(1, "", "user", 86400), trtc_asr::ASRError);
  EXPECT_THROW(trtc_asr::GenUserSig(1, "key", "", 86400), trtc_asr::ASRError);
}

TEST(UserSig, Base64UrlRoundTripAndAlphabet) {
  std::string data = "\xfb\xff\xff\x3e\x80";
  std::string std_form = trtc_asr::Base64Encode(
      reinterpret_cast<const uint8_t*>(data.data()), data.size());
  EXPECT_TRUE(std_form.find('+') != std::string::npos ||
              std_form.find('/') != std::string::npos ||
              std_form.find('=') != std::string::npos);

  std::string encoded = trtc_asr::Base64UrlEncode(
      reinterpret_cast<const uint8_t*>(data.data()), data.size());
  EXPECT_EQ(encoded.find('+'), std::string::npos);
  EXPECT_EQ(encoded.find('/'), std::string::npos);
  EXPECT_EQ(encoded.find('='), std::string::npos);

  EXPECT_EQ(trtc_asr::Base64UrlDecode(encoded), data);
}

}  // namespace
