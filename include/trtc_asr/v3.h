#pragma once

// TRTC-ASR v3 protocol client.
//
// The v3 protocol restructures the wire format around two separated blocks:
// an "auth" block (sdkappid / usersig / request_id) consumed by the gateway's
// authentication layer, and a "params" block (engine, VAD, hotwords, filters,
// ...) consumed by the recognition layer. All field names are snake_case and
// responses are flat (no Response envelope) with numeric codes.
//
// v3 uses SdkAppID as the only customer dimension; the Tencent Cloud AppID is
// not needed (see NewCredential).
//
// Interfaces:
// - SpeechRecognizer: WebSocket /asr/v3 — the URL carries only voice_id; auth
//   and params travel in a single start frame sent right after the handshake.
//   Start() waits synchronously for the server ack.
// - SentenceRecognizer: POST /v3/transcribe — one-shot (<=60s).
// - FileRecognizer: /v3/create_transcription + /v3/describe_transcription.
//
// The downlink frame shape is identical to v2, so the streaming client reuses
// trtc_asr::SpeechRecognitionResponse and SpeechRecognitionListener.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "trtc_asr/credential.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/speech_recognizer.h"  // listener + response + shared state

// Re-export the shared internal namespace so v3 members can refer to it as
// trtc_asr::internal without ambiguity from namespace v3.
#include "trtc_asr/types.h"

namespace trtc_asr {
namespace v3 {

/// SourceType for the HTTP interfaces.
constexpr int kSourceTypeUrl = 0;
/// SourceType for the HTTP interfaces.
constexpr int kSourceTypeData = 1;

/// Task status values returned by describe_transcription.
constexpr int kTaskStatusWaiting = 0;
constexpr int kTaskStatusRunning = 1;
constexpr int kTaskStatusSuccess = 2;
constexpr int kTaskStatusFailed = 3;

/// Speaker diarization modes.
constexpr int kSpeakerDiarizationOff = 0;
constexpr int kSpeakerDiarizationCluster = 1;
constexpr int kSpeakerDiarizationVoiceprint = 3;

/// Wire size limit of the online start frame, mirroring the server.
constexpr size_t kStartFrameMaxBytes = 64 * 1024;
/// Wire size limit of one audio frame, mirroring the server.
constexpr size_t kStreamFrameMaxBytes = 256 * 1024;

/// How long Start() waits for the server's start-frame acknowledgement.
constexpr std::chrono::milliseconds kAckTimeout{5000};

/// Creates a credential for the v3 API: only SdkAppID + SecretKey are needed.
Credential NewCredential(int64_t sdk_app_id, std::string secret_key);

/// A temporary voiceprint enrollment entry used with
/// speaker_diarization=3. Serialized with the v3 snake_case wire names
/// (role_name / audio_url), unlike the v2 CamelCase SpeakerRole.
class SpeakerRole {
 public:
  std::string role_name;
  std::string audio_url;
};

/// A domain key-value pair of Context::general.
struct ContextKV {
  std::string key;
  std::string value;
};

/// Recognition context, aligned with Soniox / Volcengine semantics. LLM-class
/// engines can consume all parts; traditional engines degrade terms to
/// hotwords and ignore text/general.
class Context {
 public:
  std::string text;
  std::vector<std::string> terms;
  std::vector<ContextKV> general;
};

/// One audio piece of a distributed recording task.
class AudioURLItem {
 public:
  int64_t index = 0;
  std::string url;
  std::string label;
};

/// The params block of POST /v3/transcribe (snake_case wire names).
class TranscribeRequest {
 public:
  std::string engine_model_type;
  int source_type = kSourceTypeData;
  std::string voice_format = "pcm";
  std::string url;
  std::string data;   // base64
  int64_t data_len = 0;
  int word_info = 0;
  int filter_dirty = 0;
  int filter_modal = 0;
  int filter_punc = 0;
  int convert_num_mode = 0;
  std::string hotword_id;
  std::string customization_id;
  std::string hotword_list;
  int input_sample_rate = 0;
  /// None leaves the server default; an explicit 0/1 is honored.
  std::optional<int> needvad;
  std::optional<int> vad_silence_time;
  std::string language;
  int speaker_diarization = 0;
  int speaker_number = 0;
  std::optional<Context> context;

