#include "trtc_asr/speech_recognizer.h"

#include "trtc_asr/params.h"
#include "trtc_asr/usersig.h"
#include "ws_client.h"

namespace trtc_asr {
namespace {

constexpr int kHandshakeTimeoutMs = 10000;

// Poll interval for the reader thread: Read uses a short timeout so Close
// can always interrupt a blocked read promptly.
constexpr int kReadPollMs = 100;

using S = internal::RecognizerSharedState;

}  // namespace

// SharedState methods that touch the WsClient (incomplete in the header).

void internal::RecognizerSharedState::Finish() {
  std::call_once(finish_once, [this] {
    state.store(kStateStopped_);
    Close();
  });
}

void internal::RecognizerSharedState::Close() {
  std::lock_guard<std::mutex> lock(conn_mu);
  if (conn) {
    conn->Close();
    conn.reset();
  }
}

void internal::RecognizerSharedState::WaitForReadLoopOrClose() {
  std::unique_lock<std::mutex> lock(done_mu);
  // Absolute deadline: spurious wakes must NOT reset the budget, otherwise
  // a stream of them could keep Stop() blocked far beyond stop_timeout
  // (Java already uses the deadline pattern).
  const auto deadline = std::chrono::steady_clock::now() + stop_timeout;
  while (true) {
    if (done) return;
    if (terminal_received.load()) {
      // The terminal response has arrived (possibly after this waiter
      // entered the timed wait); wait without a timeout so the terminal
      // callbacks can finish — mirrors the Go SDK's
      // waitForCallbacksOrAbort terminal branch.
      done_cv.wait(lock, [this] { return done; });
      return;
    }
    // Non-predicate wait_until: a wake from SignalDone or
    // MarkTerminalReceived re-enters the loop and re-checks both flags
    // (the predicate overload would swallow the terminal wake and keep
    // sleeping until the deadline).
    if (done_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      if (done) return;
      if (terminal_received.load()) continue;  // terminal raced with timeout
      // Timed out waiting for the server's final response: force the
      // connection closed so the reader thread exits.
      Close();
      // Give the reader thread a brief chance to run its exit path; callers
      // must not wait indefinitely here.
      done_cv.wait_for(lock, std::chrono::milliseconds(300));
      return;
    }
  }
}

SpeechRecognizer::SpeechRecognizer(const Credential& credential,
                                   std::string engine_model_type,
                                   SpeechRecognitionListener* listener)
    : credential_(credential),
      listener_(listener),
      endpoint_(),
      engine_model_type_(std::move(engine_model_type)) {}

SpeechRecognizer::~SpeechRecognizer() {
  // Ensure the connection is torn down if the user drops the recognizer
  // without Stop. Best effort; errors are irrelevant here.
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
  ValidateSpeakerDiarization(speaker_diarization_, speaker_number_, speaker_roles_,
                             voiceprint_ids_);
  ValidateVadTuning(vad_level_, noise_threshold_);
  if (filter_empty_result_.has_value()) {
    ValidateEnumOption("FilterEmptyResult", *filter_empty_result_, {0, 1});
  }
  // 8000 is the only supported override; 0 means "use the engine rate".
  ValidateEnumOption("InputSampleRate", input_sample_rate_, {0, 8000});
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

void SpeechRecognizer::Connect() {
  if (voice_id_.empty()) {
    voice_id_ = internal::GenerateUuid();
  }

  // Resolve UserSig locally without mutating the shared credential.
  std::string user_sig = credential_.user_sig();
  if (user_sig.empty()) {
    try {
      user_sig = GenUserSig(credential_.sdk_app_id(), credential_.secret_key(),
                            voice_id_, 86400);
    } catch (const ASRError& e) {
      throw ASRError(kErrAuthFailed,
                     "generate user sig failed: " + std::string(e.message()));
    }
  }

  // Authentication identity (sdkappid + usersig) travels in the query string
  // instead of headers, so browser WebSocket clients work without header
  // support; the gateway reads these query parameters when the corresponding
  // headers are absent.
  SignatureParams p(credential_.app_id(), engine_model_type_, voice_id_);
  p.sdk_app_id = credential_.sdk_app_id();
  p.voice_format = voice_format_;
  p.need_vad = need_vad_;
  p.convert_num_mode = convert_num_mode_;
  p.hotword_id = hotword_id_;
  p.hotword_list = hotword_list_;
  p.customization_id = customization_id_;
  p.replace_text_id = replace_text_id_;
  p.filter_dirty = filter_dirty_;
  p.filter_modal = filter_modal_;
  p.filter_punc = filter_punc_;
  p.filter_empty_result = filter_empty_result_;
  p.word_info = word_info_;
  p.vad_silence_time = vad_silence_time_;
  p.vad_level = vad_level_;
  p.noise_threshold = noise_threshold_;
  p.max_speak_time = max_speak_time_;
  p.input_sample_rate = input_sample_rate_;
  p.speaker_diarization = speaker_diarization_;
  p.speaker_number = speaker_number_;
  p.speaker_roles = speaker_roles_;
  p.voiceprint_ids = voiceprint_ids_;
  p.language = language_;

  std::string query = p.BuildQueryStringWithSignature(user_sig);
  // URL path uses the Tencent Cloud AppID (not SdkAppID).
  std::string base = ResolveWSEndpoint(endpoint_, credential_.site());
  std::string ws_url =
      base + "/asr/v2/" + std::to_string(credential_.app_id()) + "?" + query;

  std::string err;
  auto conn = internal::WsClient::Connect(ws_url, kHandshakeTimeoutMs, &err);
  if (!conn) {
    throw ASRError(kErrConnectFailed, "websocket dial failed: " + err);
  }
  conn->SetWriteTimeoutMs(static_cast<int>(write_timeout_.count()));

  std::lock_guard<std::mutex> lock(shared_->conn_mu);
  shared_->conn = std::shared_ptr<internal::WsClient>(std::move(conn));
}

void SpeechRecognizer::Write(const uint8_t* data, size_t len) {
  if (data == nullptr && len > 0) {
    throw ASRError(kErrInvalidParam, "audio data pointer is null");
  }
  if (shared_->state.load() != S::kStateRunning_) {
    throw ASRError(kErrNotStarted, "recognizer not running");
  }

  std::shared_ptr<internal::WsClient> conn;
  {
    std::lock_guard<std::mutex> lock(shared_->conn_mu);
    conn = shared_->conn;
  }
  if (!conn) {
    throw ASRError(kErrNotStarted, "connection not established");
  }

  // Re-check the state under write_mu. Between the entry check and acquiring
  // write_mu, Stop may have transitioned the state and sent the end signal.
  // Writing audio after end would violate the protocol, so bail out instead.
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

  std::shared_ptr<internal::WsClient> conn;
  {
    std::lock_guard<std::mutex> lock(shared_->conn_mu);
    conn = shared_->conn;
  }
  if (!conn) {
    shared_->state.store(S::kStateStopped_);
    throw ASRError(kErrNotStarted, "connection not established");
  }

  // Send the end signal, serialized with Write via write_mu.
  std::string send_err;
  bool send_failed = false;
  bool already_stopped = false;
  {
    std::lock_guard<std::mutex> write_lock(shared_->write_mu);
    if (shared_->state.load() == S::kStateStopped_) {
      // The session finished naturally while we waited for an in-flight
      // writer: skip the end signal entirely.
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

  // If Stop is called from within a listener callback (which runs on the
  // reader thread), waiting on done here would self-block until timeout.
  // Return after sending end; the watchdog preserves Stop's timeout
  // semantics if the server never sends a terminal response. The watchdog
  // captures the shared state (not `this`), so it stays valid even if the
  // recognizer is destroyed first.
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

  // A listener callback that throws must never crash the host process:
  // finish the lifecycle first (so a re-entrant Stop from OnFail observes
  // the stopped state), then surface the failure.
  //
  // Structure note: all exit paths live in ReadLoopInner (plain returns);
  // SignalDone below always runs — mirroring the Go readLoop's deferred
  // closeDone.
  try {
    ReadLoopInner(shared);
  } catch (const std::exception& e) {
    shared->Finish();
    SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                 std::string("recovered from panic in read loop: ") +
                                     e.what()));
  } catch (...) {
    shared->Finish();
    SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                 "recovered from panic in read loop: unknown exception"));
  }

