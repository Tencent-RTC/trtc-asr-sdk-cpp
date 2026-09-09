// v3 protocol tests: start-frame wire format, sync ack handling, offline
// interfaces. Mirrors the Go asr/v3 tests.

#include "trtc_asr/v3.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "mock_servers.h"
#include "nlohmann/json.hpp"

namespace {

using trtc_asr::ASRError;
using trtc_asr::SpeechRecognitionResponse;
using trtc_asr::v3::AudioURLItem;
using trtc_asr::v3::Context;
using trtc_asr::v3::CreateTranscriptionRequest;
using trtc_asr::v3::FileRecognizer;
using trtc_asr::v3::NewCredential;
using trtc_asr::v3::SentenceRecognizer;
using trtc_asr::v3::SpeakerRole;
using trtc_asr::v3::SpeechRecognizer;
using trtc_asr::v3::TranscribeRequest;

trtc_asr::Credential TestCredential() {
  return NewCredential(1400000000, "test-secret");
}

std::string Ack(const std::string& voice_id) {
  return "{\"code\":0,\"message\":\"success\",\"voice_id\":\"" + voice_id + "\"}";
}

std::string ResultFrame(int slice_type, int final_flag, const std::string& text) {
  return "{\"code\":0,\"message\":\"success\",\"voice_id\":\"v1\",\"final\":" +
         std::to_string(final_flag) + ",\"result\":{\"slice_type\":" +
         std::to_string(slice_type) + ",\"index\":0,\"start_time\":0,\"end_time\":1000," +
         "\"voice_text_str\":\"" + text + "\"}}";
}

/// Mirrors the real worker: ack the start frame, wait for the client's end
/// signal, then send the terminal frame (a final sent earlier would complete
/// the session before Stop() runs).
void ServeAckThenFinalAfterEnd(trtc_asr_test::MockWsSession& ws) {
  int opcode;
  std::string payload;
  ws.Read(&opcode, &payload);  // start frame
  ws.SendText(Ack("v1"));
  while (ws.Read(&opcode, &payload)) {
    if (opcode == 0x1 && payload.find("\"end\"") != std::string::npos) break;
  }
  ws.SendText(ResultFrame(2, 1, "done"));
  while (ws.Read(&opcode, &payload)) {
  }
}

class RecordingListener : public trtc_asr::SpeechRecognitionListener {
 public:
  std::atomic<int> begins{0};
  std::atomic<int> changes{0};
  std::atomic<int> ends{0};
  std::atomic<int> completes{0};
  std::atomic<int> fails{0};
  std::atomic<int> last_fail_code{0};