  /// Validates the required/conditional fields and local ranges.
  void Validate() const;
};

/// A word-level timing entry (transcribe / describe responses).
class Word {
 public:
  std::string word;
  int64_t start_time = 0;
  int64_t end_time = 0;
};

/// The flat /v3/transcribe response.
class TranscribeResponse {
 public:
  int64_t code = 0;
  std::string message;
  std::string request_id;
  std::string result;
  int64_t audio_duration = 0;
  std::string language;
  std::string language_b47;
  int word_size = 0;
  std::vector<Word> word_list;
};

/// The params block of POST /v3/create_transcription (snake_case wire names).
class CreateTranscriptionRequest {
 public:
  std::string engine_model_type;
  int channel_num = 1;
  int res_text_format = 1;
  int source_type = kSourceTypeData;
  std::string url;
  std::string data;  // base64
  int64_t data_len = 0;
  /// Distributed recording pieces. When non-empty, source_type/url/data must
  /// be left at their zero values — the server rejects any combination of
  /// audio_urls with a single-audio source.
  std::vector<AudioURLItem> audio_urls;
  std::string callback_url;
  int speaker_diarization = 0;
  int speaker_number = 0;
  std::vector<std::string> voiceprint_ids;
  std::vector<SpeakerRole> speaker_roles;
  std::string hotword_id;
  std::string customization_id;
  std::string hotword_list;
  std::vector<std::string> keyword_lib_id_list;
  std::string replace_text_id;
  int convert_num_mode = 0;
  int filter_dirty = 0;
  int filter_punc = 0;
  int filter_modal = 0;
  int sentence_max_length = 0;
  std::string extra;
  int vad_silence_ms = 0;
  std::optional<int> vad_level;
  std::optional<double> noise_threshold;
  std::string language;
  std::optional<Context> context;

  void Validate() const;
};

/// One sentence of a describe_transcription result.
class SentenceDetail {
 public:
  std::string final_sentence;
  std::string slice_sentence;
  std::string written_text;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  int words_num = 0;
  std::vector<Word> words;
  double speech_speed = 0.0;
  int speaker_id = 0;
  int channel_id = 0;
  std::string speaker_role_name;
  int silence_time = 0;
  std::string language;
  std::string language_b47;
};

/// The flat /v3/describe_transcription response.
class TranscriptionStatus {
 public:
  int64_t code = 0;
  std::string message;
  std::string request_id;
  std::string transcription_id;
  int status = 0;
  std::string status_str;
  int progress = 0;
  double audio_duration = 0.0;
  std::string result;
  std::vector<SentenceDetail> result_detail;
  std::string error_msg;
};

/// Shared local checks mirroring the server-side validators.
namespace internal {
/// Checks the distributed-recording invariants (rectask_cluster.go
/// validateDistributedAudioUrlsRequest).
void ValidateAudioURLs(int source_type, const std::string& url,
                       const std::string& data,
                       const std::vector<AudioURLItem>& audio_urls);
/// Checks the diarization mode and its enrollment input (v3 snake_case
/// SpeakerRole).
void ValidateSpeakerDiarization(int mode, int speaker_number,
                                const std::vector<SpeakerRole>& roles,
                                const std::vector<std::string>& voiceprint_ids);
void ValidateEnumOption(const std::string& name, int value,
                        const std::vector<int>& allowed);
void ValidateVadTuning(const std::optional<int>& vad_level,
                       const std::optional<double>& noise_threshold);
}  // namespace internal

/// The main client for realtime speech recognition over the v3 protocol
/// (WebSocket /asr/v3).
///
/// The lifecycle machinery mirrors the v2 client (single-use instances,
/// reader thread, re-entrant Stop). The v3 difference: Start() waits
/// synchronously for the server's start-frame ack, so authentication (4002),
/// gray-switch (4001) and scheduling (5000) errors are thrown directly from
/// Start(). The downlink frame shape is identical to v2, so the v2 listener
/// and response types are reused.
class SpeechRecognizer {
 public:
  /// Production WebSocket endpoint for the TRTC-ASR service (v3).
  static constexpr const char* kEndpoint = "wss://asr.cloud-rtc.com";

