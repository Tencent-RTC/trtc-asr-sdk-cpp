#include "trtc_asr/v3.h"

#include <cctype>
#include <cmath>

#include "http_client.h"
#include "sdkinfo.h"
#include "json_helper.h"
#include "trtc_asr/usersig.h"
#include "ws_client.h"

namespace trtc_asr {
namespace v3 {

// The v2 machinery (WsClient / HttpPost / shared state) lives in
// trtc_asr::internal; v3::internal holds the v3-specific validators. Alias
// to avoid the name shadowing between the two.
namespace v2internal = ::trtc_asr::internal;

namespace {

using S = v2internal::RecognizerSharedState;

constexpr int kHandshakeTimeoutMs = 10000;
constexpr int kReadPollMs = 100;

// Server-side accepted ranges (asr-proxy validator_v3.go).
constexpr size_t kMaxVoiceIdLen = 128;
constexpr int kMinMaxSpeakTime = 5000;
constexpr int kMaxMaxSpeakTime = 90000;
constexpr int kMinVadSilenceTimeMs = 240;
constexpr int kMaxVadSilenceTimeMs = 2000;

/// Wraps params into the {"auth": ..., "params": ...} offline body, injecting
/// the SDK telemetry into the params block.
nlohmann::json OfflineEnvelope(const Credential& credential, const std::string& request_id,
                               nlohmann::json params) {
  std::string user_sig = credential.user_sig();
  if (user_sig.empty()) {
    try {
      user_sig = GenUserSig(credential.sdk_app_id(), credential.secret_key(), request_id, 86400);
    } catch (const ASRError& e) {
      throw ASRError(kErrAuthFailed,
                     "generate user sig failed: " + std::string(e.message()));
    }
  }
  params["sdk_info"] = v2internal::SdkReportParams();
  nlohmann::json auth;
  auth["sdkappid"] = std::to_string(credential.sdk_app_id());
  auth["usersig"] = user_sig;
  auth["request_id"] = request_id;
  nlohmann::json body;
  body["auth"] = std::move(auth);
  body["params"] = std::move(params);
  return body;
}

/// POSTs the envelope and returns the decoded flat response. Two failure
/// shapes are rejected: a body that is not a JSON object, and a non-2xx
/// status whose body carries no numeric code (a gateway/LB JSON error page
/// would otherwise decode to code==0 and be mistaken for success).
nlohmann::json PostFlat(const Credential& credential, const std::string& endpoint,
                        const std::string& path, const nlohmann::json& body,
                        std::chrono::seconds timeout) {
  const std::string url = ResolveHTTPEndpoint(endpoint, credential.site()) + path;
  v2internal::HttpResponse resp =
      v2internal::HttpPost(url, body.dump(), {"Content-Type: application/json; charset=utf-8"},
                           static_cast<int>(timeout.count()), nullptr);
  nlohmann::json data;
  try {
    data = nlohmann::json::parse(resp.body);
  } catch (const std::exception& e) {
    throw ASRError(kErrServerError, "invalid response (http " +
                                        std::to_string(resp.status) + "): " + e.what());
  }
  if (!data.is_object()) {
    throw ASRError(kErrServerError, "invalid response (http " +
                                        std::to_string(resp.status) +
                                        "): not a JSON object");
  }
  const int64_t code = data.value("code", 0);
  if ((resp.status < 200 || resp.status > 299) && code == 0) {
    std::string preview = resp.body.substr(0, 256);
    if (resp.body.size() > 256) preview += "...";
    throw ASRError(kErrServerError, "http " + std::to_string(resp.status) +
                                        " with non-v3 response body: " + preview);
  }
  return data;
}

/// Converts a v3 flat error into an ASRError carrying the server code.
ASRError ServerError(const nlohmann::json& data) {
  const int code = static_cast<int>(data.value("code", 0));
  std::string message = data.value("message", "");
  const std::string request_id = data.value("request_id", "");
  if (!request_id.empty()) {
    message += " (request_id: " + request_id + ")";
  }
  return ASRError(code, message);
}

/// Checks a flat response body for code != 0 and throws.
void CheckCode(const nlohmann::json& data) {
  if (data.value("code", 0) != 0) {
    throw ServerError(data);
  }
}

}  // namespace

Credential NewCredential(int64_t sdk_app_id, std::string secret_key) {
  return Credential(0, sdk_app_id, std::move(secret_key));
}

namespace internal {

void ValidateEnumOption(const std::string& name, int value,
                        const std::vector<int>& allowed) {
  for (int candidate : allowed) {
    if (value == candidate) return;
  }
  std::string list;
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += std::to_string(allowed[i]);
    if (i + 1 < allowed.size()) list += ", ";
  }
  throw ASRError(kErrInvalidParam,
                 name + " must be one of [" + list + "], got " + std::to_string(value));
}

void ValidateVadTuning(const std::optional<int>& vad_level,
                       const std::optional<double>& noise_threshold) {
  if (vad_level.has_value() && *vad_level != 0 && *vad_level != 1) {
    throw ASRError(kErrInvalidParam,
                   "VadLevel must be 0 (high recall) or 1 (far-field filtering), got " +
                       std::to_string(*vad_level));
  }
  if (noise_threshold.has_value() &&
      !(std::isfinite(*noise_threshold) && *noise_threshold >= 0.0 && *noise_threshold <= 4.0)) {
    throw ASRError(kErrInvalidParam,
                   "NoiseThreshold must be between 0.0 and 4.0, got " +
                       std::to_string(*noise_threshold));
  }
}

void ValidateSpeakerContext(int mode, int diarization) {
  // enable_speaker_context accepts 0 (off), 1 (sync) or 2 (async); the server
  // silently normalizes anything else to off, but a caller that meant to
  // enable resumption is better served by an immediate error than by a
  // session that quietly never returns a speaker_context_id.
  if (mode != kSpeakerContextOff && mode != kSpeakerContextSync &&
      mode != kSpeakerContextAsync) {
    throw ASRError(kErrInvalidParam,
                   "EnableSpeakerContext must be 0 (off), 1 (sync) or 2 (async), got " +
                       std::to_string(mode));
  }
  // The server ignores the speaker-context parameters entirely when speaker
  // diarization is off, so that combination is a caller mistake as well.
  if (mode != kSpeakerContextOff && diarization == kSpeakerDiarizationOff) {
    throw ASRError(kErrInvalidParam, "EnableSpeakerContext requires SpeakerDiarization=1 or 3");
  }
}

void ValidateSpeakerDiarization(int mode, int speaker_number,
                                const std::vector<SpeakerRole>& roles,
                                const std::vector<std::string>& voiceprint_ids) {
  if (mode != kSpeakerDiarizationOff && mode != 1 && mode != kSpeakerDiarizationVoiceprint) {
    throw ASRError(kErrInvalidParam,
                   "SpeakerDiarization must be 0 (off), 1 (cluster) or 3 (voiceprint), got " +
                       std::to_string(mode));
  }
  if (speaker_number < 0) {
    throw ASRError(kErrInvalidParam, "SpeakerNumber must be >= 0 (0 = auto detection), got " +
                                         std::to_string(speaker_number));
  }
  const bool has_enrollment = !roles.empty() || !voiceprint_ids.empty();
  if (mode != kSpeakerDiarizationVoiceprint && has_enrollment) {
    throw ASRError(kErrInvalidParam,
                   "SpeakerRoles/VoiceprintIds require SpeakerDiarization=3");
  }
  for (size_t i = 0; i < roles.size(); ++i) {
    if (roles[i].role_name.empty()) {
      throw ASRError(kErrInvalidParam,
                     "SpeakerRoles[" + std::to_string(i) + "].RoleName is empty");
    }
    const std::string& url = roles[i].audio_url;
    if (url.find("http://") != 0 && url.find("https://") != 0) {
      throw ASRError(kErrInvalidParam, "SpeakerRoles[" + std::to_string(i) +
                                           "].AudioURL must be a valid http/https URL");
    }
  }
  for (size_t i = 0; i < voiceprint_ids.size(); ++i) {
    if (voiceprint_ids[i].empty()) {
      throw ASRError(kErrInvalidParam,
                     "VoiceprintIds[" + std::to_string(i) + "] is empty");
    }
  }
}

void ValidateAudioURLs(int source_type, const std::string& url, const std::string& data,
                       const std::vector<AudioURLItem>& audio_urls) {
  if (source_type != kSourceTypeUrl || !url.empty() || !data.empty()) {
    throw ASRError(kErrInvalidParam,
                   "AudioURLs cannot be used together with non-zero sourceType, url or data");
  }
  std::vector<int64_t> seen;
  for (size_t i = 0; i < audio_urls.size(); ++i) {
    const auto& item = audio_urls[i];
    if (item.index < 0) {
      throw ASRError(kErrInvalidParam,
                     "AudioURLs[" + std::to_string(i) + "].Index must be non-negative");
    }
    for (int64_t prev : seen) {
      if (prev == item.index) {
        throw ASRError(kErrInvalidParam,
                       "AudioURLs index duplicated: " + std::to_string(item.index));
      }
    }
    seen.push_back(item.index);
    if (item.url.find("http://") != 0 && item.url.find("https://") != 0) {
      throw ASRError(kErrInvalidParam,
                     "AudioURLs[" + std::to_string(i) +
                         "].URL must be a valid http/https URL");
    }
  }
}

}  // namespace internal

// ---------------------------------------------------------------- transcribe

nlohmann::json TranscribeRequestToWire(const TranscribeRequest& req) {
  nlohmann::json d;
  d["engine_model_type"] = req.engine_model_type;
  d["source_type"] = req.source_type;
  d["voice_format"] = req.voice_format;
  if (req.source_type == kSourceTypeUrl) {
    d["url"] = req.url;
  } else {
    d["data"] = req.data;
    d["data_len"] = req.data_len;
  }
  if (req.word_info != 0) d["word_info"] = req.word_info;
  if (req.filter_dirty != 0) d["filter_dirty"] = req.filter_dirty;
  if (req.filter_modal != 0) d["filter_modal"] = req.filter_modal;
  if (req.filter_punc != 0) d["filter_punc"] = req.filter_punc;
  if (req.convert_num_mode != 0) d["convert_num_mode"] = req.convert_num_mode;
  if (!req.hotword_id.empty()) d["hotword_id"] = req.hotword_id;
  if (!req.customization_id.empty()) d["customization_id"] = req.customization_id;
  if (!req.hotword_list.empty()) d["hotword_list"] = req.hotword_list;
  if (req.input_sample_rate != 0) d["input_sample_rate"] = req.input_sample_rate;
  if (req.needvad.has_value()) d["needvad"] = *req.needvad;
  if (req.vad_silence_time.has_value()) d["vad_silence_time"] = *req.vad_silence_time;
  if (!req.language.empty()) d["language"] = req.language;
  if (req.speaker_diarization != 0) {
    d["speaker_diarization"] = req.speaker_diarization;
    if (req.speaker_number != 0) d["speaker_number"] = req.speaker_number;
  }
  if (req.context.has_value()) {
    nlohmann::json c;
    if (!req.context->text.empty()) c["text"] = req.context->text;
    if (!req.context->terms.empty()) c["terms"] = req.context->terms;
    if (!req.context->general.empty()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& kv : req.context->general) {
        nlohmann::json obj;
        obj["key"] = kv.key;
        obj["value"] = kv.value;
        arr.push_back(std::move(obj));
      }
      c["general"] = std::move(arr);
    }
    if (!c.empty()) d["context"] = std::move(c);
  }
  return d;
}