  void OnSentenceBegin(const SpeechRecognitionResponse&) override { begins++; }
  void OnRecognitionResultChange(const SpeechRecognitionResponse&) override { changes++; }
  void OnSentenceEnd(const SpeechRecognitionResponse&) override { ends++; }
  void OnRecognitionComplete(const SpeechRecognitionResponse&) override { completes++; }
  void OnFail(const SpeechRecognitionResponse*, const ASRError& err) override {
    fails++;
    last_fail_code = err.code();
  }
};

bool WaitUntil(const std::function<bool()>& cond, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

}  // namespace

TEST(V3SpeechRecognizer, StartFrameWireFormat) {
  auto start_frames = std::make_shared<std::atomic<int>>(0);
  auto captured = std::make_shared<std::mutex>();
  auto captured_frames = std::make_shared<std::vector<std::string>>();

  trtc_asr_test::MockWsServer server([=](trtc_asr_test::MockWsSession& ws) {
    int opcode;
    std::string payload;
    if (ws.Read(&opcode, &payload) && opcode == 0x1) {
      std::lock_guard<std::mutex> lock(*captured);
      captured_frames->push_back(payload);
      start_frames->fetch_add(1);
    }
    ws.SendText(Ack("voice-1"));
    ServeAckThenFinalAfterEnd(ws);
  });

  RecordingListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint(server.Url());
  r.SetVoiceId("voice-1");
  r.SetHotwordList("深度学习|10");
  r.SetVadLevel(0);
  r.SetNoiseThreshold(1.5);
  r.SetFilterEmptyResult(0);
  r.SetConvertNumMode(0);  // explicit 0 must be sent on v3
  r.SetSpeakerDiarization(trtc_asr::v3::kSpeakerDiarizationVoiceprint);
  r.SetSpeakerRoles({SpeakerRole{"teacher", "https://example.com/t.wav"}});
  r.SetVoiceprintIds({"vp-1"});
  Context ctx;
  ctx.text = "bg";
  ctx.terms = {"ASR"};
  ctx.general = {trtc_asr::v3::ContextKV{"domain", "Meeting"}};
  r.SetContext(ctx);

  EXPECT_NO_THROW(r.Start());
  EXPECT_NO_THROW(r.Write(std::vector<uint8_t>(1280, 0)));
  r.Stop();
  const bool completed = WaitUntil([&] { return listener.completes.load() > 0; }, 3000);
  EXPECT_TRUE(completed);

  // Handshake URL: path /asr/v3, query carries only voice_id.
  EXPECT_EQ(server.RequestTarget(), "/asr/v3?voice_id=voice-1");

  ASSERT_EQ(captured_frames->size(), 1u);
  const nlohmann::json frame = nlohmann::json::parse((*captured_frames)[0]);
  EXPECT_EQ(frame["type"], "start");
  EXPECT_EQ(frame["auth"]["sdkappid"], "1400000000");
  EXPECT_FALSE(frame["auth"]["usersig"].get<std::string>().empty());
  // business is a server-side internal gray dimension; never sent.
  EXPECT_FALSE(frame["auth"].contains("business"));

  const auto& params = frame["params"];
  EXPECT_EQ(params["voice_id"], "voice-1");
  EXPECT_EQ(params["engine_model_type"], "16k_zh_en");
  EXPECT_EQ(params["voice_format"], 1);
  EXPECT_EQ(params["needvad"], 1);
  EXPECT_EQ(params["convert_num_mode"], 0);      // explicit 0 preserved
  EXPECT_EQ(params["filter_empty_result"], 0);   // explicit 0 preserved
  EXPECT_EQ(params["vad_level"], 0);             // explicit 0 preserved
  EXPECT_EQ(params["noise_threshold"], 1.5);
  EXPECT_EQ(params["hotword_list"], "深度学习|10");
  EXPECT_EQ(params["speaker_diarization"], 3);
  EXPECT_EQ(params["voiceprint_ids"], nlohmann::json::array({"vp-1"}));
  // speaker_roles elements are snake_case, not the v2 CamelCase wire.
  EXPECT_EQ(params["speaker_roles"],
            nlohmann::json::array(
                {nlohmann::json{{"role_name", "teacher"},
                                {"audio_url", "https://example.com/t.wav"}}}));
  const nlohmann::json want_context = {
      {"text", "bg"},
      {"terms", nlohmann::json::array({"ASR"})},
      {"general",
       nlohmann::json::array({nlohmann::json{{"key", "domain"}, {"value", "Meeting"}}})}};
  EXPECT_EQ(params["context"], want_context);
  EXPECT_EQ(params["sdk_info"]["sdk_lang"], "cpp");
}

TEST(V3SpeechRecognizer, StartAuthErrorIsSynchronous) {
  trtc_asr_test::MockWsServer server([](trtc_asr_test::MockWsSession& ws) {
    int opcode;
    std::string payload;
    ws.Read(&opcode, &payload);
    ws.SendText("{\"code\":4002,\"message\":\"auth failed\",\"voice_id\":\"v1\"}");
    while (ws.Read(&opcode, &payload)) {
    }
  });

  RecordingListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint(server.Url());
  try {
    r.Start();
    FAIL() << "start must fail synchronously";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), 4002);
    EXPECT_NE(std::string(e.message()).find("auth failed"), std::string::npos);
  }
  // The failed start leaves no usable session.
  try {
    r.Write(std::vector<uint8_t>(16, 0));
    FAIL() << "write after failed start must fail";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), trtc_asr::kErrNotStarted);
  }
}

