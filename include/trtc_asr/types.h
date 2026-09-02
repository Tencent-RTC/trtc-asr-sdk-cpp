#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trtc_asr {

/// Word-level recognition details (realtime protocol).
struct WordInfo {
  std::string word;
  int64_t start_time = 0;
  int64_t end_time = 0;
  int stable_flag = 0;
  /// Speaker of this word, when diarization + word_info are enabled.
  /// Valid IDs start at 1, -1 unknown, 0 absent.
  int speaker_id = 0;
  /// Enrolled role name, only with speaker_diarization=3.
  std::string speaker_name;
};

/// A contiguous section of one result attributed to a single speaker.
struct SpeakerSegment {
  /// Speaker number in the session. Valid IDs start at 1, -1 unknown,
  /// 0 reserved.
  int speaker_id = 0;
  /// Enrolled role name, only with speaker_diarization=3.
  std::string speaker_name;
  int64_t start_time = 0;
  int64_t end_time = 0;
  std::string text;
  /// Inclusive indexes into word_list. nullopt when word_info=0.
  std::optional<int> word_start;
  std::optional<int> word_end;
  /// 1 = stable, 0 = not.
  int stable_flag = 0;
};

/// Recognition result details carried by a realtime response.
struct RecognitionResult {
  /// 0 = sentence begin, 1 = intermediate result, 2 = sentence-final result.
  int slice_type = 0;
  int index = 0;
  int64_t start_time = 0;
  int64_t end_time = 0;
  std::string voice_text_str;
  int word_size = 0;
  std::vector<WordInfo> word_list;
  /// Detected language when the engine reports one.
  std::string language;

  /// Speaker attribution split by speaker turn — the recommended entry point
  /// for diarization. Empty when diarization is disabled.
  std::vector<SpeakerSegment> speaker_segments;

  /// Legacy sentence-level speaker attribution (0 is reserved; prefer
  /// speaker_segments / WordInfo::speaker_id).
  std::optional<int> speaker_id;

  /// Trailing silence (ms) that triggered the sentence break.
  int64_t finish_silence_ms = 0;
  /// Server-side decoding time (ms) of the last token.
  int64_t last_token_runtime_ms = 0;
};

/// A response message from the ASR service (realtime WebSocket protocol).
struct SpeechRecognitionResponse {
  int code = 0;
  std::string message;
  std::string voice_id;
  std::string message_id;
  /// 1 marks the session-ending frame.
  int final_flag = 0;
  RecognitionResult result;
  /// Whether the raw frame carried a "result" object. The connection ack
  /// frame does not, and must not be dispatched as a sentence begin.
  bool has_result = false;
};

/// Parses a realtime response frame. Throws ASRError (kErrReadFailed) on
/// malformed JSON. Exported for tests.
SpeechRecognitionResponse ParseSpeechResponse(const std::string& json);

}  // namespace trtc_asr