void TranscribeRequest::Validate() const {
  if (engine_model_type.empty()) {
    throw ASRError(kErrInvalidParam, "engine_model_type is required");
  }
  if (voice_format.empty()) {
    throw ASRError(kErrInvalidParam, "voice_format is required");
  }
  if (source_type == kSourceTypeUrl && url.empty()) {
    throw ASRError(kErrInvalidParam, "url is required when source_type=0");
  }
  if (source_type == kSourceTypeData && data.empty()) {
    throw ASRError(kErrInvalidParam, "data is required when source_type=1");
  }
  if (speaker_diarization != 0 || speaker_number != 0) {
    internal::ValidateSpeakerDiarization(speaker_diarization, speaker_number, {}, {});
  }
  if (needvad.has_value()) {
    internal::ValidateEnumOption("needvad", *needvad, {0, 1});
  }
  // 8000 is the only supported override; 0 means "use the engine rate".
  internal::ValidateEnumOption("input_sample_rate", input_sample_rate, {0, 8000});
}

Word WordFromJson(const nlohmann::json& j) {
  Word w;
  w.word = j.value("word", "");
  w.start_time = j.value("start_time", 0);
  w.end_time = j.value("end_time", 0);
  return w;
}

TranscribeResponse TranscribeResponseFromJson(const nlohmann::json& j) {
  TranscribeResponse r;
  r.code = j.value("code", 0);
  r.message = j.value("message", "");
  r.request_id = j.value("request_id", "");
  r.result = j.value("result", "");
  r.audio_duration = j.value("audio_duration", 0);
  r.language = j.value("language", "");
  r.language_b47 = j.value("language_b47", "");
  r.word_size = j.value("word_size", 0);
  if (j.contains("word_list") && j["word_list"].is_array()) {
    for (const auto& w : j["word_list"]) r.word_list.push_back(WordFromJson(w));
  }
  return r;
}

