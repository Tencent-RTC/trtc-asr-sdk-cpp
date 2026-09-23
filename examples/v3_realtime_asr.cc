// Realtime speech recognition example over the v3 protocol (/asr/v3).
//
// Reads a PCM file (16kHz 16bit mono) and streams it in 200ms chunks.
//
// Credentials come from environment variables:
//   TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
// (v3 does not need the Tencent Cloud APPID.)
//
// Usage: ./v3_realtime_asr <audio.pcm> [engine] [lang] [speaker-context] [speaker-context-id]
//
// Speaker diarization can be made resumable across connections
// ("断点续传"): keep the printed speaker_context_id and pass it back on the
// next connection, e.g. `... test.pcm bigmodel zh 1` then
// `... test.pcm bigmodel zh 1 <speaker_context_id>`.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "trtc_asr/v3.h"

class Printer : public trtc_asr::SpeechRecognitionListener {
 public:
  void OnRecognitionStart(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[start] voice_id=" << resp.voice_id << "\n";
    if (resp.speaker_continue.has_value()) {
      std::cout << "speaker context: status=\"" << resp.speaker_continue->continue_status
                << "\" id=" << resp.speaker_continue->speaker_context_id << "\n";
    }
  }
  void OnSentenceEnd(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[end] " << resp.result.voice_text_str << "\n";
    for (const auto& seg : resp.result.speaker_segments) {
      const std::string label =
          seg.speaker_name.empty() ? "spk" + std::to_string(seg.speaker_id) : seg.speaker_name;
      std::cout << "  [" << label << "] " << seg.text << " (" << seg.start_time << "-"
                << seg.end_time << "ms)\n";
    }
  }
  void OnRecognitionResultChange(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[change] " << resp.result.voice_text_str << "\n";
  }
  void OnRecognitionComplete(const trtc_asr::SpeechRecognitionResponse&) override {
    std::cout << "[complete]\n";
  }
  void OnFail(const trtc_asr::SpeechRecognitionResponse*, const trtc_asr::ASRError& err) override {
    std::cerr << "[fail] " << err.what() << "\n";
  }
};

int main(int argc, char** argv) {
  const char* sdk_app_id_env = std::getenv("TRTC_ASR_SDK_APP_ID");
  const char* secret_key_env = std::getenv("TRTC_ASR_SECRET_KEY");
  const int64_t sdk_app_id = sdk_app_id_env ? std::atoll(sdk_app_id_env) : 0;
  const std::string secret_key = secret_key_env ? secret_key_env : "";
  if (sdk_app_id == 0 || secret_key.empty()) {
    std::cerr << "Set TRTC_ASR_SDK_APP_ID and TRTC_ASR_SECRET_KEY first.\n";
    return 1;
  }
  const std::string path = argc > 1 ? argv[1] : "examples/test.pcm";
  if (argc <= 2) {
    std::cerr << "error: engine argument is required (engine model type, e.g. bigmodel)\n";
    return 2;
  }
  const std::string engine = argv[2];
  // Empty lang falls back to server-side language detection.
  std::string lang = argc > 3 ? argv[3] : "";
  // The bigmodel engine is best used with an explicit language; every other
  // engine falls back to server-side detection unless lang is given.
  if (lang.empty() && engine == "bigmodel") {
    lang = "zh";
  }

  // arg[3]: resumable speaker diarization (0 off / 1 sync / 2 async);
  // arg[4]: the speaker_context_id printed by an earlier run.
  const int speaker_context = argc > 4 ? std::atoi(argv[4]) : 0;
  const std::string speaker_context_id = argc > 5 ? argv[5] : "";

  // v3 credentials need only SdkAppID + SecretKey (no Tencent Cloud APPID).
  Printer listener;
  trtc_asr::v3::SpeechRecognizer recognizer(
      trtc_asr::v3::NewCredential(sdk_app_id, secret_key), engine, &listener);
  if (!lang.empty()) {
    recognizer.SetLanguage(lang); // bigmodel 建议显式指定语种
  }
  // Speaker-context persistence requires speaker diarization; the SDK rejects
  // the combination locally otherwise.
  if (speaker_context != 0) {
    recognizer.SetSpeakerDiarization(trtc_asr::v3::kSpeakerDiarizationCluster);
    recognizer.SetEnableSpeakerContext(speaker_context);
  }
  if (!speaker_context_id.empty()) {
    recognizer.SetSpeakerContextId(speaker_context_id);
  }

  // Start() waits synchronously for the server's ack: authentication (4002)
  // and gray-switch (4001) errors are thrown here, not via OnFail. While
  // resuming a speaker context the ack arrives once the server has applied
  // the stored snapshot.
  try {
    recognizer.Start();
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "start failed: " << e.what() << "\n";
    return 1;
  }

  // The handshake result is also available without a callback. Persist the
  // id: passing it back keeps speaker ids stable across connections.
  if (recognizer.GetSpeakerContinue().has_value()) {
    const auto& sc = *recognizer.GetSpeakerContinue();
    std::cout << "speaker context id: " << sc.speaker_context_id << " (status: \""
              << sc.continue_status << "\") — reuse it as the 5th argument\n";
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << path << "\n";
    return 1;
  }
  // 200ms of 16kHz 16bit mono PCM. The server rate-limits to at most 3s of
  // audio per 1s wall-clock (error 4000): keep the pacing when enlarging
  // the buffer.
  std::vector<uint8_t> buf(6400);
  while (file) {
    file.read(reinterpret_cast<char*>(buf.data()), buf.size());
    const std::streamsize n = file.gcount();
    if (n <= 0) break;
    recognizer.Write(buf.data(), static_cast<size_t>(n));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  recognizer.Stop();
  return 0;
}
