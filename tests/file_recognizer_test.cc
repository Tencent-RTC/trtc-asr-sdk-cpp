#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "json_helper.h"
#include "mock_servers.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/file_recognizer.h"
#include "trtc_asr/usersig.h"

namespace {

using nlohmann::json;
using trtc_asr::ASRError;
using trtc_asr::CreateRecTaskRequest;
using trtc_asr::FileRecognizer;
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

CreateRecTaskRequest MinimalRequest() {
  CreateRecTaskRequest req;
  req.engine_model_type = "16k_zh_en";
  req.channel_num = 1;
  req.res_text_format = 1;
  return req;
}

TEST(FileRecognizer, ValidateCreateRequest) {
  FileRecognizer r(TestCredential());

  ExpectError([&] { r.CreateTask(CreateRecTaskRequest{}); },
              trtc_asr::kErrInvalidParam, "EngineModelType is required");

  {
    auto req = MinimalRequest();
    req.channel_num = 0;
    ExpectError([&] { r.CreateTask(req); }, trtc_asr::kErrInvalidParam,
                "ChannelNum must be positive");
  }
  {
    auto req = MinimalRequest();
    req.source_type = 0;  // URL
    ExpectError([&] { r.CreateTask(req); }, trtc_asr::kErrInvalidParam,
                "Url is required");
  }
  {
    auto req = MinimalRequest();
    req.source_type = 1;  // Data
    ExpectError([&] { r.CreateTask(req); }, trtc_asr::kErrInvalidParam,
                "Data is required");
  }
}

TEST(FileRecognizer, CreateTaskFromDataRejectsEmptyAndOversized) {
  FileRecognizer r(TestCredential());
  ExpectError([&] { r.CreateTaskFromData({}, "pcm", "16k_zh"); },
              trtc_asr::kErrInvalidParam, "audio data is empty");
  std::vector<uint8_t> big(6 * 1024 * 1024, 0);  // 6MB > 5MB limit
  ExpectError([&] { r.CreateTaskFromData(big, "pcm", "16k_zh"); },
              trtc_asr::kErrInvalidParam, "5MB");
}

TEST(FileRecognizer, CreateTaskFromUrlRejectsEmpty) {
  FileRecognizer r(TestCredential());
  ExpectError([&] { r.CreateTaskFromURL("", "16k_zh"); },
              trtc_asr::kErrInvalidParam, "audio URL is empty");
}

TEST(FileRecognizer, CreateTaskSuccess) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    EXPECT_EQ(req.method, "POST");
    EXPECT_TRUE(req.target.rfind("/v1/CreateRecTask?", 0) == 0);
    EXPECT_EQ(req.Header("Content-Type"), "application/json; charset=utf-8");
    EXPECT_FALSE(req.Header("X-TRTC-SdkAppId").empty());
    EXPECT_FALSE(req.Header("X-TRTC-UserSig").empty());
    for (const char* key : {"AppId", "Secretid", "RequestId", "Timestamp"}) {
      EXPECT_TRUE(req.Query(key).has_value()) << "missing " << key;
    }
    json body = json::parse(req.body);
    EXPECT_EQ(body["EngineModelType"], "16k_zh_en");
    EXPECT_EQ(body["SourceType"], 1);

    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"test-task-id-12345"},"RequestId":"test-request-id"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  std::string task_id = r.CreateTaskFromData(Bytes("fake-pcm-audio"), "pcm", "16k_zh_en");
  EXPECT_EQ(task_id, "test-task-id-12345");
}

TEST(FileRecognizer, CreateTaskServerError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Error":{"Code":"4002","Message":"鉴权失败"},"RequestId":"test-request-id"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError([&] { r.CreateTaskFromData(Bytes("fake-audio"), "pcm", "16k_zh_en"); },
              trtc_asr::kErrServerError, "4002");
}