  SpeechRecognizer(const Credential& credential, std::string engine_model_type,
                   SpeechRecognitionListener* listener);
  ~SpeechRecognizer();
  SpeechRecognizer(const SpeechRecognizer&) = delete;
  SpeechRecognizer& operator=(const SpeechRecognizer&) = delete;

  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }
  /// Sets the audio encoding format. 1: PCM (default).
  void SetVoiceFormat(int format) { voice_format_ = format; }
  /// Unlike the v2 transport, v3 honors an explicit 0 (sent on the wire).
  void SetNeedVad(int need_vad) { need_vad_ = need_vad; }
  /// Unlike the v2 transport, v3 honors an explicit 0.
  void SetConvertNumMode(int mode) { convert_num_mode_ = mode; }
  void SetHotwordId(std::string id) { hotword_id_ = std::move(id); }
  void SetHotwordList(std::string list) { hotword_list_ = std::move(list); }
  void SetFilterDirty(int mode) { filter_dirty_ = mode; }
  void SetFilterModal(int mode) { filter_modal_ = mode; }
  void SetFilterPunc(int mode) { filter_punc_ = mode; }
  void SetFilterEmptyResult(int mode) { filter_empty_result_ = mode; }
  /// 0 no (default), 1 yes, 2 with punctuation, 100 caption.
  void SetWordInfo(int mode) { word_info_ = mode; }
  /// Whether English words are joined with spaces: 0 no (default), 1 yes.
  void SetWordWithSpace(int mode) { word_with_space_ = mode; }
  void SetVadSilenceTime(int ms) { vad_silence_time_ = ms; }
  void SetVadLevel(int level) { vad_level_ = level; }
  void SetNoiseThreshold(double threshold) { noise_threshold_ = threshold; }
  void SetMaxSpeakTime(int ms) { max_speak_time_ = ms; }
  void SetInputSampleRate(int rate) { input_sample_rate_ = rate; }
  void SetSpeakerDiarization(int mode) { speaker_diarization_ = mode; }
  void SetSpeakerNumber(int n) { speaker_number_ = n; }
  void SetSpeakerRoles(std::vector<SpeakerRole> roles) {
    speaker_roles_ = std::move(roles);
  }
  void SetVoiceprintIds(std::vector<std::string> ids) {
    voiceprint_ids_ = std::move(ids);
  }
  void SetVoiceId(std::string id) { voice_id_ = std::move(id); }
  void SetLanguage(std::string lang) { language_ = std::move(lang); }
  void SetContext(Context context) { context_ = std::move(context); }
  void SetWriteTimeout(std::chrono::milliseconds timeout);
  void SetStopTimeout(std::chrono::milliseconds timeout);

  /// Connects to /asr/v3, sends the start frame and waits for the server
  /// ack. Auth (4002), gray-switch (4001) and scheduling (5000) errors are
  /// thrown here synchronously.
  void Start();
  /// Sends one audio frame (binary, <=256KB).
  void Write(const uint8_t* data, size_t len);
  void Write(const std::vector<uint8_t>& data) { Write(data.data(), data.size()); }
  /// Gracefully stops: sends the end signal and waits for the final response.
  void Stop();

 private:
  void ValidateOptions() const;
  void Connect();
  void ReadLoop();
  void ReadLoopInner(
      std::shared_ptr<::trtc_asr::internal::RecognizerSharedState> shared);
  void DispatchEvent(const SpeechRecognitionResponse& resp);
  void SafeOnFail(const SpeechRecognitionResponse* resp, const ASRError& err);
  void SafeComplete(const SpeechRecognitionResponse& resp);

  Credential credential_;
  SpeechRecognitionListener* listener_;
  // Reuses the v2 recognizer's shared lifecycle state (identical downlink
  // shape and concurrency model).
  std::shared_ptr<::trtc_asr::internal::RecognizerSharedState> shared_ =
      std::make_shared<::trtc_asr::internal::RecognizerSharedState>();
  std::thread reader_thread_;