SentenceRecognizer::SentenceRecognizer(Credential credential)
    : credential_(std::move(credential)) {}

TranscribeResponse SentenceRecognizer::Recognize(const TranscribeRequest& req) {
  req.Validate();
  const std::string request_id = v2internal::GenerateUuid();
  const nlohmann::json data =
      PostFlat(credential_, endpoint_, "/v3/transcribe",
               OfflineEnvelope(credential_, request_id, TranscribeRequestToWire(req)), timeout_);
  CheckCode(data);
  return TranscribeResponseFromJson(data);
}

TranscribeResponse SentenceRecognizer::RecognizeData(const std::vector<uint8_t>& data,
                                                     const std::string& voice_format,
                                                     const std::string& engine_model_type) {
  if (data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (data.size() > 3 * 1024 * 1024) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 3MB limit");
  }
  TranscribeRequest req;
  req.engine_model_type = engine_model_type;
  req.source_type = kSourceTypeData;
  req.voice_format = voice_format;
  req.data = Base64Encode(data.data(), data.size());
  req.data_len = static_cast<int64_t>(data.size());
  return Recognize(req);
}

TranscribeResponse SentenceRecognizer::RecognizeDataWithOptions(const std::vector<uint8_t>& data,
                                                                TranscribeRequest* req) {
  if (req == nullptr) {
    throw ASRError(kErrInvalidParam, "request is null");
  }
  if (data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (data.size() > 3 * 1024 * 1024) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 3MB limit");
  }
  req->source_type = kSourceTypeData;
  req->data = Base64Encode(data.data(), data.size());
  req->data_len = static_cast<int64_t>(data.size());
  return Recognize(*req);
}

TranscribeResponse SentenceRecognizer::RecognizeUrl(const std::string& audio_url,
                                                    const std::string& voice_format,
                                                    const std::string& engine_model_type) {
  if (audio_url.empty()) {
    throw ASRError(kErrInvalidParam, "audio URL is empty");
  }
  TranscribeRequest req;
  req.engine_model_type = engine_model_type;
  req.source_type = kSourceTypeUrl;
  req.voice_format = voice_format;
  req.url = audio_url;
  return Recognize(req);
}

// ---------------------------------------------------------------- create

