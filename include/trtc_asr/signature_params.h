#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace trtc_asr {

/// Speaker diarization modes for the speaker_diarization parameter.
inline constexpr int kSpeakerDiarizationOff = 0;
/// Anonymous clustering: speakers numbered from 1 in the session, -1 unknown.
inline constexpr int kSpeakerDiarizationCluster = 1;
/// Voiceprint role authentication; combine with SpeakerRoles / voiceprint IDs.
inline constexpr int kSpeakerDiarizationVoiceprint = 3;

/// 说话人分离断点续传（speaker context）模式：enable_speaker_context 参数。
inline constexpr int kSpeakerContextOff = 0;
inline constexpr int kSpeakerContextSync = 1;
inline constexpr int kSpeakerContextAsync = 2;

/// A temporary voiceprint enrollment entry used with speaker_diarization=3.
/// role_name is echoed back by the server as speaker_name on matched words /
/// speaker segments.
///
/// Serialized as {"RoleName":..,"AudioUrl":..} to match the server contract.
struct SpeakerRole {
  std::string role_name;
  std::string audio_url;
};

/// URL query parameters for the ASR WebSocket request.
///
/// The "secretid" URL parameter is required by the protocol but internally
/// populated with the APPID — users do not provide a separate SecretID. The
/// "signature" parameter is set to the UserSig value per protocol spec, and
/// the same value is also sent as "usersig" so the gateway can authenticate
/// clients (e.g. browsers) that cannot attach custom WebSocket headers.
struct SignatureParams {
  int64_t app_id = 0;
  int64_t timestamp = 0;
  int64_t expired = 0;
  int nonce = 0;
  std::string engine_model_type;
  std::string voice_id;
  int voice_format = 1;  // pcm
  int need_vad = 1;

  /// TRTC application ID, sent as the "sdkappid" query parameter. 0 = unset.
  int64_t sdk_app_id = 0;

  // Optional parameters (empty/0 means not configured).
  std::string hotword_id;
  /// Temporary inline hotwords: "word|weight,word|weight".
  std::string hotword_list;
  std::string customization_id;
  std::string replace_text_id;
  int filter_dirty = 0;
  int filter_modal = 0;
  int filter_punc = 0;
  int convert_num_mode = 1;
  int word_info = 0;
  int vad_silence_time = 0;
  int max_speak_time = 0;
  /// 8000: feed 8kHz PCM to a 16k engine (upsampled server-side).
  int input_sample_rate = 0;
  /// Bigmodel engine language hint (e.g. "zh", "en", "auto").
  std::string language;

  /// Tri-state options: has_value distinguishes an explicit 0 from
  /// "not configured" (the server defaults vad_level to 1 and
  /// filter_empty_result to 1).
  std::optional<int> filter_empty_result;
  std::optional<int> vad_level;
  std::optional<double> noise_threshold;

  /// 0 = off (default), 1 = anonymous clustering, 3 = voiceprint roles.
  int speaker_diarization = 0;

  /// 说话人分离断点续传：0=off（默认），1=同步回报恢复状态，2=异步只回 id。
  /// 需与 speaker_diarization 1/3 同开。
  int enable_speaker_context = 0;

  /// 上次会话签发的上下文 ID；仅与 enable_speaker_context 一起下发。
  std::string speaker_context_id;
  /// Expected speaker count hint; 0 = auto detection (default).
  int speaker_number = 0;
  /// Only sent when speaker_diarization is 3.
  std::vector<SpeakerRole> speaker_roles;
  std::vector<std::string> voiceprint_ids;

  /// Creates parameters with sensible defaults (current time, random nonce).
  SignatureParams(int64_t app_id, std::string engine_model_type, std::string voice_id);

  /// Builds the URL query string with all parameters (without signature).
  std::string BuildQueryString() const;

  /// Builds the URL query string with "signature" and "usersig" set to the
  /// given UserSig value (per protocol both carry the UserSig).
  std::string BuildQueryStringWithSignature(const std::string& user_sig) const;

  /// Collects the parameters into a sorted key/value map (without signature).
  std::map<std::string, std::string> ToMap() const;
};

/// Percent-encodes a query value with Go url.QueryEscape semantics:
/// unreserved [A-Za-z0-9-_.~] stay as-is, space becomes '+', everything else
/// becomes %XX (uppercase hex, per UTF-8 byte).
std::string QueryEscape(const std::string& s);

}  // namespace trtc_asr
