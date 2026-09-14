#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "trtc_asr/credential.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/signature_params.h"

namespace trtc_asr {

/// Task is queued.
inline constexpr int kTaskStatusWaiting = 0;
/// Task is being processed.
inline constexpr int kTaskStatusRunning = 1;
/// Task completed successfully.
inline constexpr int kTaskStatusSuccess = 2;
/// Task failed.
inline constexpr int kTaskStatusFailed = 3;

/// Word-level timing within a sentence.
///
/// Offsets are milliseconds relative to the start of the audio. On the wire
/// the server spells them StartTime / EndTime; the older OffsetStartMs /
/// OffsetEndMs spelling is accepted as a fallback.
struct SentenceWords {
  std::string word;
  int64_t offset_start_ms = 0;
  int64_t offset_end_ms = 0;
};

/// Sentence-level recognition result with word timing.
struct SentenceDetail {
  std::string final_sentence;
  std::string slice_sentence;
  std::string written_text;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  int words_num = 0;
  std::vector<SentenceWords> words;
  double speech_speed = 0;
  int64_t silence_time = 0;
  /// Speaker number of this sentence, when diarization is enabled.
  int speaker_id = 0;
  /// Enrolled role name (mode 3); empty when no enrolled speaker matched.
  std::string speaker_role_name;
  /// Audio channel for stereo recordings (ChannelNum=2): 1=left, 2=right.
  int channel_id = 0;
  /// Detected language of this sentence, when the engine reports one.
  std::string language;
};

/// Full task status and result returned by DescribeTaskStatus.
struct TaskStatus {
  std::string rec_task_id;
  /// 0 waiting, 1 executing, 2 success, 3 failed.
  int status = 0;
  std::string status_str;
  /// Progress 0-100.
  int progress = 0;
  std::string result;
  std::string error_msg;
  std::vector<SentenceDetail> result_detail;
  /// Audio duration in seconds.
  double audio_duration = 0;
};

/// JSON request body for creating a file recognition task.
struct CreateRecTaskRequest {
  /// Engine model type. Required. E.g. "16k_zh", "16k_zh_en".
  std::string engine_model_type;
  /// Audio channels. Required. 1: mono; 2: stereo (8k telephony, server
  /// separates speakers by channel and returns channel_id).
  int channel_num = 0;
  /// Result format: 0 basic, 1 word-level timing, 2 +punctuation timing.
  int res_text_format = 0;
  /// Audio source: 0 = URL, 1 = base64 data in body.
  int source_type = 0;
  /// Audio file URL (required when source_type = 0). ≤ 12h, ≤ 1GB.
  std::string url;
  /// Base64-encoded audio data (required when source_type = 1). ≤ 5MB.
  std::string data;
  /// Audio data length in bytes (required when source_type = 1).
  int64_t data_len = 0;
  /// Callback URL; results are POSTed there when the task completes.
  std::string callback_url;
  /// Profanity filter: 0 off (default), 1 filter, 2 replace with *.
  int filter_dirty = 0;
  /// Modal particle filter: 0 off (default), 1 partial, 2 strict.
  int filter_modal = 0;
  /// Punctuation filter: 0 off (default), 1 trailing, 2 all.
  int filter_punc = 0;
  /// Arabic numeral conversion: 0 off, 1 smart (default).
  int convert_num_mode = 0;
  /// Hotword vocabulary ID from the console.
  std::string hotword_id;
  /// Temporary inline hotword list: "word1|weight1,word2|weight2".
  std::string hotword_list;
  /// Custom language model ID.
  std::string customization_id;
  /// Replacement word table ID for forced text replacement.
  std::string replace_text_id;
  /// Forces the audio language on supporting engines. Empty = auto.
  std::string language;
  /// Speaker diarization: 0 off (default), 1 anonymous clustering,
  /// 3 voiceprint role authentication. For stereo (channel_num=2) do NOT
  /// enable this: the server fills channel_id per sentence instead.
  int speaker_diarization = 0;
  /// Expected speaker count hint. 0 = auto (default).
  int speaker_number = 0;
  /// Temporary voiceprints; only used when speaker_diarization is 3.
  std::vector<SpeakerRole> speaker_roles;
  /// Previously enrolled voiceprint IDs; only when mode is 3.
  std::vector<std::string> voiceprint_ids;
  /// Silence detection threshold in milliseconds.
  int vad_silence_ms = 0;
  /// VAD profile: 0 = high recall (default), 1 = far-field filtering.
  /// optional so an explicit 0 is distinguishable from "not configured".
  std::optional<int> vad_level;
  /// VAD noise fine-tuning, range [0, 4]. Overrides vad_level when set.
  std::optional<double> noise_threshold;
};

/// Async audio file recognition client (HTTP). For long audio (≤ 12h):
/// submit a task (CreateRecTask), then poll for results (DescribeTaskStatus).
class FileRecognizer {
 public:
  /// Production HTTPS endpoint for audio file recognition.
  static constexpr const char* kEndpoint = "https://asr.cloud-rtc.com";

  /// Max audio size for data upload (before base64 encoding).
  static constexpr size_t kMaxAudioSize = 5 * 1024 * 1024;

  explicit FileRecognizer(const Credential& credential);

  /// Overrides the default API endpoint (for testing).
  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }

  /// Submits an audio file recognition task and returns the task ID.
  /// Throws ASRError.
  std::string CreateTask(const CreateRecTaskRequest& req);

  /// Convenience method that submits local audio data (≤ 5MB), handling
  /// base64 encoding automatically.
  std::string CreateTaskFromData(const std::vector<uint8_t>& data,
                                 const std::string& voice_format,
                                 const std::string& engine_model_type);

  /// Convenience method that submits an audio URL (≤ 12h / ≤ 1GB).
  std::string CreateTaskFromURL(const std::string& audio_url,
                                const std::string& engine_model_type);

  /// Submits local audio data with a pre-configured request, handling base64
  /// encoding automatically. data/data_len/source_type are set from raw_data.
  std::string CreateTaskFromDataWithOptions(const std::vector<uint8_t>& raw_data,
                                            CreateRecTaskRequest* req);

  /// Queries the status of a file recognition task. Throws ASRError.
  TaskStatus DescribeTaskStatus(const std::string& rec_task_id);

  /// Polls for the result until the task completes or fails. Default poll
  /// interval is 1s, max wait is 10 minutes. Throws ASRError.
  TaskStatus WaitForResult(const std::string& rec_task_id);

  /// Polls for the result with a custom interval and timeout.
  TaskStatus WaitForResultWithInterval(const std::string& rec_task_id,
                                       std::chrono::milliseconds interval,
                                       std::chrono::milliseconds timeout);

 private:
  /// Sends an HTTP POST to the given API path with a JSON body.
  std::string DoRequest(const std::string& path, const std::string& json_body);

  Credential credential_;
  std::string endpoint_;
  int timeout_seconds_ = 60;
};

}  // namespace trtc_asr