TEST(V3SpeechRecognizer, StartGrayDisabledErrorIsSynchronous) {
  trtc_asr_test::MockWsServer server([](trtc_asr_test::MockWsSession& ws) {
    int opcode;
    std::string payload;
    ws.Read(&opcode, &payload);
    ws.SendText("{\"code\":4001,\"message\":\"v3 interface not enabled\",\"voice_id\":\"v1\"}");
    while (ws.Read(&opcode, &payload)) {
    }
  });

  RecordingListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint(server.Url());
  try {
    r.Start();
    FAIL() << "start must fail synchronously";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), 4001);
  }
}

TEST(V3SpeechRecognizer, NormalFlowDispatchesEvents) {
  trtc_asr_test::MockWsServer server([](trtc_asr_test::MockWsSession& ws) {
    int opcode;
    std::string payload;
    ws.Read(&opcode, &payload);
    ws.SendText(Ack("v1"));
    ws.SendText(ResultFrame(0, 0, ""));
    ws.SendText(ResultFrame(1, 0, "你好"));
    ws.SendText(ResultFrame(2, 0, "你好。"));
    while (ws.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload.find("\"end\"") != std::string::npos) break;
    }
    ws.SendText(ResultFrame(2, 1, "你好。"));
    while (ws.Read(&opcode, &payload)) {
    }
  });

  RecordingListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint(server.Url());
  r.SetWriteTimeout(std::chrono::milliseconds(500));
  r.SetStopTimeout(std::chrono::seconds(2));
  EXPECT_NO_THROW(r.Start());
  EXPECT_NO_THROW(r.Write(std::vector<uint8_t>(1280, 0)));
  const bool progressed = WaitUntil([&] {
    return listener.begins.load() >= 1 && listener.changes.load() >= 1 &&
           listener.ends.load() >= 1;
  }, 3000);
  EXPECT_TRUE(progressed);
  EXPECT_NO_THROW(r.Stop());
  // The standalone sentence-end (slice_type=2, final=0) and the terminal
  // final=1&slice_type=2 frame each dispatch OnSentenceEnd — mirroring Go.
  EXPECT_GE(listener.begins.load(), 1);
  EXPECT_GE(listener.changes.load(), 1);
  EXPECT_GE(listener.ends.load(), 2);
  EXPECT_GE(listener.completes.load(), 1);
  EXPECT_EQ(listener.fails.load(), 0);
}

TEST(V3SpeechRecognizer, OversizedAudioFrameFailsLocally) {
  trtc_asr_test::MockWsServer server([](trtc_asr_test::MockWsSession& ws) {
    ServeAckThenFinalAfterEnd(ws);
  });
  RecordingListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint(server.Url());
  EXPECT_NO_THROW(r.Start());
  try {
    r.Write(std::vector<uint8_t>(trtc_asr::v3::kStreamFrameMaxBytes + 1, 0));
    FAIL() << "oversized frame must fail locally";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), trtc_asr::kErrInvalidParam);
  }
  EXPECT_NO_THROW(r.Stop());
}