TEST(FileRecognizer, CreateTaskHttpError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{500, "internal server error"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError([&] { r.CreateTaskFromData(Bytes("fake-audio"), "pcm", "16k_zh_en"); },
              trtc_asr::kErrServerError, "500");
}

TEST(FileRecognizer, CreateTaskEmptyTaskIdIsError) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{200,
                            R"({"Response":{"Data":{"RecTaskId":""},"RequestId":"r"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError([&] { r.CreateTaskFromData(Bytes("fake-audio"), "pcm", "16k_zh_en"); },
              trtc_asr::kErrServerError, "empty RecTaskId");
}

TEST(FileRecognizer, CreateTaskWithDiarizationAndVadOptions) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    json body = json::parse(req.body);
    EXPECT_EQ(body["SpeakerDiarization"], 3);
    EXPECT_EQ(body["SpeakerNumber"], 2);
    EXPECT_EQ(body["SpeakerRoles"][0]["RoleName"], "teacher");
    EXPECT_EQ(body["SpeakerRoles"][0]["AudioUrl"], "https://example.com/a.wav");
    EXPECT_EQ(body["VoiceprintIds"][0], "vp-1");
    // VadLevel=0 must be serialized (optional distinguishes "unset").
    EXPECT_TRUE(body.contains("VadLevel")) << req.body;
    EXPECT_EQ(body["VadLevel"], 0);
    EXPECT_DOUBLE_EQ(body["NoiseThreshold"].get<double>(), 1.5);
    return MockHttpResponse{200,
                            R"({"Response":{"Data":{"RecTaskId":"task-diar"},"RequestId":"r"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto req = MinimalRequest();
  req.source_type = 0;
  req.url = "https://example.com/call.wav";
  req.speaker_diarization = trtc_asr::kSpeakerDiarizationVoiceprint;
  req.speaker_number = 2;
  req.speaker_roles = {{"teacher", "https://example.com/a.wav"}};
  req.voiceprint_ids = {"vp-1"};
  req.vad_level = 0;
  req.noise_threshold = 1.5;

  EXPECT_EQ(r.CreateTask(req), "task-diar");
}

TEST(FileRecognizer, CreateTaskRejectsInvalidDiarization) {
  FileRecognizer r(TestCredential());
  auto req = MinimalRequest();
  req.source_type = 0;
  req.url = "https://example.com/call.wav";
  req.speaker_diarization = 2;  // unsupported
  ExpectError([&] { r.CreateTask(req); }, trtc_asr::kErrInvalidParam,
              "SpeakerDiarization must be 0");
}

TEST(FileRecognizer, CreateTaskFromDataWithOptions) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    json body = json::parse(req.body);
    EXPECT_EQ(body["FilterDirty"], 1);
    EXPECT_EQ(body["ResTextFormat"], 2);
    EXPECT_EQ(body["HotwordId"], "hw-123");
    return MockHttpResponse{200,
                            R"({"Response":{"Data":{"RecTaskId":"task-opts"},"RequestId":"r"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto req = MinimalRequest();
  req.res_text_format = 2;
  req.filter_dirty = 1;
  req.hotword_id = "hw-123";
  EXPECT_EQ(r.CreateTaskFromDataWithOptions(Bytes("fake-audio"), &req), "task-opts");
}

TEST(FileRecognizer, CreateTaskFromDataWithOptionsNullRequest) {
  FileRecognizer r(TestCredential());
  ExpectError([&] { r.CreateTaskFromDataWithOptions(Bytes("x"), nullptr); },
              trtc_asr::kErrInvalidParam, "request is null");
}

TEST(FileRecognizer, DescribeTaskStatusMalformedFieldTypes) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"t","Status":"oops"},"RequestId":"r"}})"};
  });
  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());
  ExpectError([&] { r.DescribeTaskStatus("t"); }, trtc_asr::kErrReadFailed,
              "unmarshal response failed");
}

TEST(FileRecognizer, DescribeTaskStatusEmptyId) {
  FileRecognizer r(TestCredential());
  ExpectError([&] { r.DescribeTaskStatus(""); }, trtc_asr::kErrInvalidParam,
              "RecTaskId is empty");
}

