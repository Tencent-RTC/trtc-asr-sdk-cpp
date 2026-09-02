#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "mock_servers.h"
#include "sdkinfo.h"
#include "trtc_asr/credential.h"
#include "trtc_asr/file_recognizer.h"
#include "trtc_asr/sentence_recognizer.h"
#include "trtc_asr/speech_recognizer.h"
#include "trtc_asr/version.h"

namespace {

using trtc_asr::CreateRecTaskRequest;
using trtc_asr::FileRecognizer;
using trtc_asr::SentenceRecognitionRequest;
using trtc_asr::SentenceRecognizer;
using trtc_asr::SpeechRecognizer;
using trtc_asr_test::MockHttpResponse;
using trtc_asr_test::MockHttpServer;
using trtc_asr_test::MockWsServer;
using trtc_asr_test::MockWsSession;

trtc_asr::Credential TestCredential() {
  return trtc_asr::Credential(1300000000, 1400000000, "test-secret");
}

/// Parses a request target ("/path?a=1&b=2") into decoded key/value pairs.
std::map<std::string, std::string> QueryOf(const std::string& target) {
  std::map<std::string, std::string> out;
  size_t q = target.find('?');
  if (q == std::string::npos) return out;
  std::string query = target.substr(q + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string pair = query.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    if (!pair.empty()) {
      size_t eq = pair.find('=');
      std::string k = trtc_asr_test::PercentDecode(pair.substr(0, eq));
      std::string v =
          eq == std::string::npos
              ? ""
              : trtc_asr_test::PercentDecode(pair.substr(eq + 1));
      out[k] = v;
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return out;
}

/// Checks that a captured request target carries the SDK identification the
/// service relies on for diagnostics.
void ExpectSdkReportParams(const std::string& target) {
  auto q = QueryOf(target);
  EXPECT_EQ(q["sdk_lang"], "cpp") << target;
  EXPECT_EQ(q["sdk_type"], "server") << target;
  EXPECT_EQ(q["version"], TRTC_ASR_VERSION_STRING) << target;
  EXPECT_EQ(q["platform"], trtc_asr::internal::SdkPlatform()) << target;
}

class SilentListener : public trtc_asr::SpeechRecognitionListener {};

TEST(SdkInfo, ConstantsMatchVersionHeader) {
  EXPECT_STREQ(trtc_asr::internal::kSdkVersion, TRTC_ASR_VERSION_STRING);
  EXPECT_STREQ(trtc_asr::internal::kSdkLanguage, "cpp");
  EXPECT_STREQ(trtc_asr::internal::kSdkType, "server");
}

TEST(SdkInfo, PlatformIsNormalized) {
  // Any platform we build on must map into the service vocabulary; unknown
  // platforms are reported verbatim rather than mislabeled.
  const std::string platform = trtc_asr::internal::SdkPlatform();
  static const std::vector<std::string> kKnown = {"windows", "linux", "mac",
                                                  "android", "ios"};
#if defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__) || \
    defined(__linux__)
  EXPECT_NE(std::find(kKnown.begin(), kKnown.end(), platform), kKnown.end())
      << "unexpected platform: " << platform;
#endif
  EXPECT_FALSE(platform.empty());
}

TEST(SdkInfo, ReportQueryIsSortedAndEncoded) {
  // The three transports concatenate this fragment onto an existing query, so
  // it must carry no leading separator and keep the key order stable.
  EXPECT_EQ(trtc_asr::internal::SdkReportQuery(),
            std::string("platform=") + trtc_asr::internal::SdkPlatform() +
                "&sdk_lang=cpp&sdk_type=server&version=" +
                TRTC_ASR_VERSION_STRING);
}

TEST(SdkInfo, SpeechRecognizerHandshakeReportsSdkIdentity) {
  MockWsServer server([](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(
            R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  SilentListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint(server.Url());
  r.SetVoiceId("voice-sdkinfo");
  r.SetStopTimeout(std::chrono::milliseconds(2000));
  r.Start();
  r.Stop();
  server.Join();

  const std::string target = server.RequestTarget();
  ExpectSdkReportParams(target);

  // The pre-existing protocol parameters must survive the addition.
  auto q = QueryOf(target);
  EXPECT_EQ(q["voice_id"], "voice-sdkinfo");
  EXPECT_EQ(q["engine_model_type"], "16k_zh_en");
  EXPECT_EQ(q["secretid"], "1300000000");
  for (const char* key : {"signature", "usersig", "timestamp", "expired",
                          "nonce", "voice_format", "needvad"}) {
    EXPECT_FALSE(q[key].empty()) << "missing " << key << " in " << target;
  }
}

TEST(SdkInfo, SentenceRecognizerReportsSdkIdentity) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200, R"({"Response":{"RequestId":"req-1","Result":"hello"}})"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  SentenceRecognitionRequest req;
  req.eng_service_type = "16k_zh";
  req.voice_format = "wav";
  req.source_type = SentenceRecognizer::kSourceTypeURL;
  req.url = "https://example.com/test.wav";
  r.Recognize(req);

  auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 1u);
  const std::string& target = requests[0].target;
  ExpectSdkReportParams(target);
  EXPECT_EQ(target.rfind("/v1/SentenceRecognition?", 0), 0u) << target;
  for (const char* key : {"AppId", "Secretid", "RequestId", "Timestamp"}) {
    ASSERT_TRUE(requests[0].Query(key).has_value()) << "missing " << key;
    EXPECT_FALSE(requests[0].Query(key)->empty()) << "empty " << key;
  }
}

TEST(SdkInfo, FileRecognizerReportsSdkIdentityOnBothEndpoints) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    if (req.target.rfind("/v1/CreateRecTask", 0) == 0) {
      return MockHttpResponse{
          200,
          R"({"Response":{"Data":{"RecTaskId":"task-42"},"RequestId":"req-1"}})"};
    }
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-42","Status":2,"StatusStr":"success","Result":"hello"},"RequestId":"req-2"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  CreateRecTaskRequest req;
  req.engine_model_type = "16k_zh";
  req.channel_num = 1;
  req.res_text_format = 1;
  req.source_type = 0;
  req.url = "https://example.com/test.wav";
  std::string task_id = r.CreateTask(req);
  r.DescribeTaskStatus(task_id);

  auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  // Both CreateRecTask and DescribeTaskStatus go through the shared request
  // path, so both must report.
  EXPECT_EQ(requests[0].target.rfind("/v1/CreateRecTask?", 0), 0u);
  EXPECT_EQ(requests[1].target.rfind("/v1/DescribeTaskStatus?", 0), 0u);
  for (const auto& captured : requests) {
    ExpectSdkReportParams(captured.target);
    for (const char* key : {"AppId", "Secretid", "RequestId", "Timestamp"}) {
      ASSERT_TRUE(captured.Query(key).has_value())
          << "missing " << key << " in " << captured.target;
      EXPECT_FALSE(captured.Query(key)->empty()) << "empty " << key;
    }
  }
}

}  // namespace
