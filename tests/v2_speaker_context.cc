// v2 断点续传：query 参数下发、本地校验与首响应 speaker_continue 捕获。

#include "trtc_asr/speech_recognizer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "mock_servers.h"
#include "trtc_asr/signature_params.h"
#include "trtc_asr/v3.h"

namespace {

using trtc_asr::SpeechRecognizer;

trtc_asr::Credential TestCredential() {
  return trtc_asr::v3::NewCredential(1400000000, "test-secret");
}

class NopListener : public trtc_asr::SpeechRecognitionListener {
 public:
  void OnRecognitionStart(const trtc_asr::SpeechRecognitionResponse&) override {}
  void OnSentenceBegin(const trtc_asr::SpeechRecognitionResponse&) override {}
  void OnRecognitionResultChange(const trtc_asr::SpeechRecognitionResponse&) override {}
  void OnSentenceEnd(const trtc_asr::SpeechRecognitionResponse&) override {}
  void OnRecognitionComplete(const trtc_asr::SpeechRecognitionResponse&) override {}
  void OnFail(const trtc_asr::SpeechRecognitionResponse*,
              const trtc_asr::ASRError&) override {}
};

TEST(V2SpeakerContext, QueryEmitsContextParams) {
  trtc_asr::SignatureParams p(1400000000, "bigmodel", "v1");
  p.speaker_diarization = trtc_asr::kSpeakerDiarizationCluster;
  p.enable_speaker_context = trtc_asr::kSpeakerContextSync;
  p.speaker_context_id = "abc123";
  const std::string query = p.BuildQueryString();
  EXPECT_NE(query.find("enable_speaker_context=1"), std::string::npos);
  EXPECT_NE(query.find("speaker_context_id=abc123"), std::string::npos);

  // 异步模式值为 2
  p.enable_speaker_context = trtc_asr::kSpeakerContextAsync;
  EXPECT_NE(p.BuildQueryString().find("enable_speaker_context=2"),
            std::string::npos);
}

TEST(V2SpeakerContext, QueryOmitsContextParamsWhenOff) {
  trtc_asr::SignatureParams p(1400000000, "bigmodel", "v1");
  p.speaker_diarization = trtc_asr::kSpeakerDiarizationCluster;
  p.speaker_context_id = "abc123";  // 未开 context 时即使带了 id 也不下发
  EXPECT_EQ(p.BuildQueryString().find("speaker_context"), std::string::npos);
}

TEST(V2SpeakerContext, EnableSpeakerContextRequiresDiarization) {
  NopListener listener;
  SpeechRecognizer r(TestCredential(), "bigmodel", &listener);
  r.SetEnableSpeakerContext(trtc_asr::kSpeakerContextSync);
  try {
    r.Start();
    FAIL() << "expected Start to throw";
  } catch (const trtc_asr::ASRError& e) {
    EXPECT_NE(std::string(e.what()).find("SetSpeakerDiarization"),
              std::string::npos)
        << e.what();
  }
}

TEST(V2SpeakerContext, EnableSpeakerContextRejectsUnknownMode) {
  NopListener listener;
  SpeechRecognizer r(TestCredential(), "bigmodel", &listener);
  r.SetSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster);
  r.SetEnableSpeakerContext(3);
  try {
    r.Start();
    FAIL() << "expected Start to throw";
  } catch (const trtc_asr::ASRError& e) {
    EXPECT_NE(std::string(e.what()).find("EnableSpeakerContext"),
              std::string::npos)
        << e.what();
  }
}

/// 走真实 Start 路径：握手 query 带上两个参数，首响应 speaker_continue 被捕获。
TEST(V2SpeakerContext, StartQueryAndFirstResponseCapture) {
  auto closed = std::make_shared<std::atomic<bool>>(false);

  trtc_asr_test::MockWsServer server([=](trtc_asr_test::MockWsSession& ws) {
    ws.SendText(
        "{\"code\":0,\"message\":\"success\",\"voice_id\":\"v1\","
        "\"speaker_continue\":{\"continue_status\":\"resumed\","
        "\"speaker_context_id\":\"abc\"}}");
    ws.SendText(
        "{\"code\":0,\"message\":\"success\",\"voice_id\":\"v1\",\"final\":1,"
        "\"result\":{\"slice_type\":2,\"index\":0}}");
    int opcode;
    std::string payload;
    while (ws.Read(&opcode, &payload)) {
    }
    closed->store(true);
  });

  NopListener listener;
  SpeechRecognizer r(TestCredential(), "bigmodel", &listener);
  r.SetEndpoint(server.Url());
  r.SetSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster);
  r.SetEnableSpeakerContext(trtc_asr::kSpeakerContextSync);
  r.SetSpeakerContextId("  abc\n");
  r.Start();

  // 握手 query：升级 URI 应携带两个参数（签名校验由 signature 测试覆盖）。
  const std::string target = server.RequestTarget();
  EXPECT_NE(target.find("enable_speaker_context=1"), std::string::npos)
      << target;
  const auto id_pos = target.find("speaker_context_id=");
  ASSERT_NE(id_pos, std::string::npos) << target;
  auto id_value = target.substr(id_pos + std::string("speaker_context_id=").size());
  if (const auto amp = id_value.find('&'); amp != std::string::npos) {
    id_value = id_value.substr(0, amp);
  }
  EXPECT_EQ(id_value, "abc") << target;

  // 首响应的 speaker_continue 经 reader 捕获，getter 可读取。
  bool got = false;
  for (int i = 0; i < 100 && !got; ++i) {
    got = r.GetSpeakerContinue().has_value();
    if (!got) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(r.GetSpeakerContinue()->continue_status, "resumed");
  EXPECT_EQ(r.GetSpeakerContinue()->speaker_context_id, "abc");

  // 会话可能已被 final 帧结束，stop() 的 NotStarted 属预期。
  try {
    r.Stop();
  } catch (const trtc_asr::ASRError&) {
  }
  EXPECT_TRUE(closed->load());
}

}  // namespace
