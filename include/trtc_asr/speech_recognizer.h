#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "trtc_asr/credential.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/signature_params.h"
#include "trtc_asr/types.h"

namespace trtc_asr {
namespace internal {

class WsClient;

/// Runtime state of a SpeechRecognizer, shared with the reader thread and
/// the stop watchdog via shared_ptr so those threads never dereference a
/// destroyed recognizer. Internal implementation detail.
struct RecognizerSharedState {
  std::atomic<int> state{0};  // idle
  /// Guards conn acquisition; critical sections never contain network I/O.
  std::mutex conn_mu;
  std::shared_ptr<WsClient> conn;
  /// Serializes WebSocket writes (audio frames and the end signal).
  std::mutex write_mu;
  std::condition_variable done_cv;
  std::mutex done_mu;
  bool done = false;
  std::atomic<bool> terminal_received{false};
  std::once_flag finish_once;
  std::thread::id reader_tid;
  mutable std::mutex tid_mu;
  std::chrono::milliseconds stop_timeout{10000};

  /// Advances to the terminal stopped state exactly once and closes the
  /// connection. Invoked before terminal callbacks. (Defined in the .cc
  /// because WsClient is incomplete here.)
  void Finish();
  void Close();
  void WaitForReadLoopOrClose();

  /// Records that a terminal response (final=1 or code!=0) has been
  /// received and wakes any Stop() still inside its timed wait, so it can
  /// switch to the unbounded wait for the terminal callbacks. The flag is
  /// stored under done_mu to keep the condvar protocol free of
  /// lost-wakeup windows.
  void MarkTerminalReceived() {
    {
      std::lock_guard<std::mutex> lock(done_mu);
      terminal_received.store(true);
    }
    done_cv.notify_all();
  }

  void SignalDone() {
    {
      std::lock_guard<std::mutex> lock(done_mu);
      done = true;
    }
    done_cv.notify_all();
  }

  bool CalledFromListenerCallback() const {
    std::lock_guard<std::mutex> lock(tid_mu);
    return reader_tid != std::thread::id() &&
           reader_tid == std::this_thread::get_id();
  }

  static constexpr int kStateIdle_ = 0;
  static constexpr int kStateStarting_ = 1;
  static constexpr int kStateRunning_ = 2;
  static constexpr int kStateStopping_ = 3;
  static constexpr int kStateStopped_ = 4;
};

}  // namespace internal

/// Callback interface for speech recognition events. All methods are virtual
/// no-ops so callers can override only the events they care about.
///
/// Callbacks are delivered sequentially on an SDK-owned reader thread. A
/// callback that throws does not crash the SDK: the session is terminated and
/// the failure is surfaced via OnFail.
class SpeechRecognitionListener {
 public:
  virtual ~SpeechRecognitionListener() = default;

  /// Called when the recognition session starts successfully.
  virtual void OnRecognitionStart(const SpeechRecognitionResponse&) {}
  /// Called when a new sentence begins.
  virtual void OnSentenceBegin(const SpeechRecognitionResponse&) {}
  /// Called when intermediate recognition results are available.
  virtual void OnRecognitionResultChange(const SpeechRecognitionResponse&) {}
  /// Called when a sentence ends with the final result.
  virtual void OnSentenceEnd(const SpeechRecognitionResponse&) {}
  /// Called when the entire recognition session completes.
  virtual void OnRecognitionComplete(const SpeechRecognitionResponse&) {}
  /// Called when an error occurs during recognition.
  virtual void OnFail(const SpeechRecognitionResponse* /*resp*/,
                      const ASRError& /*err*/) {}
};

/// The main client for realtime speech recognition (WebSocket).
///
/// Lifecycle and concurrency (mirrors the Go SDK):
/// - A SpeechRecognizer is single-use: once it reaches the stopped state
///   (via Stop() or a terminal error) it cannot be restarted. Create a new
///   instance to reconnect.
/// - All SetXxx options must be configured before Start() and must not be
///   called concurrently with Start().
/// - After Start() returns, Write() and Stop() may be called from threads
///   other than the one that called Start(). Recognition callbacks are
///   delivered on an internal reader thread.
/// - Stop() is safe to call from a recognition callback: re-entry is
///   detected via the reader thread ID. For terminal callbacks the
///   recognizer has already advanced to stopped, so Stop() returns
///   immediately with not-running; for non-terminal callbacks it sends the
///   end signal and returns without waiting (waiting would self-block).
class SpeechRecognizer {
 public:
  /// Production WebSocket endpoint for the TRTC-ASR service.
  static constexpr const char* kEndpoint = "wss://asr.cloud-rtc.com";

  /// - credential: TRTC authentication credential (copied)
  /// - engine_model_type: recognition engine model ("16k_zh", "8k_zh",
  ///   "16k_zh_en")
  /// - listener: callback listener; must outlive the recognizer
  SpeechRecognizer(const Credential& credential, std::string engine_model_type,
                   SpeechRecognitionListener* listener);
  ~SpeechRecognizer();