nlohmann::json CreateTranscriptionRequestToWire(const CreateTranscriptionRequest& req) {
  nlohmann::json d;
  d["engine_model_type"] = req.engine_model_type;
  d["channel_num"] = req.channel_num;
  d["res_text_format"] = req.res_text_format;
  d["source_type"] = req.source_type;
  // Distributed mode (req.source_type=0 with empty req.url/req.data) must omit both
  // fields entirely — the server rejects any req.url/req.data alongside req.audio_urls.
  if (req.source_type == kSourceTypeUrl && !req.url.empty()) {
    d["url"] = req.url;
  } else if (req.source_type == kSourceTypeData && !req.data.empty()) {
    d["data"] = req.data;
    d["data_len"] = req.data_len;
  }
  if (!req.audio_urls.empty()) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : req.audio_urls) {
      nlohmann::json obj;
      obj["index"] = item.index;
      obj["req.url"] = item.url;
      if (!item.label.empty()) obj["label"] = item.label;
      arr.push_back(std::move(obj));
    }
    d["audio_urls"] = std::move(arr);
  }
  if (!req.callback_url.empty()) d["callback_url"] = req.callback_url;
  if (req.speaker_diarization != 0) {
    d["speaker_diarization"] = req.speaker_diarization;
    if (req.speaker_number != 0) d["speaker_number"] = req.speaker_number;
  }
  if (req.speaker_diarization == kSpeakerDiarizationVoiceprint) {
    if (!req.voiceprint_ids.empty()) d["voiceprint_ids"] = req.voiceprint_ids;
    if (!req.speaker_roles.empty()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& role : req.speaker_roles) {
        nlohmann::json obj;
        obj["role_name"] = role.role_name;
        obj["audio_url"] = role.audio_url;
        arr.push_back(std::move(obj));
      }
      d["speaker_roles"] = std::move(arr);
    }
  }
  if (!req.hotword_id.empty()) d["hotword_id"] = req.hotword_id;
  if (!req.customization_id.empty()) d["customization_id"] = req.customization_id;
  if (!req.hotword_list.empty()) d["hotword_list"] = req.hotword_list;
  if (!req.keyword_lib_id_list.empty()) d["keyword_lib_id_list"] = req.keyword_lib_id_list;
  if (!req.replace_text_id.empty()) d["replace_text_id"] = req.replace_text_id;
  if (req.convert_num_mode != 0) d["convert_num_mode"] = req.convert_num_mode;
  if (req.filter_dirty != 0) d["filter_dirty"] = req.filter_dirty;
  if (req.filter_punc != 0) d["filter_punc"] = req.filter_punc;
  if (req.filter_modal != 0) d["filter_modal"] = req.filter_modal;
  if (req.sentence_max_length != 0) d["sentence_max_length"] = req.sentence_max_length;
  if (!req.extra.empty()) d["extra"] = req.extra;
  if (req.vad_silence_ms != 0) d["vad_silence_ms"] = req.vad_silence_ms;
  if (req.vad_level.has_value()) d["vad_level"] = *req.vad_level;
  if (req.noise_threshold.has_value()) d["noise_threshold"] = *req.noise_threshold;
  if (!req.language.empty()) d["language"] = req.language;
  if (req.context.has_value()) {
    nlohmann::json c;
    if (!req.context->text.empty()) c["text"] = req.context->text;
    if (!req.context->terms.empty()) c["terms"] = req.context->terms;
    if (!req.context->general.empty()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& kv : req.context->general) {
        nlohmann::json obj;
        obj["key"] = kv.key;
        obj["value"] = kv.value;
        arr.push_back(std::move(obj));
      }
      c["general"] = std::move(arr);
    }
    if (!c.empty()) d["context"] = std::move(c);
  }
  return d;
}

void CreateTranscriptionRequest::Validate() const {
  if (engine_model_type.empty()) {
    throw ASRError(kErrInvalidParam, "engine_model_type is required");
  }
  if (channel_num != 1 && channel_num != 2) {
    throw ASRError(kErrInvalidParam, "channel_num must be 1 or 2");
  }
  if (res_text_format < 0 || res_text_format > 3) {
    throw ASRError(kErrInvalidParam,
                   "res_text_format must be one of [0, 1, 2, 3], got " +
                       std::to_string(res_text_format));
  }
  if (channel_num == 2 && speaker_diarization != 0) {
    throw ASRError(kErrInvalidParam,
                   "speaker_diarization is not supported for stereo (channel_num=2); "
                   "sentences carry channel_id instead");
  }
  if (!audio_urls.empty()) {
    internal::ValidateAudioURLs(source_type, url, data, audio_urls);
  } else {
    if (source_type == kSourceTypeUrl && url.empty()) {
      throw ASRError(kErrInvalidParam, "url is required when source_type=0");
    }
    if (source_type == kSourceTypeData && data.empty()) {
      throw ASRError(kErrInvalidParam, "data is required when source_type=1");
    }
  }
  internal::ValidateSpeakerDiarization(speaker_diarization, speaker_number, speaker_roles,
                                       voiceprint_ids);
  internal::ValidateVadTuning(vad_level, noise_threshold);
}

SentenceDetail SentenceDetailFromJson(const nlohmann::json& j) {
  SentenceDetail d;
  d.final_sentence = j.value("final_sentence", "");
  d.slice_sentence = j.value("slice_sentence", "");
  d.written_text = j.value("written_text", "");
  d.start_ms = j.value("start_ms", 0);
  d.end_ms = j.value("end_ms", 0);
  d.words_num = j.value("words_num", 0);
  if (j.contains("words") && j["words"].is_array()) {
    for (const auto& w : j["words"]) d.words.push_back(WordFromJson(w));
  }
  d.speech_speed = j.value("speech_speed", 0.0);
  d.speaker_id = j.value("speaker_id", 0);
  d.channel_id = j.value("channel_id", 0);
  d.speaker_role_name = j.value("speaker_role_name", "");
  d.silence_time = j.value("silence_time", 0);
  d.language = j.value("language", "");
  d.language_b47 = j.value("language_b47", "");
  return d;
}

TranscriptionStatus TranscriptionStatusFromJson(const nlohmann::json& j) {
  TranscriptionStatus s;
  s.code = j.value("code", 0);
  s.message = j.value("message", "");
  s.request_id = j.value("request_id", "");
  s.transcription_id = j.value("transcription_id", "");
  s.status = j.value("status", 0);
  s.status_str = j.value("status_str", "");
  s.progress = j.value("progress", 0);
  s.audio_duration = j.value("audio_duration", 0.0);
  s.result = j.value("result", "");
  if (j.contains("result_detail") && j["result_detail"].is_array()) {
    for (const auto& d : j["result_detail"]) {
      s.result_detail.push_back(SentenceDetailFromJson(d));
    }
  }
  s.error_msg = j.value("error_msg", "");
  return s;
}

FileRecognizer::FileRecognizer(Credential credential) : credential_(std::move(credential)) {}

std::string FileRecognizer::CreateTask(const CreateTranscriptionRequest& req) {
  req.Validate();
  const std::string request_id = v2internal::GenerateUuid();
  const nlohmann::json data =
      PostFlat(credential_, endpoint_, "/v3/create_transcription",
               OfflineEnvelope(credential_, request_id, CreateTranscriptionRequestToWire(req)),
               timeout_);
  CheckCode(data);
  const std::string id = data.value("transcription_id", "");
  if (id.empty()) {
    throw ASRError(kErrServerError, "empty transcription_id in response");
  }
  return id;
}