TEST(FileRecognizer, DescribeTaskStatusSuccessWithDetails) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest& req) {
    EXPECT_TRUE(req.target.rfind("/v1/DescribeTaskStatus?", 0) == 0);
    json body = json::parse(req.body);
    EXPECT_EQ(body["RecTaskId"], "task-123");
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-123","Status":2,"StatusStr":"success","Progress":100,"Result":"今天天气不错。","AudioDuration":2.38,"ResultDetail":[{"FinalSentence":"今天天气不错。","SliceSentence":"今天 天气 不错","StartMs":200,"EndMs":1380,"WordsNum":1,"SpeechSpeed":2.0,"SpeakerId":1,"SpeakerRoleName":"teacher","ChannelId":0,"Language":"zh","Words":[{"Word":"今天","OffsetStartMs":200,"OffsetEndMs":500}]}]},"RequestId":"req-123"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto status = r.DescribeTaskStatus("task-123");
  EXPECT_EQ(status.status, trtc_asr::kTaskStatusSuccess);
  EXPECT_EQ(status.result, "今天天气不错。");
  EXPECT_DOUBLE_EQ(status.audio_duration, 2.38);
  ASSERT_EQ(status.result_detail.size(), 1);
  const auto& detail = status.result_detail[0];
  EXPECT_EQ(detail.final_sentence, "今天天气不错。");
  EXPECT_EQ(detail.speaker_id, 1);
  EXPECT_EQ(detail.speaker_role_name, "teacher");
  EXPECT_EQ(detail.language, "zh");
  ASSERT_EQ(detail.words.size(), 1);
  EXPECT_EQ(detail.words[0].word, "今天");
}

TEST(FileRecognizer, DescribeTaskStatusTaskFailedFields) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-fail","Status":3,"StatusStr":"failed","ErrorMsg":"Failed to download audio file"},"RequestId":"req-456"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto status = r.DescribeTaskStatus("task-fail");
  EXPECT_EQ(status.status, trtc_asr::kTaskStatusFailed);
  EXPECT_EQ(status.error_msg, "Failed to download audio file");
}

TEST(FileRecognizer, WaitForResultImmediateSuccess) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-ok","Status":2,"StatusStr":"success","Result":"识别结果","AudioDuration":1.5},"RequestId":"req-ok"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto status = r.WaitForResult("task-ok");
  EXPECT_EQ(status.result, "识别结果");
}

TEST(FileRecognizer, WaitForResultPollingThenSuccess) {
  auto calls = std::make_shared<int>(0);
  MockHttpServer server([calls](const trtc_asr_test::CapturedHttpRequest&) {
    int n = ++*calls;
    if (n < 3) {
      return MockHttpResponse{
          200,
          R"({"Response":{"Data":{"RecTaskId":"task-poll","Status":1,"StatusStr":"doing"},"RequestId":"req-poll"}})"};
    }
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-poll","Status":2,"StatusStr":"success","Result":"轮询成功","AudioDuration":3.0},"RequestId":"req-poll"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  auto status = r.WaitForResultWithInterval("task-poll", std::chrono::milliseconds(50),
                                            std::chrono::seconds(5));
  EXPECT_EQ(status.result, "轮询成功");
  EXPECT_GE(*calls, 3);
}

TEST(FileRecognizer, WaitForResultTaskFailed) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-err","Status":3,"StatusStr":"failed","ErrorMsg":"转码失败"},"RequestId":"req-err"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError([&] { r.WaitForResult("task-err"); }, trtc_asr::kErrServerError,
              "转码失败");
}

TEST(FileRecognizer, WaitForResultTimeout) {
  MockHttpServer server([](const trtc_asr_test::CapturedHttpRequest&) {
    return MockHttpResponse{
        200,
        R"({"Response":{"Data":{"RecTaskId":"task-slow","Status":0,"StatusStr":"waiting"},"RequestId":"req-slow"}})"};
  });

  FileRecognizer r(TestCredential());
  r.SetEndpoint(server.Url());

  ExpectError(
      [&] {
        r.WaitForResultWithInterval("task-slow", std::chrono::milliseconds(20),
                                    std::chrono::milliseconds(100));
      },
      trtc_asr::kErrTimeout, "not completed");
}

}  // namespace
