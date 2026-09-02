#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "trtc_asr/credential.h"
#include "trtc_asr/errors.h"

namespace trtc_asr {

/// Word-level timing information for sentence recognition.
struct SentenceWord {
  std::string word;
  int64_t start_time = 0;
  int64_t end_time = 0;
};

/// Successful sentence recognition result.
struct SentenceRecognitionResult {
  /// Recognition text.
  std::string result;
  /// Audio duration in milliseconds.
  int64_t audio_duration = 0;
  /// Word count (0 when word info is not enabled).
  int word_size = 0;
  /// Word-level timing details (empty when word info is not enabled).
  std::vector<SentenceWord> word_list;
  /// Unique request identifier.
  std::string request_id;
};

/// JSON request body for sentence recognition.
///
/// Field names serialize exactly per the server-side contract (including the
/// quirky EngSerViceType capitalization).
struct SentenceRecognitionRequest {
  /// Engine model type. Required. E.g. "16k_zh", "16k_zh_en".
  std::string eng_service_type;
  /// Audio source: 0 = URL, 1 = base64 data in body.
  int source_type = 0;
  /// Audio format: "wav", "pcm", "ogg-opus", "mp3", "m4a".
  std::string voice_format;
  /// Audio file URL (required when source_type = 0). ≤ 60s, ≤ 3MB.
  std::string url;
  /// Base64-encoded audio data (required when source_type = 1).
  std::string data;
  /// Audio data length in bytes (required when source_type = 1).
  int64_t data_len = 0;
  /// Word-level timing: 0 hide (default), 1 show, 2 with punctuation.
  int word_info = 0;
  /// Profanity filter: 0 off (default), 1 filter, 2 replace with *.
  int filter_dirty = 0;
  /// Modal particle filter: 0 off (default), 1 partial, 2 strict.
  int filter_modal = 0;
  /// Punctuation filter: 0 off (default), 2 filter all punctuation.
  int filter_punc = 0;
  /// Arabic numeral conversion: 0 off, 1 smart (default).
  int convert_num_mode = 0;
  /// Hotword vocabulary ID from the console.
  std::string hotword_id;
  /// Temporary inline hotword list: "word1|weight1,word2|weight2".
  std::string hotword_list;
  /// Custom language model ID.
  std::string customization_id;
  /// PCM input sample rate override. Only 8000 is supported.
  int input_sample_rate = 0;
  /// Forces the audio language on supporting engines. Empty = auto.
  std::string language;
};

/// One-shot sentence recognition client (HTTP). Audio ≤ 60s / ≤ 3MB.
class SentenceRecognizer {
 public:
  /// Production HTTPS endpoint for sentence recognition.
  static constexpr const char* kEndpoint = "https://asr.cloud-rtc.com";

  /// Audio from a URL.
  static constexpr int kSourceTypeURL = 0;
  /// Audio data in the request body (base64 encoded).
  static constexpr int kSourceTypeData = 1;

  /// Max audio size for one-shot recognition (before base64 encoding).
  static constexpr size_t kMaxAudioSize = 3 * 1024 * 1024;

  explicit SentenceRecognizer(const Credential& credential);

  /// Overrides the default API endpoint (for testing).
  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }

  /// Sends a sentence recognition request and returns the result.
  /// Throws ASRError.
  SentenceRecognitionResult Recognize(const SentenceRecognitionRequest& req);

  /// Convenience method that sends local audio data for recognition,
  /// handling base64 encoding automatically.
  SentenceRecognitionResult RecognizeData(const std::vector<uint8_t>& data,
                                          const std::string& voice_format,
                                          const std::string& engine_model_type);

  /// Convenience method that sends an audio URL for recognition.
  SentenceRecognitionResult RecognizeURL(const std::string& audio_url,
                                         const std::string& voice_format,
                                         const std::string& engine_model_type);

  /// Sends local audio data with a pre-configured request, handling base64
  /// encoding automatically. data/data_len are set from raw_data.
  SentenceRecognitionResult RecognizeDataWithOptions(
      const std::vector<uint8_t>& raw_data, SentenceRecognitionRequest* req);

 private:
  Credential credential_;
  std::string endpoint_;
  int timeout_seconds_ = 30;
};

}  // namespace trtc_asr