std::string FileRecognizer::CreateTaskFromData(const std::vector<uint8_t>& data,
                                               const std::string& engine_model_type) {
  if (data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (data.size() > 5 * 1024 * 1024) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 5MB limit");
  }
  CreateTranscriptionRequest req;
  req.engine_model_type = engine_model_type;
  req.channel_num = 1;
  req.res_text_format = 1;
  req.source_type = kSourceTypeData;
  req.data = Base64Encode(data.data(), data.size());
  req.data_len = static_cast<int64_t>(data.size());
  return CreateTask(req);
}

std::string FileRecognizer::CreateTaskFromDataWithOptions(const std::vector<uint8_t>& data,
                                                          CreateTranscriptionRequest* req) {
  if (req == nullptr) {
    throw ASRError(kErrInvalidParam, "request is null");
  }
  if (data.empty()) {
    throw ASRError(kErrInvalidParam, "audio data is empty");
  }
  if (data.size() > 5 * 1024 * 1024) {
    throw ASRError(kErrInvalidParam, "audio data exceeds 5MB limit");
  }
  req->source_type = kSourceTypeData;
  req->data = Base64Encode(data.data(), data.size());
  req->data_len = static_cast<int64_t>(data.size());
  return CreateTask(*req);
}

std::string FileRecognizer::CreateTaskFromUrl(const std::string& audio_url,
                                              const std::string& engine_model_type) {
  if (audio_url.empty()) {
    throw ASRError(kErrInvalidParam, "audio URL is empty");
  }
  CreateTranscriptionRequest req;
  req.engine_model_type = engine_model_type;
  req.channel_num = 1;
  req.res_text_format = 1;
  req.source_type = kSourceTypeUrl;
  req.url = audio_url;
  return CreateTask(req);
}

TranscriptionStatus FileRecognizer::DescribeTask(const std::string& transcription_id) {
  if (transcription_id.empty()) {
    throw ASRError(kErrInvalidParam, "transcription_id is empty");
  }
  const std::string request_id = v2internal::GenerateUuid();
  nlohmann::json params;
  params["transcription_id"] = transcription_id;
  const nlohmann::json data =
      PostFlat(credential_, endpoint_, "/v3/describe_transcription",
               OfflineEnvelope(credential_, request_id, std::move(params)), timeout_);
  CheckCode(data);
  return TranscriptionStatusFromJson(data);
}

TranscriptionStatus FileRecognizer::WaitForResult(const std::string& transcription_id) {
  return WaitForResultWithInterval(transcription_id, std::chrono::seconds(1),
                                   std::chrono::seconds(600));
}

TranscriptionStatus FileRecognizer::WaitForResultWithInterval(
    const std::string& transcription_id, std::chrono::milliseconds interval,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    TranscriptionStatus status = DescribeTask(transcription_id);
    if (status.status == kTaskStatusSuccess) return status;
    if (status.status == kTaskStatusFailed) {
      throw ASRError(kErrServerError, "task failed: " + status.error_msg +
                                          " (transcription_id: " + status.transcription_id +
                                          ")");
    }
    if (std::chrono::steady_clock::now() > deadline) {
      throw ASRError(kErrTimeout,
                     "task not completed within " +
                         std::to_string(timeout.count()) + "ms (transcription_id: " +
                         transcription_id + ", Status: " + status.status_str + ")");
    }
    std::this_thread::sleep_for(interval);
  }
}

// ---------------------------------------------------------------- streaming

SpeechRecognizer::SpeechRecognizer(const Credential& credential,
                                   std::string engine_model_type,
                                   SpeechRecognitionListener* listener)
    : credential_(credential),
      listener_(listener),
      engine_model_type_(std::move(engine_model_type)) {}

SpeechRecognizer::~SpeechRecognizer() {
  if (shared_->state.load() == S::kStateRunning_) {
    try {
      Stop();
    } catch (...) {
    }
  }
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
}

void SpeechRecognizer::SetWriteTimeout(std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds(0)) {
    write_timeout_ = std::chrono::milliseconds(5000);
  } else if (timeout < std::chrono::milliseconds(50)) {
    write_timeout_ = std::chrono::milliseconds(50);
  } else if (timeout > std::chrono::milliseconds(30000)) {
    write_timeout_ = std::chrono::milliseconds(30000);
  } else {
    write_timeout_ = timeout;
  }
}

void SpeechRecognizer::SetStopTimeout(std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds(0)) {
    stop_timeout_ = std::chrono::milliseconds(10000);
  } else if (timeout < std::chrono::milliseconds(1000)) {
    stop_timeout_ = std::chrono::milliseconds(1000);
  } else if (timeout > std::chrono::milliseconds(60000)) {
    stop_timeout_ = std::chrono::milliseconds(60000);
  } else {
    stop_timeout_ = timeout;
  }
}

