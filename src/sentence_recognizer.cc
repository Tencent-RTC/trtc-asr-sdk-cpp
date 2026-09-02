#include "trtc_asr/sentence_recognizer.h"

#include <chrono>

#include "http_client.h"
#include "json_helper.h"
#include "sdkinfo.h"
#include "trtc_asr/usersig.h"
#include "ws_client.h"  // GenerateUuid

namespace trtc_asr {
namespace {

using nlohmann::json;

/// Serializes the request with Go's omitempty semantics: zero ints and empty
/// strings are omitted; SourceType is always sent.
std::string SerializeRequest(const SentenceRecognitionRequest& req) {
  json j;
  j["EngSerViceType"] = req.eng_service_type;
  j["SourceType"] = req.source_type;
  j["VoiceFormat"] = req.voice_format;
  if (!req.url.empty()) j["Url"] = req.url;
  if (!req.data.empty()) j["Data"] = req.data;
  if (req.data_len != 0) j["DataLen"] = req.data_len;
  if (req.word_info != 0) j["WordInfo"] = req.word_info;
  if (req.filter_dirty != 0) j["FilterDirty"] = req.filter_dirty;
  if (req.filter_modal != 0) j["FilterModal"] = req.filter_modal;
  if (req.filter_punc != 0) j["FilterPunc"] = req.filter_punc;
  if (req.convert_num_mode != 0) j["ConvertNumMode"] = req.convert_num_mode;
  if (!req.hotword_id.empty()) j["HotwordId"] = req.hotword_id;
  if (!req.hotword_list.empty()) j["HotwordList"] = req.hotword_list;
  if (!req.customization_id.empty()) j["CustomizationId"] = req.customization_id;
  if (req.input_sample_rate != 0) j["InputSampleRate"] = req.input_sample_rate;
  if (!req.language.empty()) j["Language"] = req.language;
  return j.dump();
}

void ValidateRequest(const SentenceRecognitionRequest& req) {
  if (req.eng_service_type.empty()) {
    throw ASRError(kErrInvalidParam, "EngServiceType is required");
  }
  if (req.voice_format.empty()) {
    throw ASRError(kErrInvalidParam, "VoiceFormat is required");
  }
  if (req.source_type == SentenceRecognizer::kSourceTypeURL && req.url.empty()) {
    throw ASRError(kErrInvalidParam, "Url is required when SourceType=0");
  }
  if (req.source_type == SentenceRecognizer::kSourceTypeData && req.data.empty()) {
    throw ASRError(kErrInvalidParam, "Data is required when SourceType=1");
  }
}

SentenceRecognitionResult ParseResult(const json& response) {
  SentenceRecognitionResult out;
  out.result = response.value("Result", "");
  out.audio_duration = response.value("AudioDuration", (int64_t)0);
  out.word_size = response.value("WordSize", 0);
  out.request_id = response.value("RequestId", "");
  if (response.contains("WordList") && response["WordList"].is_array()) {
    for (const auto& w : response["WordList"]) {
      SentenceWord word;
      word.word = w.value("Word", "");
      word.start_time = w.value("StartTime", (int64_t)0);
      word.end_time = w.value("EndTime", (int64_t)0);
      out.word_list.push_back(std::move(word));
    }
  }
  return out;
}

}  // namespace

SentenceRecognizer::SentenceRecognizer(const Credential& credential)
    : credential_(credential), endpoint_() {}

SentenceRecognitionResult SentenceRecognizer::Recognize(
    const SentenceRecognitionRequest& req) {
  ValidateRequest(req);

  std::string request_id = internal::GenerateUuid();

  // Generate UserSig using RequestId as the userID per protocol spec.
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

  // The SDK identification fragment is appended to the query; the request is
  // authenticated by the UserSig header, so extra parameters are safe.
  std::string req_url =
      ResolveHTTPEndpoint(endpoint_, credential_.site()) +
      "/v1/SentenceRecognition?AppId=" + credential_.app_id_str() +
      "&Secretid=" + credential_.app_id_str() + "&RequestId=" + request_id +
      "&Timestamp=" +
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()) +
      "&" + internal::SdkReportQuery();

  std::string body = SerializeRequest(req);

  std::string err;
  internal::HttpResponse http_resp = internal::HttpPost(
      req_url, body,
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

  json root;
  try {
    root = json::parse(http_resp.body);
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
  // Field-type errors (e.g. "AudioDuration":"oops") raise
  // nlohmann::type_error; converge them onto the SDK error model.
  try {
    return ParseResult(response);
  } catch (const std::exception& e) {
    throw ASRError(kErrReadFailed,
                   std::string("unmarshal response failed: ") + e.what());
  }
}

SentenceRecognitionResult SentenceRecognizer::RecognizeData(
    const std::vector<uint8_t>& data, const std::string& voice_format,
    const std::string& engine_model_type) {
  if (data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (data.size() > kMaxAudioSize) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 3MB limit");
  }
  SentenceRecognitionRequest req;
  req.eng_service_type = engine_model_type;
  req.source_type = kSourceTypeData;
  req.voice_format = voice_format;
  req.data = Base64Encode(data.data(), data.size());
  req.data_len = static_cast<int64_t>(data.size());
  return Recognize(req);
}

SentenceRecognitionResult SentenceRecognizer::RecognizeURL(
    const std::string& audio_url, const std::string& voice_format,
    const std::string& engine_model_type) {
  if (audio_url.empty()) {
    throw ASRError(kErrInvalidParam, "audio URL is empty");
  }
  SentenceRecognitionRequest req;
  req.eng_service_type = engine_model_type;
  req.source_type = kSourceTypeURL;
  req.voice_format = voice_format;
  req.url = audio_url;
  return Recognize(req);
}

SentenceRecognitionResult SentenceRecognizer::RecognizeDataWithOptions(
    const std::vector<uint8_t>& raw_data, SentenceRecognitionRequest* req) {
  if (req == nullptr) {
    throw ASRError(kErrInvalidParam, "request is null");
  }
  if (raw_data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (raw_data.size() > kMaxAudioSize) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 3MB limit");
  }
  req->source_type = kSourceTypeData;
  req->data = Base64Encode(raw_data.data(), raw_data.size());
  req->data_len = static_cast<int64_t>(raw_data.size());
  return Recognize(*req);
}

}  // namespace trtc_asr