TEST(V3SpeechRecognizer, LocalValidationRejectsOutOfRangeOptions) {
  const std::string long_voice_id(129, 'x');
  const std::vector<std::pair<std::string, std::function<void(SpeechRecognizer&)>>>
      cases = {
          {"voice_id too long",
           [&](SpeechRecognizer& r) { r.SetVoiceId(long_voice_id); }},
          {"max_speak_time too small",
           [](SpeechRecognizer& r) { r.SetMaxSpeakTime(1000); }},
          {"max_speak_time too large",
           [](SpeechRecognizer& r) { r.SetMaxSpeakTime(90001); }},
          {"vad_silence_time too small",
           [](SpeechRecognizer& r) { r.SetVadSilenceTime(100); }},
          {"vad_silence_time too large",
           [](SpeechRecognizer& r) { r.SetVadSilenceTime(2001); }},
          {"needvad invalid", [](SpeechRecognizer& r) { r.SetNeedVad(2); }},
          {"convert_num_mode invalid", [](SpeechRecognizer& r) { r.SetConvertNumMode(2); }},
          {"filter_dirty invalid", [](SpeechRecognizer& r) { r.SetFilterDirty(3); }},
          {"filter_modal invalid", [](SpeechRecognizer& r) { r.SetFilterModal(3); }},
          {"filter_punc invalid", [](SpeechRecognizer& r) { r.SetFilterPunc(2); }},
          {"word_info invalid", [](SpeechRecognizer& r) { r.SetWordInfo(3); }},
          {"word_with_space invalid", [](SpeechRecognizer& r) { r.SetWordWithSpace(2); }},
          {"voice_format invalid", [](SpeechRecognizer& r) { r.SetVoiceFormat(2); }},
          {"input_sample_rate invalid", [](SpeechRecognizer& r) { r.SetInputSampleRate(16000); }},
          {"filter_empty_result invalid",
           [](SpeechRecognizer& r) { r.SetFilterEmptyResult(2); }},
          {"vad_level invalid", [](SpeechRecognizer& r) { r.SetVadLevel(2); }},
          {"noise_threshold out of range", [](SpeechRecognizer& r) { r.SetNoiseThreshold(4.1); }},
          {"diarization invalid", [](SpeechRecognizer& r) { r.SetSpeakerDiarization(2); }},
      };
  for (const auto& tc : cases) {
    RecordingListener listener;
    SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
    r.SetEndpoint("ws://127.0.0.1:1");  // unreachable; validation fails first
    tc.second(r);
    try {
      r.Start();
      FAIL() << tc.first << " must fail locally";
    } catch (const ASRError& e) {
      EXPECT_EQ(e.code(), trtc_asr::kErrInvalidParam) << tc.first;
    }
  }

  // Boundary values must pass validation (they then fail at dial, which must
  // not be INVALID_PARAM).
  RecordingListener listener;
  SpeechRecognizer r(TestCredential(), "16k_zh_en", &listener);
  r.SetEndpoint("ws://127.0.0.1:1");
  r.SetMaxSpeakTime(90000);
  r.SetVadSilenceTime(240);
  r.SetVoiceFormat(12);
  r.SetWordInfo(100);
  try {
    r.Start();
  } catch (const ASRError& e) {
    EXPECT_NE(e.code(), trtc_asr::kErrInvalidParam);
  }
}