void SpeechRecognizer::ValidateOptions() const {
  if (voice_id_.size() > kMaxVoiceIdLen) {
    throw ASRError(kErrInvalidParam,
                   "VoiceID length must not exceed " + std::to_string(kMaxVoiceIdLen));
  }
  internal::ValidateSpeakerDiarization(speaker_diarization_, speaker_number_, speaker_roles_,
                                       voiceprint_ids_);
  internal::ValidateSpeakerContext(enable_speaker_context_, speaker_diarization_);
  internal::ValidateVadTuning(vad_level_, noise_threshold_);
  if (max_speak_time_ != 0 &&
      (max_speak_time_ < kMinMaxSpeakTime || max_speak_time_ > kMaxMaxSpeakTime)) {
    throw ASRError(kErrInvalidParam,
                   "MaxSpeakTime must be between " + std::to_string(kMinMaxSpeakTime) +
                       " and " + std::to_string(kMaxMaxSpeakTime) + " ms, got " +
                       std::to_string(max_speak_time_));
  }
  // vad_silence_time==0 means "not set"; the range check only applies to an
  // explicitly set value with VAD enabled, matching the server rule.
  if (vad_silence_time_ != 0 && need_vad_ == 1 &&
      (vad_silence_time_ < kMinVadSilenceTimeMs || vad_silence_time_ > kMaxVadSilenceTimeMs)) {
    throw ASRError(kErrInvalidParam,
                   "VadSilenceTime must be between " + std::to_string(kMinVadSilenceTimeMs) +
                       " and " + std::to_string(kMaxVadSilenceTimeMs) +
                       " ms (needvad=1), got " + std::to_string(vad_silence_time_));
  }
  internal::ValidateEnumOption("NeedVad", need_vad_, {0, 1});
  internal::ValidateEnumOption("ConvertNumMode", convert_num_mode_, {0, 1, 3});
  internal::ValidateEnumOption("FilterDirty", filter_dirty_, {0, 1, 2});
  internal::ValidateEnumOption("FilterModal", filter_modal_, {0, 1, 2});
  internal::ValidateEnumOption("FilterPunc", filter_punc_, {0, 1});
  internal::ValidateEnumOption("WordInfo", word_info_, {0, 1, 2, 100});
  internal::ValidateEnumOption("WordWithSpace", word_with_space_, {0, 1});
  internal::ValidateEnumOption("VoiceFormat", voice_format_, {1, 4, 6, 8, 10, 11, 12, 14, 16});
  if (filter_empty_result_.has_value()) {
    internal::ValidateEnumOption("FilterEmptyResult", *filter_empty_result_, {0, 1});
  }
  // 8000 is the only supported override; 0 means "use the engine rate".
  internal::ValidateEnumOption("InputSampleRate", input_sample_rate_, {0, 8000});
}

void SpeechRecognizer::Connect() {
  if (voice_id_.empty()) {
    voice_id_ = v2internal::GenerateUuid();
  }

  // Resolve UserSig locally without mutating the shared credential. The v3
  // signature identifier is the voice_id.
  std::string user_sig = credential_.user_sig();
  if (user_sig.empty()) {
    try {
      user_sig = GenUserSig(credential_.sdk_app_id(), credential_.secret_key(), voice_id_, 86400);
    } catch (const ASRError& e) {
      throw ASRError(kErrAuthFailed,
                     "generate user sig failed: " + std::string(e.message()));
    }
  }

  nlohmann::json params;
  params["voice_id"] = voice_id_;
  params["engine_model_type"] = engine_model_type_;
  if (!language_.empty()) params["language"] = language_;
  params["voice_format"] = voice_format_;
  // SDK-managed defaults are always sent — including an explicit 0, which
  // v3 honors (the v2 query transport dropped it).
  params["needvad"] = need_vad_;
  params["convert_num_mode"] = convert_num_mode_;
  // SDK telemetry: the gateway replays the start frame byte-for-byte to the
  // worker, which ignores unknown keys, so it survives in server-side dumps
  // without disturbing the protocol.
  params["sdk_info"] = v2internal::SdkReportParams();
  if (!hotword_id_.empty()) params["hotword_id"] = hotword_id_;
  if (!hotword_list_.empty()) params["hotword_list"] = hotword_list_;
  if (filter_dirty_ != 0) params["filter_dirty"] = filter_dirty_;
  if (filter_modal_ != 0) params["filter_modal"] = filter_modal_;
  if (filter_punc_ != 0) params["filter_punc"] = filter_punc_;
  if (filter_empty_result_.has_value()) params["filter_empty_result"] = *filter_empty_result_;
  if (word_info_ != 0) params["word_info"] = word_info_;
  if (word_with_space_ != 0) params["word_with_space"] = word_with_space_;
  if (vad_silence_time_ != 0) params["vad_silence_time"] = vad_silence_time_;
  if (vad_level_.has_value()) params["vad_level"] = *vad_level_;
  if (noise_threshold_.has_value()) params["noise_threshold"] = *noise_threshold_;
  if (max_speak_time_ != 0) params["max_speak_time"] = max_speak_time_;
  if (input_sample_rate_ != 0) params["input_sample_rate"] = input_sample_rate_;
  if (speaker_diarization_ != 0) {
    params["speaker_diarization"] = speaker_diarization_;
    if (speaker_number_ != 0) params["speaker_number"] = speaker_number_;
  }
  if (speaker_diarization_ == kSpeakerDiarizationVoiceprint) {
    if (!voiceprint_ids_.empty()) params["voiceprint_ids"] = voiceprint_ids_;
    if (!speaker_roles_.empty()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& role : speaker_roles_) {
        nlohmann::json obj;
        obj["role_name"] = role.role_name;
        obj["audio_url"] = role.audio_url;
        arr.push_back(std::move(obj));
      }
      params["speaker_roles"] = std::move(arr);
    }
  }
  // Speaker context ("断点续传") is only sent when the caller opted in; the
  // mode/diarization combination is validated locally.
  if (enable_speaker_context_ != 0) {
    params["enable_speaker_context"] = enable_speaker_context_;
    if (!speaker_context_id_.empty()) params["speaker_context_id"] = speaker_context_id_;
  }
  if (context_.has_value()) {
    nlohmann::json c;
    if (!context_->text.empty()) c["text"] = context_->text;
    if (!context_->terms.empty()) c["terms"] = context_->terms;
    if (!context_->general.empty()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& kv : context_->general) {
        nlohmann::json obj;
        obj["key"] = kv.key;
        obj["value"] = kv.value;
        arr.push_back(std::move(obj));
      }
      c["general"] = std::move(arr);
    }
    if (!c.empty()) params["context"] = std::move(c);
  }

  nlohmann::json auth;
  auth["sdkappid"] = std::to_string(credential_.sdk_app_id());
  auth["usersig"] = user_sig;
  nlohmann::json frame;
  frame["type"] = "start";
  frame["auth"] = std::move(auth);
  frame["params"] = std::move(params);
  const std::string start_frame = frame.dump();
  if (start_frame.size() > kStartFrameMaxBytes) {
    throw ASRError(kErrInvalidParam, "start frame exceeds " +
                                         std::to_string(kStartFrameMaxBytes) + " bytes");
  }

  // The URL carries only voice_id; auth and params travel in the start
  // frame, so the handshake stays header-free.
  const std::string base = ResolveWSEndpoint(endpoint_, credential_.site());
  const std::string ws_url = base + "/asr/v3?voice_id=" + voice_id_;

  std::string err;
  auto conn = v2internal::WsClient::Connect(ws_url, kHandshakeTimeoutMs, &err);
  if (!conn) {
    throw ASRError(kErrConnectFailed, "websocket dial failed: " + err);
  }
  conn->SetWriteTimeoutMs(static_cast<int>(write_timeout_.count()));

  // Send the start frame immediately (the server enforces a 3s deadline).
  if (!conn->SendText(start_frame, &err)) {
    conn->Close();
    throw ASRError(kErrWriteFailed, "send start frame failed: " + err);
  }

  {
    std::lock_guard<std::mutex> lock(shared_->conn_mu);
    shared_->conn = std::move(conn);
  }

  // Wait for the ack. A failure arrives as a structured error frame
  // ({code,message,voice_id}) followed by a normal close.
  const auto deadline = std::chrono::steady_clock::now() + AckTimeout();
  while (true) {
    v2internal::WsFrame f;
    std::string read_err;
    const int rc = shared_->conn->Read(&f, kReadPollMs, &read_err);
    if (rc == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        shared_->Close();
        throw ASRError(kErrReadFailed, "read start ack failed: ack timeout");
      }
      continue;
    }
    if (rc < 0) {
      shared_->Close();
      throw ASRError(kErrReadFailed,
                     "read start ack failed" + (read_err.empty() ? "" : ": " + read_err));
    }
    if (f.opcode == 0x8) {
      shared_->Close();
      throw ASRError(kErrReadFailed, "read start ack failed: connection closed by server");
    }
    if (f.opcode != 0x1) continue;  // ping/pong handled internally

    nlohmann::json ack;
    try {
      ack = nlohmann::json::parse(f.payload);
    } catch (const std::exception& e) {
      shared_->Close();
      throw ASRError(kErrServerError, "invalid start ack: " + std::string(e.what()));
    }
    const int64_t code = ack.value("code", 0);
    if (code != 0) {
      shared_->Close();
      throw ASRError(static_cast<int>(code), ack.value("message", ""));
    }
    // speaker_continue is present only when the session enabled the speaker
    // context; it carries the id to persist for a later resume.
    if (ack.contains("speaker_continue") && ack["speaker_continue"].is_object()) {
      const auto& sc = ack["speaker_continue"];
      SpeakerContinue parsed;
      parsed.continue_status = sc.value("continue_status", "");
      parsed.speaker_context_id = sc.value("speaker_context_id", "");
      speaker_continue_ = std::move(parsed);
    }
    // The ack frame never carries a result per the protocol; a defensive
    // ack-with-result is consumed here.
    return;
  }
}