  std::string endpoint_;
  std::string engine_model_type_;
  int voice_format_ = 1;
  int need_vad_ = 1;
  int convert_num_mode_ = 1;
  std::string hotword_id_;
  std::string hotword_list_;
  int filter_dirty_ = 0;
  int filter_modal_ = 0;
  int filter_punc_ = 0;
  std::optional<int> filter_empty_result_;
  int word_info_ = 0;
  int word_with_space_ = 0;
  int vad_silence_time_ = 0;
  std::optional<int> vad_level_;
  std::optional<double> noise_threshold_;
  int max_speak_time_ = 0;
  int input_sample_rate_ = 0;
  int speaker_diarization_ = 0;
  int speaker_number_ = 0;
  std::vector<SpeakerRole> speaker_roles_;
  std::vector<std::string> voiceprint_ids_;
  std::string voice_id_;
  std::string language_;
  std::optional<Context> context_;

  std::chrono::milliseconds write_timeout_{5000};
  std::chrono::milliseconds stop_timeout_{10000};
};

/// One-shot sentence recognition client (POST /v3/transcribe).
class SentenceRecognizer {
 public:
  /// Production HTTPS endpoint for the v3 offline interfaces.
  static constexpr const char* kEndpoint = "https://asr.cloud-rtc.com";

  explicit SentenceRecognizer(Credential credential);
  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }
  void SetTimeout(std::chrono::seconds timeout) { timeout_ = timeout; }

  TranscribeResponse Recognize(const TranscribeRequest& req);
  /// Recognizes local audio data with a pre-configured request: data,
  /// data_len and source_type are filled in from `data`, everything else
  /// (engine, language, word_info …) comes from `req`.
  TranscribeResponse RecognizeDataWithOptions(const std::vector<uint8_t>& data,
                                              TranscribeRequest* req);
  /// Recognizes local audio data (raw bytes; base64 is applied internally).
  TranscribeResponse RecognizeData(const std::vector<uint8_t>& data,
                                   const std::string& voice_format,
                                   const std::string& engine_model_type);
  TranscribeResponse RecognizeUrl(const std::string& audio_url,
                                  const std::string& voice_format,
                                  const std::string& engine_model_type);

 private:
  Credential credential_;
  std::string endpoint_;
  std::chrono::seconds timeout_{30};
};

/// Async audio file recognition client. The task ID (transcription_id) is
/// valid for 24 hours and belongs to the v3 task space: a v1 RecTaskId
/// cannot be queried through this client and vice versa.
class FileRecognizer {
 public:
  /// Production HTTPS endpoint for the v3 offline interfaces.
  static constexpr const char* kEndpoint = "https://asr.cloud-rtc.com";

  explicit FileRecognizer(Credential credential);
  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }
  void SetTimeout(std::chrono::seconds timeout) { timeout_ = timeout; }

  std::string CreateTask(const CreateTranscriptionRequest& req);
  /// Submits local audio data with a pre-configured request: data, data_len
  /// and source_type are filled in from `data`.
  std::string CreateTaskFromDataWithOptions(const std::vector<uint8_t>& data,
                                            CreateTranscriptionRequest* req);
  /// Submits local audio data (raw bytes; base64 is applied internally).
  std::string CreateTaskFromData(const std::vector<uint8_t>& data,
                                 const std::string& engine_model_type);
  std::string CreateTaskFromUrl(const std::string& audio_url,
                                const std::string& engine_model_type);
  TranscriptionStatus DescribeTask(const std::string& transcription_id);
  TranscriptionStatus WaitForResult(const std::string& transcription_id);
  TranscriptionStatus WaitForResultWithInterval(const std::string& transcription_id,
                                                std::chrono::milliseconds interval,
                                                std::chrono::milliseconds timeout);

 private:
  Credential credential_;
  std::string endpoint_;
  std::chrono::seconds timeout_{60};
};

}  // namespace v3
}  // namespace trtc_asr
