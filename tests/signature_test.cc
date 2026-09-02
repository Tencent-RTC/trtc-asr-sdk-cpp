#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json_helper.h"
#include "mock_servers.h"
#include "trtc_asr/signature_params.h"

namespace {

using nlohmann::json;
using trtc_asr::SignatureParams;
using trtc_asr::SpeakerRole;

std::optional<std::string> QueryGet(const std::string& qs, const std::string& key) {
  size_t pos = 0;
  while (pos <= qs.size()) {
    size_t amp = qs.find('&', pos);
    std::string pair =
        qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = pair.find('=');
    if (pair.substr(0, eq) == key) {
      return trtc_asr_test::PercentDecode(
          eq == std::string::npos ? "" : pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return std::nullopt;
}

std::vector<std::string> QueryKeys(const std::string& qs) {
  std::vector<std::string> keys;
  size_t pos = 0;
  while (pos <= qs.size()) {
    size_t amp = qs.find('&', pos);
    std::string pair =
        qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    keys.push_back(pair.substr(0, pair.find('=')));
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return keys;
}

TEST(SignatureParams, Defaults) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  EXPECT_EQ(p.app_id, 1300403317);
  EXPECT_EQ(p.engine_model_type, "16k_zh");
  EXPECT_EQ(p.voice_id, "voice-001");
  EXPECT_EQ(p.voice_format, 1);
  EXPECT_EQ(p.need_vad, 1);
  EXPECT_NE(p.timestamp, 0);
  EXPECT_GT(p.expired, p.timestamp);
  EXPECT_GE(p.nonce, 1);
  EXPECT_LE(p.nonce, 9999999);
}

TEST(SignatureParams, QueryStringContainsRequiredKeys) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  std::string qs = p.BuildQueryString();
  EXPECT_FALSE(qs.empty());
  for (const char* key :
       {"secretid=", "timestamp=", "expired=", "nonce=", "engine_model_type=",
        "voice_id="}) {
    EXPECT_NE(qs.find(key), std::string::npos) << "missing " << key;
  }
  EXPECT_NE(qs.find("secretid=1300403317"), std::string::npos);
  EXPECT_EQ(qs.find("signature="), std::string::npos);
}

TEST(SignatureParams, QueryStringWithSignature) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  p.sdk_app_id = 1400000000;
  std::string user_sig = "eJwtzDEOgCAQRdG9UBMH-test-user-sig";
  std::string qs = p.BuildQueryStringWithSignature(user_sig);

  ASSERT_TRUE(QueryGet(qs, "signature").has_value());
  EXPECT_EQ(*QueryGet(qs, "signature"), user_sig);
  ASSERT_TRUE(QueryGet(qs, "usersig").has_value());
  EXPECT_EQ(*QueryGet(qs, "usersig"), user_sig);
  ASSERT_TRUE(QueryGet(qs, "sdkappid").has_value());
  EXPECT_EQ(*QueryGet(qs, "sdkappid"), "1400000000");
  for (const char* key : {"secretid", "timestamp", "expired", "nonce"}) {
    EXPECT_TRUE(QueryGet(qs, key).has_value()) << "missing " << key;
  }
}

TEST(SignatureParams, SecretKeyNeverInQueryString) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  std::string qs = p.BuildQueryStringWithSignature("some-user-sig");
  EXPECT_EQ(qs.find("secret_key"), std::string::npos);
  EXPECT_EQ(qs.find("secretkey"), std::string::npos);
}

TEST(SignatureParams, OmitsUnsetOptionalParams) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  std::string qs = p.BuildQueryString();
  for (const char* key : {"speaker_diarization", "speaker_number", "speaker_roles",
                          "voiceprintids", "noise_threshold", "vad_level",
                          "filter_empty_result", "hotword_list", "replace_text_id",
                          "input_sample_rate", "sdkappid", "language"}) {
    EXPECT_EQ(qs.find(std::string(key) + "="), std::string::npos)
        << key << " should be omitted: " << qs;
  }
}

TEST(SignatureParams, SpeakerDiarizationCluster) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  p.speaker_diarization = trtc_asr::kSpeakerDiarizationCluster;
  p.speaker_number = 2;
  // Enrollment input only applies to mode 3 and must not leak into mode 1.
  p.speaker_roles = {{"teacher", "https://example.com/a.wav"}};
  p.voiceprint_ids = {"vp-1"};

  std::string qs = p.BuildQueryString();
  ASSERT_TRUE(QueryGet(qs, "speaker_diarization").has_value());
  EXPECT_EQ(*QueryGet(qs, "speaker_diarization"), "1");
  ASSERT_TRUE(QueryGet(qs, "speaker_number").has_value());
  EXPECT_EQ(*QueryGet(qs, "speaker_number"), "2");
  EXPECT_FALSE(QueryGet(qs, "speaker_roles").has_value());
  EXPECT_FALSE(QueryGet(qs, "voiceprintids").has_value());
}