std::chrono::milliseconds SpeechRecognizer::AckTimeout() const {
  if (enable_speaker_context_ == kSpeakerContextSync && !speaker_context_id_.empty()) {
    return kSpeakerContextAckTimeout;
  }
  return kAckTimeout;
}

void SpeechRecognizer::SetSpeakerContextId(std::string id) {
  // Trim like the server does, so a stray newline from a persisted id does
  // not turn the resume into a silent "new session".
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!id.empty() && is_space(static_cast<unsigned char>(id.front()))) id.erase(id.begin());
  while (!id.empty() && is_space(static_cast<unsigned char>(id.back()))) id.pop_back();
  speaker_context_id_ = std::move(id);
}

void SpeechRecognizer::Start() {
  int expected = S::kStateIdle_;
  if (!shared_->state.compare_exchange_strong(expected, S::kStateStarting_)) {
    throw ASRError(kErrAlreadyStarted, "recognizer already started");
  }

  try {
    // Validate before dialing so an invalid option fails locally instead of
    // costing a connection and coming back as a server-side 4001.
    ValidateOptions();
    Connect();
  } catch (...) {
    shared_->state.store(S::kStateIdle_);
    throw;
  }

  shared_->state.store(S::kStateRunning_);
  shared_->stop_timeout = stop_timeout_;
  reader_thread_ = std::thread([this] { ReadLoop(); });
}

void SpeechRecognizer::Write(const uint8_t* data, size_t len) {
  if (data == nullptr && len > 0) {
    throw ASRError(kErrInvalidParam, "audio data pointer is null");
  }
  if (len > kStreamFrameMaxBytes) {
    throw ASRError(kErrInvalidParam,
                   "audio frame exceeds " + std::to_string(kStreamFrameMaxBytes) + " bytes");
  }
  if (shared_->state.load() != S::kStateRunning_) {
    throw ASRError(kErrNotStarted, "recognizer not running");
  }

  std::shared_ptr<v2internal::WsClient> conn;
  {
    std::lock_guard<std::mutex> lock(shared_->conn_mu);
    conn = shared_->conn;
  }
  if (!conn) {
    throw ASRError(kErrNotStarted, "connection not established");
  }

  std::lock_guard<std::mutex> write_lock(shared_->write_mu);
  if (shared_->state.load() != S::kStateRunning_) {
    throw ASRError(kErrNotStarted, "recognizer not running");
  }

  std::string err;
  if (!conn->SendBinary(data, len, &err)) {
    throw ASRError(kErrWriteFailed, "write audio data failed: " + err);
  }
}

