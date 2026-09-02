#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "json_helper.h"
#include "mock_servers.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/sentence_recognizer.h"
#include "trtc_asr/usersig.h"

namespace {

using nlohmann::json;
using trtc_asr::ASRError;
using trtc_asr::SentenceRecognitionRequest;
using trtc_asr::SentenceRecognizer;
using trtc_asr_test::MockHttpResponse;
using trtc_asr_test::MockHttpServer;

trtc_asr::Credential TestCredential() {
  return trtc_asr::Credential(1300000000, 1400000000, "test-secret");
}

std::vector<uint8_t> Bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

void ExpectError(const std::function<void()>& fn, int code,
                 const std::string& want_message) {
  try {
    fn();
    FAIL() << "expected ASRError " << code;
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), code);
    if (!want_message.empty()) {
      EXPECT_NE(std::string(e.what()).find(want_message), std::string::npos)
          << "error: " << e.what() << "\nwant: " << want_message;
    }
  }
}

TEST(SentenceRecognizer, RejectsInvalidRequests) {
  SentenceRecognizer r(TestCredential());

  ExpectError([&] { r.Recognize(SentenceRecognitionRequest{}); },
              trtc_asr::kErrInvalidParam, "EngServiceType is required");

  SentenceRecognitionRequest req;
  req.eng_service_type = "16k_zh";
  ExpectError([&] { r.Recognize(req); }, trtc_asr::kErrInvalidParam,
              "VoiceFormat is required");

  req.voice_format = "pcm";
  req.source_type = SentenceRecognizer::kSourceTypeURL;
  ExpectError([&] { r.Recognize(req); }, trtc_asr::kErrInvalidParam,
              "Url is required when SourceType=0");

  req.source_type = SentenceRecognizer::kSourceTypeData;
  ExpectError([&] { r.Recognize(req); }, trtc_asr::kErrInvalidParam,
              "Data is required when SourceType=1");
}

TEST(SentenceRecognizer, RecognizeDataRejectsEmptyAndOversized) {
  SentenceRecognizer r(TestCredential());
  ExpectError([&] { r.RecognizeData({}, "pcm", "16k_zh"); },
              trtc_asr::kErrInvalidParam, "audio data is empty");
  std::vector<uint8_t> big(3 * 1024 * 1024 + 1, 0);
  ExpectError([&] { r.RecognizeData(big, "pcm", "16k_zh"); },
              trtc_asr::kErrInvalidParam, "3MB");
}

TEST(SentenceRecognizer, RecognizeUrlRejectsEmptyUrl) {
  SentenceRecognizer r(TestCredential());
  ExpectError([&] { r.RecognizeURL("", "wav", "16k_zh"); },
              trtc_asr::kErrInvalidParam, "audio URL is empty");
}

TEST(SentenceRecognizer, RecognizeDataSuccess) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    EXPECT_EQ(req.method, "POST");
    EXPECT_TRUE(req.target.rfind("/v1/SentenceRecognition?", 0) == 0);
    EXPECT_EQ(req.Header("Content-Type"), "application/json; charset=utf-8");
    EXPECT_FALSE(req.Header("X-TRTC-SdkAppId").empty());
    EXPECT_FALSE(req.Header("X-TRTC-UserSig").empty());
    for (const char* key : {"AppId", "Secretid", "RequestId", "Timestamp"}) {
      EXPECT_TRUE(req.Query(key).has_value()) << "missing " << key;
    }

    json body = json::parse(req.body);
    EXPECT_EQ(body["EngSerViceType"], "16k_zh_en");
    EXPECT_EQ(body["SourceType"], 1);
    EXPECT_EQ(body["VoiceFormat"], "pcm");
    std::string raw = trtc_asr::Base64Decode(body["Data"].get<std::string>());
    EXPECT_EQ(raw, "fake-pcm-audio");
    EXPECT_EQ(body["DataLen"], 14);

    return MockHttpResponse{200,
                            R"({"Response":{"Result":"今天天气不错。","AudioDuration":2380,"WordSize":1,"WordList":[{"Word":"今天","StartTime":200,"EndTime":500}],"RequestId":"req-1"}})"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto result = r.RecognizeData(Bytes("fake-pcm-audio"), "pcm", "16k_zh_en");
  EXPECT_EQ(result.result, "今天天气不错。");
  EXPECT_EQ(result.audio_duration, 2380);
  EXPECT_EQ(result.word_size, 1);
  ASSERT_EQ(result.word_list.size(), 1);
  EXPECT_EQ(result.word_list[0].word, "今天");
  EXPECT_EQ(result.request_id, "req-1");
}