TEST(SignatureParams, SpeakerDiarizationVoiceprint) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  p.speaker_diarization = trtc_asr::kSpeakerDiarizationVoiceprint;
  p.speaker_roles = {{"teacher", "https://example.com/a.wav"},
                     {"student", "https://example.com/b.wav"}};
  p.voiceprint_ids = {"vp-1", "vp-2"};
  p.speaker_number = 0;  // auto detection: omitted

  std::string qs = p.BuildQueryString();
  ASSERT_TRUE(QueryGet(qs, "speaker_diarization").has_value());
  EXPECT_EQ(*QueryGet(qs, "speaker_diarization"), "3");
  EXPECT_FALSE(QueryGet(qs, "speaker_number").has_value());

  auto roles = json::parse(*QueryGet(qs, "speaker_roles"));
  ASSERT_EQ(roles.size(), 2);
  EXPECT_EQ(roles[0]["RoleName"], "teacher");
  EXPECT_EQ(roles[1]["AudioUrl"], "https://example.com/b.wav");

  auto ids = json::parse(*QueryGet(qs, "voiceprintids"));
  ASSERT_EQ(ids.size(), 2);
  EXPECT_EQ(ids[0], "vp-1");
}

TEST(SignatureParams, TriStateVadTuning) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  p.vad_level = 0;
  p.noise_threshold = 0.0;
  p.filter_empty_result = 0;

  // An explicit 0 differs from "unset": the server defaults vad_level to 1
  // and filter_empty_result to 1, so both must reach the wire.
  std::string qs = p.BuildQueryString();
  ASSERT_TRUE(QueryGet(qs, "vad_level").has_value());
  EXPECT_EQ(*QueryGet(qs, "vad_level"), "0");
  ASSERT_TRUE(QueryGet(qs, "filter_empty_result").has_value());
  EXPECT_EQ(*QueryGet(qs, "filter_empty_result"), "0");
  // Go strconv.FormatFloat('f', 3): "0.000".
  ASSERT_TRUE(QueryGet(qs, "noise_threshold").has_value());
  EXPECT_EQ(*QueryGet(qs, "noise_threshold"), "0.000");

  p.noise_threshold = 1.5;
  qs = p.BuildQueryString();
  EXPECT_EQ(*QueryGet(qs, "noise_threshold"), "1.500");
}

TEST(SignatureParams, AdvancedOptionalParams) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  p.hotword_list = "腾讯云|5,ASR|11";
  p.replace_text_id = "replace-1";
  p.input_sample_rate = 8000;
  p.language = "zh";

  std::string qs = p.BuildQueryString();
  ASSERT_TRUE(QueryGet(qs, "hotword_list").has_value());
  EXPECT_EQ(*QueryGet(qs, "hotword_list"), "腾讯云|5,ASR|11");
  EXPECT_EQ(*QueryGet(qs, "replace_text_id"), "replace-1");
  EXPECT_EQ(*QueryGet(qs, "input_sample_rate"), "8000");
  EXPECT_EQ(*QueryGet(qs, "language"), "zh");
}

TEST(SignatureParams, QueryKeysAreSorted) {
  SignatureParams p(1300403317, "16k_zh", "voice-001");
  p.hotword_id = "hw";
  std::string qs = p.BuildQueryStringWithSignature("sig");
  auto keys = QueryKeys(qs);
  auto sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(keys, sorted) << "query keys must be sorted like Go's sort.Strings";
}

TEST(SignatureParams, QueryEscapeMatchesGoSemantics) {
  EXPECT_EQ(trtc_asr::QueryEscape("abcXYZ019-_.~"), "abcXYZ019-_.~");
  EXPECT_EQ(trtc_asr::QueryEscape("a b"), "a+b");
  EXPECT_EQ(trtc_asr::QueryEscape("a+b"), "a%2Bb");
  EXPECT_EQ(trtc_asr::QueryEscape("词|5,"), "%E8%AF%8D%7C5%2C");
  EXPECT_EQ(trtc_asr::QueryEscape("100%"), "100%25");
}

}  // namespace