void SpeechRecognizer::Stop() {
  int expected = S::kStateRunning_;
  if (!shared_->state.compare_exchange_strong(expected, S::kStateStopping_)) {
    throw ASRError(kErrNotStarted, "recognizer not running");
  }

  std::shared_ptr<v2internal::WsClient> conn;
  {
    std::lock_guard<std::mutex> lock(shared_->conn_mu);
    conn = shared_->conn;
  }
  if (!conn) {
    shared_->state.store(S::kStateStopped_);
    throw ASRError(kErrNotStarted, "connection not established");
  }

  std::string send_err;
  bool send_failed = false;
  bool already_stopped = false;
  {
    std::lock_guard<std::mutex> write_lock(shared_->write_mu);
    if (shared_->state.load() == S::kStateStopped_) {
      already_stopped = true;
    } else {
      std::string err;
      if (!conn->SendText("{\"type\":\"end\"}", &err)) {
        send_failed = true;
        send_err = err;
      }
    }
  }
  if (already_stopped) {
    shared_->WaitForReadLoopOrClose();
    return;
  }

  if (send_failed) {
    if (shared_->state.load() == S::kStateStopped_) {
      shared_->WaitForReadLoopOrClose();
      return;
    }
    shared_->Close();
    shared_->state.store(S::kStateStopped_);
    throw ASRError(kErrWriteFailed, "send end signal failed: " + send_err);
  }

  if (shared_->CalledFromListenerCallback()) {
    std::thread([shared = shared_] { shared->WaitForReadLoopOrClose(); }).detach();
    return;
  }

  shared_->WaitForReadLoopOrClose();
  shared_->state.store(S::kStateStopped_);
}

void SpeechRecognizer::ReadLoop() {
  auto shared = shared_;
  {
    std::lock_guard<std::mutex> lock(shared->tid_mu);
    shared->reader_tid = std::this_thread::get_id();
  }

  // Structure note: all exit paths live in ReadLoopInner (plain returns);
  // SignalDone below always runs — mirroring the Go readLoop's deferred
  // closeDone. (Early returns inside the loop must NOT skip it, otherwise
  // Stop() blocks on done forever.)
  try {
    ReadLoopInner(shared);
  } catch (const std::exception& e) {
    shared->Finish();
    SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                 std::string("recovered from panic in read loop: ") + e.what()));
  } catch (...) {
    shared->Finish();
    SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                 "recovered from panic in read loop: unknown exception"));
  }

  shared->Finish();
  shared->SignalDone();
}

void SpeechRecognizer::ReadLoopInner(
    std::shared_ptr<v2internal::RecognizerSharedState> shared) {
  SpeechRecognitionResponse start_resp;
  start_resp.code = 0;
  start_resp.message = "success";
  start_resp.voice_id = voice_id_;
  // The ack itself is consumed by Connect; re-attach the speaker context it
  // carried so callback-style callers see the id/status.
  start_resp.speaker_continue = speaker_continue_;
  listener_->OnRecognitionStart(start_resp);

  std::shared_ptr<v2internal::WsClient> conn;
  {
    std::lock_guard<std::mutex> lock(shared->conn_mu);
    conn = shared->conn;
  }
  if (!conn) return;

  while (true) {
    v2internal::WsFrame frame;
    std::string read_err;
    const int rc = conn->Read(&frame, kReadPollMs, &read_err);
    if (rc == 0) {
      if (shared->state.load() == S::kStateStopped_) return;
      continue;
    }
    if (rc < 0) {
      if (shared->state.load() >= S::kStateStopping_) return;
      shared->Finish();
      SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                   "read message failed" +
                                       (read_err.empty() ? "" : ": " + read_err)));
      return;
    }
    if (frame.opcode == 0x2) {
      SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                   "unmarshal response failed: unexpected binary frame"));
      continue;
    }
    if (frame.opcode == 0x8) {
      if (shared->state.load() >= S::kStateStopping_) return;
      shared->Finish();
      SafeOnFail(nullptr, ASRError(kErrReadFailed, "connection closed by server"));
      return;
    }
    if (frame.opcode != 0x1) continue;

    SpeechRecognitionResponse resp;
    try {
      resp = ParseSpeechResponse(frame.payload);
    } catch (const ASRError& e) {
      SafeOnFail(nullptr, e);
      continue;
    }

    if (resp.code != 0) {
      shared->Finish();
      shared->MarkTerminalReceived();
      SafeOnFail(&resp, ASRError(resp.code, resp.message));
      return;
    }
    if (resp.final_flag == 1) {
      shared->Finish();
      shared->MarkTerminalReceived();
      DispatchEvent(resp);
      SafeComplete(resp);
      return;
    }
    if (!resp.has_result) continue;
    DispatchEvent(resp);
  }
}

void SpeechRecognizer::DispatchEvent(const SpeechRecognitionResponse& resp) {
  if (resp.final_flag == 1 && resp.result.slice_type != 2) {
    return;
  }
  switch (resp.result.slice_type) {
    case 0:
      listener_->OnSentenceBegin(resp);
      break;
    case 1:
      listener_->OnRecognitionResultChange(resp);
      break;
    case 2:
      listener_->OnSentenceEnd(resp);
      break;
    default:
      break;
  }
}

void SpeechRecognizer::SafeOnFail(const SpeechRecognitionResponse* resp, const ASRError& err) {
  try {
    listener_->OnFail(resp, err);
  } catch (...) {
  }
}

void SpeechRecognizer::SafeComplete(const SpeechRecognitionResponse& resp) {
  try {
    listener_->OnRecognitionComplete(resp);
  } catch (...) {
  }
}

}  // namespace v3
}  // namespace trtc_asr