  // Finish is idempotent; this is the catch-all for exit paths that did not
  // finish explicitly.
  shared->Finish();
  shared->SignalDone();
}

void SpeechRecognizer::ReadLoopInner(std::shared_ptr<internal::RecognizerSharedState> shared) {
  {
    SpeechRecognitionResponse start_resp;
    start_resp.code = 0;
    start_resp.message = "success";
    start_resp.voice_id = voice_id_;
    listener_->OnRecognitionStart(start_resp);
  }

  std::shared_ptr<internal::WsClient> conn;
  {
    std::lock_guard<std::mutex> lock(shared->conn_mu);
    conn = shared->conn;
  }
  if (!conn) {
    return;
  }

    while (true) {
      internal::WsFrame frame;
      std::string read_err;
      int rc = conn->Read(&frame, kReadPollMs, &read_err);
      if (rc == 0) {
        if (shared->state.load() == S::kStateStopped_) return;
        continue;
      }
      if (rc < 0) {
        if (shared->state.load() >= S::kStateStopping_) return;
        // Terminal: finish the lifecycle before notifying, so a Stop/Write
        // from inside OnFail sees the stopped state.
        shared->Finish();
        SafeOnFail(nullptr, ASRError(kErrReadFailed,
                                     "read message failed" +
                                         (read_err.empty() ? "" : ": " + read_err)));
        return;
      }

      if (frame.opcode == 0x2) {
        // The server protocol only uses text frames; treat binary as an
        // unmarshal failure (non-terminal), like the Go SDK does.
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
      if (frame.opcode != 0x1) {
        continue;
      }

      SpeechRecognitionResponse resp;
      try {
        resp = ParseSpeechResponse(frame.payload);
      } catch (const ASRError& e) {
        // Non-terminal: the session continues.
        SafeOnFail(nullptr, e);
        continue;
      }

      if (resp.code != 0) {
        shared->Finish();
        shared->MarkTerminalReceived();
        SafeOnFail(&resp, ASRError(resp.code, resp.message));
        return;
      }

      // Check if recognition is complete before dispatching the terminal
      // response. A final=1 response can still carry slice_type=2, which
      // dispatches OnSentenceEnd; finish first so Stop/Write from that
      // callback observes the stopped state.
      if (resp.final_flag == 1) {
        shared->Finish();
        shared->MarkTerminalReceived();
        DispatchEvent(resp);
        SafeComplete(resp);
        return;
      }

      // Skip the connection acknowledgement frame. After connect, the server
      // sends an ack that carries no "result" object; a zero-valued
      // slice_type would otherwise be misread as "sentence begin".
      if (!resp.has_result) {
        continue;
      }

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

void SpeechRecognizer::SafeOnFail(const SpeechRecognitionResponse* resp,
                                  const ASRError& err) {
  try {
    listener_->OnFail(resp, err);
  } catch (...) {
    // A faulty OnFail must never crash the host process.
  }
}

void SpeechRecognizer::SafeComplete(const SpeechRecognitionResponse& resp) {
  try {
    listener_->OnRecognitionComplete(resp);
  } catch (...) {
    // Same panic-shielding guarantee as SafeOnFail.
  }
}

}  // namespace trtc_asr