TEST(V3Transcribe, WireFormatAndErrorMapping) {
  trtc_asr_test::MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.target, "/v3/transcribe");
    EXPECT_EQ(req.Header("Content-Type"), "application/json; charset=utf-8");
    EXPECT_TRUE(req.Header("X-TRTC-UserSig").empty());
    const nlohmann::json body = nlohmann::json::parse(req.body);
    EXPECT_EQ(body["auth"]["sdkappid"], "1400000000");
    EXPECT_FALSE(body["auth"]["usersig"].get<std::string>().empty());
    EXPECT_FALSE(body["auth"]["request_id"].get<std::string>().empty());
    EXPECT_FALSE(body["auth"].contains("business"));
    const auto& params = body["params"];
    EXPECT_EQ(params["engine_model_type"], "16k_zh_en");
    EXPECT_EQ(params["needvad"], 0);  // explicit 0 honored
    EXPECT_EQ(params["language"], "zh");
    EXPECT_EQ(params["sdk_info"]["sdk_lang"], "cpp");
    for (const char* bad : {"EngSerViceType", "SourceType", "VoiceFormat"}) {
      EXPECT_FALSE(params.contains(bad));
    }
    return trtc_asr_test::MockHttpResponse{
        200, "{\"code\":0,\"message\":\"success\",\"request_id\":\"req-1\","
             "\"result\":\"你好世界\",\"audio_duration\":1500,\"language\":\"zh\"}"};
  });

  SentenceRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());
  TranscribeRequest req;
  req.engine_model_type = "16k_zh_en";
  req.source_type = trtc_asr::v3::kSourceTypeData;
  req.voice_format = "pcm";
  req.data = "cG0tZGF0YQ==";
  req.data_len = 8;
  req.needvad = 0;  // explicit 0 honored
  req.language = "zh";
  auto resp = r.Recognize(req);
  EXPECT_EQ(resp.code, 0);
  EXPECT_EQ(resp.result, "你好世界");
  EXPECT_EQ(resp.request_id, "req-1");

  // Auth failure: HTTP 200 + body code 4002 — judge by the body.
  trtc_asr_test::MockHttpServer err_server(
      [](const trtc_asr_test::CapturedHttpRequest&) {
        return trtc_asr_test::MockHttpResponse{
            200, "{\"code\":4002,\"message\":\"auth failed\",\"request_id\":\"req-x\"}"};
      });
  SentenceRecognizer r2(TestCredential());
  r2.SetEndpoint(err_server.Url());
  try {
    r2.RecognizeUrl("https://example.com/a.wav", "wav", "16k_zh_en");
    FAIL() << "must fail with the body code";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), 4002);
    EXPECT_NE(std::string(e.message()).find("req-x"), std::string::npos);
  }

  // Non-v3 JSON body on non-2xx must not be treated as success.
  trtc_asr_test::MockHttpServer bad_server(
      [](const trtc_asr_test::CapturedHttpRequest&) {
        return trtc_asr_test::MockHttpResponse{502, "{\"error\":\"upstream unavailable\"}"};
      });
  SentenceRecognizer r3(TestCredential());
  r3.SetEndpoint(bad_server.Url());
  try {
    r3.RecognizeUrl("https://example.com/a.wav", "wav", "16k_zh_en");
    FAIL() << "non-v3 body must fail";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), trtc_asr::kErrServerError);
    EXPECT_NE(std::string(e.message()).find("502"), std::string::npos);
  }

  // Local validation.
  EXPECT_THROW(r.RecognizeData(std::vector<uint8_t>{}, "pcm", "16k_zh_en"), ASRError);
  TranscribeRequest bad_needvad;
  bad_needvad.engine_model_type = "16k_zh_en";
  bad_needvad.source_type = trtc_asr::v3::kSourceTypeUrl;
  bad_needvad.voice_format = "wav";
  bad_needvad.url = "https://example.com/a.wav";
  bad_needvad.needvad = 2;
  EXPECT_THROW(r.Recognize(bad_needvad), ASRError);
}

