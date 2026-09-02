#include "trtc_asr/file_recognizer.h"

#include <thread>

#include "http_client.h"
#include "json_helper.h"
#include "sdkinfo.h"
#include "trtc_asr/params.h"
#include "trtc_asr/usersig.h"
#include "ws_client.h"  // GenerateUuid

namespace trtc_asr {
namespace {

using nlohmann::json;

/// Serializes the request with Go's omitempty semantics: zero ints and empty
/// strings/containers are omitted; ChannelNum/ResTextFormat/SourceType are
/// always sent; VadLevel/NoiseThreshold (optional) are sent when set.
std::string SerializeRequest(const CreateRecTaskRequest& req) {
  json j;
  j["EngineModelType"] = req.engine_model_type;
  j["ChannelNum"] = req.channel_num;
  j["ResTextFormat"] = req.res_text_format;
  j["SourceType"] = req.source_type;
  if (!req.url.empty()) j["Url"] = req.url;
  if (!req.data.empty()) j["Data"] = req.data;
  if (req.data_len != 0) j["DataLen"] = req.data_len;
  if (!req.callback_url.empty()) j["CallbackUrl"] = req.callback_url;
  if (req.filter_dirty != 0) j["FilterDirty"] = req.filter_dirty;
  if (req.filter_modal != 0) j["FilterModal"] = req.filter_modal;
  if (req.filter_punc != 0) j["FilterPunc"] = req.filter_punc;
  if (req.convert_num_mode != 0) j["ConvertNumMode"] = req.convert_num_mode;
  if (!req.hotword_id.empty()) j["HotwordId"] = req.hotword_id;
  if (!req.hotword_list.empty()) j["HotwordList"] = req.hotword_list;
  if (!req.customization_id.empty()) j["CustomizationId"] = req.customization_id;
  if (!req.replace_text_id.empty()) j["ReplaceTextId"] = req.replace_text_id;
  if (!req.language.empty()) j["Language"] = req.language;
  if (req.speaker_diarization != 0) j["SpeakerDiarization"] = req.speaker_diarization;
  if (req.speaker_number != 0) j["SpeakerNumber"] = req.speaker_number;
  if (!req.speaker_roles.empty()) {
    j["SpeakerRoles"] = json::parse(internal::SerializeSpeakerRoles(req.speaker_roles));
  }
  if (!req.voiceprint_ids.empty()) j["VoiceprintIds"] = req.voiceprint_ids;
  if (req.vad_silence_ms != 0) j["VadSilenceMs"] = req.vad_silence_ms;
  if (req.vad_level.has_value()) j["VadLevel"] = *req.vad_level;
  if (req.noise_threshold.has_value()) j["NoiseThreshold"] = *req.noise_threshold;
  return j.dump();
}

void ValidateCreateRequest(const CreateRecTaskRequest& req) {
  if (req.engine_model_type.empty()) {
    throw ASRError(kErrInvalidParam, "EngineModelType is required");
  }
  if (req.channel_num <= 0) {
    throw ASRError(kErrInvalidParam, "ChannelNum must be positive");
  }
  if (req.source_type == 0 && req.url.empty()) {
    throw ASRError(kErrInvalidParam, "Url is required when SourceType=0");
  }
  if (req.source_type == 1 && req.data.empty()) {
    throw ASRError(kErrInvalidParam, "Data is required when SourceType=1");
  }
  ValidateSpeakerDiarization(req.speaker_diarization, req.speaker_number,
                             req.speaker_roles, req.voiceprint_ids);
  ValidateVadTuning(req.vad_level, req.noise_threshold);
}

SentenceDetail ParseSentenceDetail(const json& j) {
  SentenceDetail out;
  out.final_sentence = j.value("FinalSentence", "");
  out.slice_sentence = j.value("SliceSentence", "");
  out.written_text = j.value("WrittenText", "");
  out.start_ms = j.value("StartMs", (int64_t)0);
  out.end_ms = j.value("EndMs", (int64_t)0);
  out.words_num = j.value("WordsNum", 0);
  out.speech_speed = j.value("SpeechSpeed", 0.0);
  out.silence_time = j.value("SilenceTime", (int64_t)0);
  out.speaker_id = j.value("SpeakerId", 0);
  out.speaker_role_name = j.value("SpeakerRoleName", "");
  out.channel_id = j.value("ChannelId", 0);
  out.language = j.value("Language", "");
  if (j.contains("Words") && j["Words"].is_array()) {
    for (const auto& w : j["Words"]) {
      SentenceWords word;
      word.word = w.value("Word", "");
      word.offset_start_ms = w.value("OffsetStartMs", (int64_t)0);
      word.offset_end_ms = w.value("OffsetEndMs", (int64_t)0);
      out.words.push_back(std::move(word));
    }
  }
  return out;
}

TaskStatus ParseTaskStatus(const json& j) {
  TaskStatus out;
  out.rec_task_id = j.value("RecTaskId", "");
  out.status = j.value("Status", 0);
  out.status_str = j.value("StatusStr", "");
  out.progress = j.value("Progress", 0);
  out.result = j.value("Result", "");
  out.error_msg = j.value("ErrorMsg", "");
  out.audio_duration = j.value("AudioDuration", 0.0);
  if (j.contains("ResultDetail") && j["ResultDetail"].is_array()) {
    for (const auto& d : j["ResultDetail"]) {
      out.result_detail.push_back(ParseSentenceDetail(d));
    }
  }
  return out;
}

/// Parses the standard API envelope {"Response": {...}} and checks for an
/// API-level error.
json ParseApiEnvelope(const std::string& body, const char* request_path) {
  json root;
  try {
    root = json::parse(body);
  } catch (const std::exception& e) {
    throw ASRError(kErrReadFailed,
                   std::string("unmarshal response failed: ") + e.what());
  }
  if (!root.contains("Response") || !root["Response"].is_object()) {
    throw ASRError(kErrServerError, "empty response from server");
  }
  const json& response = root["Response"];
  if (response.contains("Error") && response["Error"].is_object()) {
    const json& api_err = response["Error"];
    throw ASRError(kErrServerError,
                   "server error [" + internal::StringFieldOrEmpty(api_err, "Code") +
                       "]: " + internal::StringFieldOrEmpty(api_err, "Message") +
                       " (RequestId: " +
                       internal::StringFieldOrEmpty(response, "RequestId") + ")");
  }
  (void)request_path;
  return response;
}

}  // namespace

FileRecognizer::FileRecognizer(const Credential& credential)
    : credential_(credential), endpoint_(kEndpoint) {}

std::string FileRecognizer::CreateTask(const CreateRecTaskRequest& req) {
  ValidateCreateRequest(req);
  std::string resp_body = DoRequest("/v1/CreateRecTask", SerializeRequest(req));

  json response = ParseApiEnvelope(resp_body, "/v1/CreateRecTask");
  std::string task_id;
  try {
    if (response.contains("Data") && response["Data"].is_object()) {
      task_id = response["Data"].value("RecTaskId", "");
    }
  } catch (const std::exception& e) {
    throw ASRError(kErrReadFailed,
                   std::string("unmarshal response failed: ") + e.what());
  }
  if (task_id.empty()) {
    throw ASRError(kErrServerError, "empty RecTaskId in response");
  }
  return task_id;
}

std::string FileRecognizer::CreateTaskFromData(const std::vector<uint8_t>& data,
                                               const std::string& voice_format,
                                               const std::string& engine_model_type) {
  (void)voice_format;  // kept for API parity with the Go SDK
  if (data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (data.size() > kMaxAudioSize) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 5MB limit");
  }
  CreateRecTaskRequest req;
  req.engine_model_type = engine_model_type;
  req.channel_num = 1;
  req.res_text_format = 1;
  req.source_type = 1;
  req.data = Base64Encode(data.data(), data.size());
  req.data_len = static_cast<int64_t>(data.size());
  return CreateTask(req);
}

std::string FileRecognizer::CreateTaskFromURL(const std::string& audio_url,
                                              const std::string& engine_model_type) {
  if (audio_url.empty()) {
    throw ASRError(kErrInvalidParam, "audio URL is empty");
  }
  CreateRecTaskRequest req;
  req.engine_model_type = engine_model_type;
  req.channel_num = 1;
  req.res_text_format = 1;
  req.source_type = 0;
  req.url = audio_url;
  return CreateTask(req);
}

std::string FileRecognizer::CreateTaskFromDataWithOptions(
    const std::vector<uint8_t>& raw_data, CreateRecTaskRequest* req) {
  if (req == nullptr) {
    throw ASRError(kErrInvalidParam, "request is null");
  }
  if (raw_data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (raw_data.size() > kMaxAudioSize) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 5MB limit");
  }
  req->source_type = 1;
  req->data = Base64Encode(raw_data.data(), raw_data.size());
  req->data_len = static_cast<int64_t>(raw_data.size());
  return CreateTask(*req);
}

TaskStatus FileRecognizer::DescribeTaskStatus(const std::string& rec_task_id) {
  if (rec_task_id.empty()) {
    throw ASRError(kErrInvalidParam, "RecTaskId is empty");
  }
  json body;
  body["RecTaskId"] = rec_task_id;
  std::string resp_body = DoRequest("/v1/DescribeTaskStatus", body.dump());

  json response = ParseApiEnvelope(resp_body, "/v1/DescribeTaskStatus");
  if (!response.contains("Data") || !response["Data"].is_object()) {
    throw ASRError(kErrServerError, "empty response from server");
  }
  // Field-type errors (e.g. "Status":"oops") raise nlohmann::type_error;
  // converge them onto the SDK error model.
  try {
    return ParseTaskStatus(response["Data"]);
  } catch (const std::exception& e) {
    throw ASRError(kErrReadFailed,
                   std::string("unmarshal response failed: ") + e.what());
  }
}

TaskStatus FileRecognizer::WaitForResult(const std::string& rec_task_id) {
  return WaitForResultWithInterval(rec_task_id, std::chrono::seconds(1),
                                   std::chrono::minutes(10));
}

TaskStatus FileRecognizer::WaitForResultWithInterval(
    const std::string& rec_task_id, std::chrono::milliseconds interval,
    std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  while (true) {
    TaskStatus status = DescribeTaskStatus(rec_task_id);

    if (status.status == kTaskStatusSuccess) {
      return status;
    }
    if (status.status == kTaskStatusFailed) {
      throw ASRError(kErrServerError, "task failed: " + status.error_msg +
                                          " (RecTaskId: " + status.rec_task_id + ")");
    }

    if (std::chrono::steady_clock::now() > deadline) {
      std::string timeout_str = std::to_string(timeout.count()) + "ms";
      throw ASRError(kErrTimeout, "task not completed within " + timeout_str +
                                      " (RecTaskId: " + rec_task_id +
                                      ", Status: " + status.status_str + ")");
    }

    std::this_thread::sleep_for(interval);
  }
}

std::string FileRecognizer::DoRequest(const std::string& path,
                                      const std::string& json_body) {
  std::string request_id = internal::GenerateUuid();

  std::string user_sig = credential_.user_sig();
  if (user_sig.empty()) {
    try {
      user_sig = GenUserSig(credential_.sdk_app_id(), credential_.secret_key(),
                            request_id, 86400);
    } catch (const ASRError& e) {
      throw ASRError(kErrAuthFailed,
                     "generate user sig failed: " + std::string(e.message()));
    }
  }

  // Shared by CreateRecTask and DescribeTaskStatus, so both report. The
  // request is authenticated by the UserSig header, so extra query
  // parameters are safe.
  std::string req_url =
      endpoint_ + path + "?AppId=" + credential_.app_id_str() +
      "&Secretid=" + credential_.app_id_str() + "&RequestId=" + request_id +
      "&Timestamp=" +
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()) +
      "&" + internal::SdkReportQuery();

  std::string err;
  internal::HttpResponse http_resp = internal::HttpPost(
      req_url, json_body,
      {"Content-Type: application/json; charset=utf-8",
       "X-TRTC-SdkAppId: " + std::to_string(credential_.sdk_app_id()),
       "X-TRTC-UserSig: " + user_sig},
      timeout_seconds_, &err);
  if (!err.empty()) {
    throw ASRError(kErrConnectFailed, err);
  }
  if (http_resp.status != 200) {
    throw ASRError(kErrServerError, "http status " +
                                        std::to_string(http_resp.status) + ": " +
                                        http_resp.body);
  }
  return http_resp.body;
}

}  // namespace trtc_asr