  SpeechRecognizer(const SpeechRecognizer&) = delete;
  SpeechRecognizer& operator=(const SpeechRecognizer&) = delete;

  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }
  /// Sets the audio encoding format. 1: PCM (default).
  void SetVoiceFormat(int format) { voice_format_ = format; }
  /// Sets whether to enable VAD. 0: disable, 1: enable (default).
  void SetNeedVad(int need_vad) { need_vad_ = need_vad; }
  /// Sets the number conversion mode. 0: none, 1: smart (default), 3: math.
  void SetConvertNumMode(int mode) { convert_num_mode_ = mode; }
  /// Sets the hotword list ID for biasing recognition.
  void SetHotwordId(std::string id) { hotword_id_ = std::move(id); }
  /// Sets a temporary inline hotword list: "word1|weight1,word2|weight2".
  void SetHotwordList(std::string list) { hotword_list_ = std::move(list); }
  /// Sets the custom language model ID.
  void SetCustomizationId(std::string id) { customization_id_ = std::move(id); }
  /// Sets the replacement word table ID.
  void SetReplaceTextId(std::string id) { replace_text_id_ = std::move(id); }
  /// Profanity filter: 0 off (default), 1 filter, 2 replace with *.
  void SetFilterDirty(int mode) { filter_dirty_ = mode; }
  /// Modal particle filter: 0 off (default), 1 partial, 2 strict.
  void SetFilterModal(int mode) { filter_modal_ = mode; }
  /// Sentence-ending punctuation filter: 0 off (default), 1 filter.
  void SetFilterPunc(int mode) { filter_punc_ = mode; }
  /// Empty-result callbacks: 0 deliver, 1 skip (server default). Calling this
  /// makes the choice explicit on the wire.
  void SetFilterEmptyResult(int mode) { filter_empty_result_ = mode; }
  /// Word-level timing: 0 no (default), 1 yes, 2 include punctuation.
  void SetWordInfo(int mode) { word_info_ = mode; }
  /// Silence detection threshold (ms). Range: 240-2000.
  void SetVadSilenceTime(int ms) { vad_silence_time_ = ms; }
  /// VAD profile: 0 = high recall, 1 = far-field filtering (server default).
  /// Calling this makes the choice explicit on the wire.
  void SetVadLevel(int level) { vad_level_ = level; }
  /// VAD noise suppression fine-tuning, range [0, 4]. Overrides the profile
  /// selected by SetVadLevel when set.
  void SetNoiseThreshold(double threshold) { noise_threshold_ = threshold; }
  /// Maximum speech time (ms). Range: 5000-90000, default: 60000.
  void SetMaxSpeakTime(int ms) { max_speak_time_ = ms; }
  /// Sample rate of the incoming PCM audio. Only 8000 is supported.
  void SetInputSampleRate(int rate) { input_sample_rate_ = rate; }
  /// Speaker diarization: 0 off (default), 1 anonymous clustering,
  /// 3 voiceprint role authentication.
  void SetSpeakerDiarization(int mode) { speaker_diarization_ = mode; }
  /// Expected speaker count hint. 0 = auto detection (default).
  void SetSpeakerNumber(int n) { speaker_number_ = n; }
  /// Temporary voiceprints; only used with voiceprint mode.
  void SetSpeakerRoles(std::vector<SpeakerRole> roles) {
    speaker_roles_ = std::move(roles);
  }
  /// Previously enrolled voiceprints by ID; only voiceprint mode.
  void SetVoiceprintIds(std::vector<std::string> ids) {
    voiceprint_ids_ = std::move(ids);
  }
  /// Custom voice ID. A UUID is generated when left empty.
  void SetVoiceId(std::string id) { voice_id_ = std::move(id); }
  /// Language hint for the bigmodel engine (e.g. "zh", "en", "auto").
  void SetLanguage(std::string lang) { language_ = std::move(lang); }

  /// Sets the timeout for a single audio write, clamped to [50ms, 30s]; a
  /// non-positive value resets to the default (5s). Clamping keeps Stop's
  /// worst-case exit time predictable.
  void SetWriteTimeout(std::chrono::milliseconds timeout);
  /// Sets how long Stop waits for the server's final response after sending
  /// the end signal, clamped to [1s, 60s]; a non-positive value resets to
  /// the default (10s).
  void SetStopTimeout(std::chrono::milliseconds timeout);

  /// Initiates the WebSocket connection and begins the recognition session.
  /// Throws ASRError on invalid options, connect failure, or if already
  /// started.
  void Start();

  /// Sends audio data to the ASR service (format per SetVoiceFormat).
  /// Throws ASRError.
  void Write(const uint8_t* data, size_t len);
  void Write(const std::vector<uint8_t>& data) { Write(data.data(), data.size()); }

  /// Gracefully stops the recognition session: sends the end signal and
  /// waits for the server's final response (up to stop timeout) before
  /// forcing the connection closed. Worst-case duration is bounded by write
  /// timeout plus stop timeout. Safe to call from a recognition callback.
  /// Throws ASRError (kErrNotStarted) when the recognizer is not running.
  void Stop();

 private:
  void ValidateOptions() const;
  void Connect();
  void ReadLoop();
  void ReadLoopInner(std::shared_ptr<internal::RecognizerSharedState> shared);
  void DispatchEvent(const SpeechRecognitionResponse& resp);
  void SafeOnFail(const SpeechRecognitionResponse* resp, const ASRError& err);
  void SafeComplete(const SpeechRecognitionResponse& resp);

  Credential credential_;
  SpeechRecognitionListener* listener_;

  // Runtime state shared with the reader thread and the stop watchdog via
  // shared_ptr, so those threads never dereference a destroyed recognizer.
  std::shared_ptr<internal::RecognizerSharedState> shared_ =
      std::make_shared<internal::RecognizerSharedState>();
  std::thread reader_thread_;

  // Configuration (set before Start).
  std::string endpoint_;
  std::string engine_model_type_;
  int voice_format_ = 1;
  int need_vad_ = 1;
  int convert_num_mode_ = 1;
  std::string hotword_id_;
  std::string hotword_list_;
  std::string customization_id_;
  std::string replace_text_id_;
  int filter_dirty_ = 0;
  int filter_modal_ = 0;
  int filter_punc_ = 0;
  std::optional<int> filter_empty_result_;
  int word_info_ = 0;
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

  std::chrono::milliseconds write_timeout_{5000};
  std::chrono::milliseconds stop_timeout_{10000};
};

}  // namespace trtc_asr