TEST(V3FileRecognizer, CreateTaskWireAudioURLsAndDescribe) {
  trtc_asr_test::MockHttpServer server(
      [](const trtc_asr_test::CapturedHttpRequest& req) {
        EXPECT_EQ(req.target, "/v3/create_transcription");
        const nlohmann::json body = nlohmann::json::parse(req.body);
        const auto& params = body["params"];
        if (params.contains("audio_urls")) {
          EXPECT_EQ(params["audio_urls"].size(), 2u);
          EXPECT_EQ(params["audio_urls"][0]["index"], 0);
          EXPECT_FALSE(params.contains("url"));
          return trtc_asr_test::MockHttpResponse{
              200, "{\"code\":0,\"message\":\"success\",\"transcription_id\":\"tid-d\"}"};
        }
        // nlohmann::json objects dump with alphabetically ordered keys, so
        // compare as parsed JSON instead of a raw string.
        EXPECT_EQ(params["speaker_roles"],
                  nlohmann::json::array({nlohmann::json{
                      {"role_name", "teacher"},
                      {"audio_url", "https://example.com/t.wav"}}}));
        return trtc_asr_test::MockHttpResponse{
            200, "{\"code\":0,\"message\":\"success\",\"transcription_id\":\"tid-abc\"}"};
      });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  CreateTranscriptionRequest req;
  req.engine_model_type = "16k_zh_en";
  req.channel_num = 1;
  req.res_text_format = 1;
  req.source_type = trtc_asr::v3::kSourceTypeUrl;
  req.url = "https://example.com/a.wav";
  req.speaker_diarization = trtc_asr::v3::kSpeakerDiarizationVoiceprint;
  req.speaker_roles = {SpeakerRole{"teacher", "https://example.com/t.wav"}};
  req.voiceprint_ids = {"vp-1"};
  EXPECT_EQ(r.CreateTask(req), "tid-abc");

  // Distributed path: audio_urls requires source_type=0 with url/data empty.
  CreateTranscriptionRequest dist;
  dist.engine_model_type = "16k_zh_en";
  dist.channel_num = 1;
  dist.res_text_format = 1;
  dist.source_type = trtc_asr::v3::kSourceTypeUrl;  // zero value; url/data empty
  dist.speaker_diarization = trtc_asr::v3::kSpeakerDiarizationCluster;
  dist.audio_urls = {AudioURLItem{0, "https://example.com/a.wav", "a"},
                     AudioURLItem{1, "https://example.com/b.wav", ""}};
  EXPECT_EQ(r.CreateTask(dist), "tid-d");

  // Bad combinations must fail local validation.
  CreateTranscriptionRequest with_url = dist;
  with_url.url = "https://example.com/x.wav";
  EXPECT_THROW(r.CreateTask(with_url), ASRError);
  CreateTranscriptionRequest with_data = dist;
  with_data.data = "AAAA";
  with_data.data_len = 3;
  EXPECT_THROW(r.CreateTask(with_data), ASRError);
  CreateTranscriptionRequest bad_type = dist;
  bad_type.source_type = trtc_asr::v3::kSourceTypeData;
  EXPECT_THROW(r.CreateTask(bad_type), ASRError);
  CreateTranscriptionRequest neg_index = dist;
  neg_index.audio_urls[0].index = -1;
  EXPECT_THROW(r.CreateTask(neg_index), ASRError);

  // Describe + poll mapping.
  int calls = 0;
  trtc_asr_test::MockHttpServer describe_server(
      [&calls](const trtc_asr_test::CapturedHttpRequest& req) {
        EXPECT_EQ(req.target, "/v3/describe_transcription");
        if (calls++ == 0) {
          return trtc_asr_test::MockHttpResponse{
              200, "{\"code\":0,\"transcription_id\":\"t1\",\"status\":1,\"progress\":40}"};
        }
        return trtc_asr_test::MockHttpResponse{
            200, "{\"code\":0,\"transcription_id\":\"t1\",\"status\":2,\"progress\":100,"
                 "\"audio_duration\":6.312,\"result\":\"全文\",\"result_detail\":[{"
                 "\"final_sentence\":\"第一句话。\",\"start_ms\":0,\"end_ms\":1200,"
                 "\"words\":[{\"word\":\"第一句\",\"start_time\":0,\"end_time\":900}],"
                 "\"speaker_id\":1,\"speaker_role_name\":\"teacher\","
                 "\"language_b47\":\"zh-CN\"}]}"};
      });
  FileRecognizer r2(TestCredential());
  r2.SetEndpoint(describe_server.Url());
  auto status = r2.WaitForResultWithInterval("t1", std::chrono::milliseconds(10),
                                                std::chrono::seconds(5));
  EXPECT_EQ(status.status, trtc_asr::v3::kTaskStatusSuccess);
  EXPECT_EQ(status.result, "全文");
  EXPECT_NEAR(status.audio_duration, 6.312, 1e-9);
  ASSERT_FALSE(status.result_detail.empty());
  EXPECT_EQ(status.result_detail[0].final_sentence, "第一句话。");
  EXPECT_EQ(status.result_detail[0].speaker_role_name, "teacher");
  EXPECT_EQ(status.result_detail[0].language_b47, "zh-CN");
}