TEST(SentenceRecognizer, RecognizeUrlSuccess) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    json body = json::parse(req.body);
    EXPECT_EQ(body["SourceType"], 0);
    EXPECT_EQ(body["Url"], "https://example.com/test.wav");
    return MockHttpResponse{
        200, R"({"Response":{"Result":"hello","AudioDuration":1000,"RequestId":"req-2"}})"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());
  auto result = r.RecognizeURL("https://example.com/test.wav", "wav", "16k_zh_en");
  EXPECT_EQ(result.result, "hello");
}

TEST(SentenceRecognizer, RecognizeServerError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Error":{"Code":"4002","Message":"鉴权失败"},"RequestId":"req-err"}})"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError([&] { r.RecognizeData(Bytes("fake-audio"), "pcm", "16k_zh_en"); },
              trtc_asr::kErrServerError, "4002");
}

TEST(SentenceRecognizer, RecognizeHttpError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{500, "internal server error"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError([&] { r.RecognizeData(Bytes("fake-audio"), "pcm", "16k_zh_en"); },
              trtc_asr::kErrServerError, "500");
}

TEST(SentenceRecognizer, RecognizeDataWithOptions) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    json body = json::parse(req.body);
    EXPECT_EQ(body["FilterDirty"], 1);
    EXPECT_EQ(body["WordInfo"], 2);
    EXPECT_EQ(body["HotwordId"], "hw-123");
    return MockHttpResponse{200,
                            R"({"Response":{"Result":"ok","AudioDuration":10,"RequestId":"r"}})"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  SentenceRecognitionRequest req;
  req.eng_service_type = "16k_zh_en";
  req.voice_format = "pcm";
  req.filter_dirty = 1;
  req.word_info = 2;
  req.hotword_id = "hw-123";
  auto result = r.RecognizeDataWithOptions(Bytes("fake-audio"), &req);
  EXPECT_EQ(result.result, "ok");
}

TEST(SentenceRecognizer, RecognizeDataWithOptionsNullRequest) {
  SentenceRecognizer r(TestCredential());
  ExpectError([&] { r.RecognizeDataWithOptions(Bytes("x"), nullptr); },
              trtc_asr::kErrInvalidParam, "request is null");
}

TEST(SentenceRecognizer, MalformedApiErrorFieldsConvergeToAsrError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    // Error.Code numeric / Error.Message array / RequestId object.
    return MockHttpResponse{
        200,
        R"({"Response":{"Error":{"Code":123,"Message":["bad"]},"RequestId":{"x":1}}})"};
  });
  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());
  // Must converge to ASRError (not leak nlohmann::type_error).
  ExpectError([&] { r.RecognizeData(Bytes("fake-audio"), "pcm", "16k_zh"); },
              trtc_asr::kErrServerError, "server error");
}

TEST(SentenceRecognizer, MalformedFieldTypesConvergeToAsrError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    // AudioDuration as a string is a field-type error.
    return MockHttpResponse{
        200, R"({"Response":{"Result":"ok","AudioDuration":"oops","RequestId":"r"}})"};
  });
  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());
  ExpectError([&] { r.RecognizeData(Bytes("fake-audio"), "pcm", "16k_zh"); },
              trtc_asr::kErrReadFailed, "unmarshal response failed");
}

TEST(SentenceRecognizer, PresetUserSigSentVerbatim) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{200, R"({"Response":{"Result":"ok","RequestId":"r"}})"};
  });

  auto cred = TestCredential();
  cred.set_user_sig("preset-user-sig-value");
  SentenceRecognizer r(cred);
  r.SetEndpoint(server.Url());

  r.RecognizeData(Bytes("fake-audio"), "pcm", "16k_zh");
  ASSERT_EQ(server.Requests().size(), 1);
  EXPECT_EQ(server.Requests()[0].Header("X-TRTC-UserSig"), "preset-user-sig-value");
}

}  // namespace
