#include "trtc_asr/signature_params.h"

#include <chrono>
#include <cstdio>
#include <random>

#include "json_helper.h"
#include "sdkinfo.h"

namespace trtc_asr {

SignatureParams::SignatureParams(int64_t app_id, std::string engine_model_type,
                                 std::string voice_id)
    : app_id(app_id),
      engine_model_type(std::move(engine_model_type)),
      voice_id(std::move(voice_id)) {
  auto now = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
  timestamp = now;
  expired = now + 86400;
  static thread_local std::mt19937 rng(std::random_device{}());
  nonce = static_cast<int>(rng() % 9999999) + 1;
}

std::string SignatureParams::BuildQueryString() const {
  std::map<std::string, std::string> params = ToMap();
  std::string out;
  for (const auto& [k, v] : params) {
    if (!out.empty()) out += '&';
    out += k;
    out += '=';
    out += QueryEscape(v);
  }
  return out;
}

std::string SignatureParams::BuildQueryStringWithSignature(
    const std::string& user_sig) const {
  std::map<std::string, std::string> params = ToMap();
  params["signature"] = user_sig;
  params["usersig"] = user_sig;
  std::string out;
  for (const auto& [k, v] : params) {
    if (!out.empty()) out += '&';
    out += k;
    out += '=';
    out += QueryEscape(v);
  }
  return out;
}

std::map<std::string, std::string> SignatureParams::ToMap() const {
  // std::map iterates keys in sorted order, matching Go's sort.Strings.
  std::map<std::string, std::string> m;
  // "secretid" is required by protocol; internally use AppID as its value.
  m["secretid"] = std::to_string(app_id);
  m["timestamp"] = std::to_string(timestamp);
  m["expired"] = std::to_string(expired);
  m["nonce"] = std::to_string(nonce);
  m["engine_model_type"] = engine_model_type;
  m["voice_id"] = voice_id;
  m["voice_format"] = std::to_string(voice_format);
  m["needvad"] = std::to_string(need_vad);

  // SDK self-identification for server-side diagnostics. Not part of the
  // signature (the signature is the UserSig), so it is safe to append.
  for (const auto& [k, v] : internal::SdkReportParams()) {
    m[k] = v;
  }

  if (sdk_app_id > 0) m["sdkappid"] = std::to_string(sdk_app_id);
  if (!hotword_id.empty()) m["hotword_id"] = hotword_id;
  if (!hotword_list.empty()) m["hotword_list"] = hotword_list;
  if (!customization_id.empty()) m["customization_id"] = customization_id;
  if (!replace_text_id.empty()) m["replace_text_id"] = replace_text_id;
  if (filter_dirty != 0) m["filter_dirty"] = std::to_string(filter_dirty);
  if (filter_modal != 0) m["filter_modal"] = std::to_string(filter_modal);
  if (filter_punc != 0) m["filter_punc"] = std::to_string(filter_punc);
  if (filter_empty_result.has_value()) {
    m["filter_empty_result"] = std::to_string(*filter_empty_result);
  }
  if (convert_num_mode != 0) m["convert_num_mode"] = std::to_string(convert_num_mode);
  if (word_info != 0) m["word_info"] = std::to_string(word_info);
  if (vad_silence_time != 0) m["vad_silence_time"] = std::to_string(vad_silence_time);
  if (max_speak_time != 0) m["max_speak_time"] = std::to_string(max_speak_time);
  if (input_sample_rate != 0) m["input_sample_rate"] = std::to_string(input_sample_rate);
  // vad_level / noise_threshold are tri-state: an explicit 0 differs from
  // "not configured" (the server defaults vad_level to 1), so they are only
  // emitted when the caller set them.
  if (vad_level.has_value()) m["vad_level"] = std::to_string(*vad_level);
  if (noise_threshold.has_value()) {
    // Matches Go strconv.FormatFloat(v, 'f', 3, 64): "0.000", "1.500".
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", *noise_threshold);
    m["noise_threshold"] = buf;
  }
  if (enable_speaker_context != 0) {
    m["enable_speaker_context"] = std::to_string(enable_speaker_context);
    if (!speaker_context_id.empty()) {
      m["speaker_context_id"] = speaker_context_id;
    }
  }
  if (speaker_diarization != 0) {
    m["speaker_diarization"] = std::to_string(speaker_diarization);
    if (speaker_number != 0) m["speaker_number"] = std::to_string(speaker_number);
  }
  // speaker_roles / voiceprintids only apply to voiceprint mode.
  if (speaker_diarization == kSpeakerDiarizationVoiceprint) {
    if (!speaker_roles.empty()) {
      m["speaker_roles"] = internal::SerializeSpeakerRoles(speaker_roles);
    }
    if (!voiceprint_ids.empty()) {
      m["voiceprintids"] = internal::SerializeStringArray(voiceprint_ids);
    }
  }
  if (!language.empty()) m["language"] = language;
  return m;
}

std::string QueryEscape(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0xF];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

}  // namespace trtc_asr
