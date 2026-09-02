// Realtime speech recognition example.
//
// Reads a PCM file (16kHz 16bit mono) and streams it in 200ms chunks.
//
// Credentials come from environment variables:
//   TRTC_ASR_APP_ID, TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
//
// Usage: ./realtime_asr <audio.pcm> [engine_model_type]

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "trtc_asr/speech_recognizer.h"

namespace {

class Printer : public trtc_asr::SpeechRecognitionListener {
 public:
  void OnRecognitionStart(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[start] voice_id=" << resp.voice_id << "\n";
  }
  void OnSentenceBegin(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[begin] index=" << resp.result.index << "\n";
  }
  void OnRecognitionResultChange(
      const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[change] " << resp.result.voice_text_str << "\n";
  }
  void OnSentenceEnd(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[end] index=" << resp.result.index
              << " text=" << resp.result.voice_text_str << " (" << resp.result.start_time
              << "-" << resp.result.end_time << "ms)\n";
    for (const auto& seg : resp.result.speaker_segments) {
      std::string name = seg.speaker_name.empty()
                             ? "spk" + std::to_string(seg.speaker_id)
                             : seg.speaker_name;
      std::cout << "       [" << name << "] " << seg.text << " (" << seg.start_time
                << "-" << seg.end_time << "ms)\n";
    }
  }
  void OnRecognitionComplete(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[complete] voice_id=" << resp.voice_id << "\n";
  }
  void OnFail(const trtc_asr::SpeechRecognitionResponse*,
              const trtc_asr::ASRError& err) override {
    std::cerr << "[fail] " << err.what() << "\n";
  }
};

int64_t EnvInt(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr) {
    std::cerr << "missing env var: " << name << "\n";
    std::exit(1);
  }
  return std::stoll(v);
}

std::string EnvStr(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr) {
    std::cerr << "missing env var: " << name << "\n";
    std::exit(1);
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <audio.pcm> [engine_model_type]\n";
    return 1;
  }
  std::string path = argv[1];
  std::string engine = argc > 2 ? argv[2] : "16k_zh_en";

  trtc_asr::Credential credential(EnvInt("TRTC_ASR_APP_ID"), EnvInt("TRTC_ASR_SDK_APP_ID"),
                                  EnvStr("TRTC_ASR_SECRET_KEY"));

  Printer listener;
  trtc_asr::SpeechRecognizer recognizer(credential, engine, &listener);
  // recognizer.SetSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster);
  // recognizer.SetWordInfo(1);

  try {
    recognizer.Start();
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "start failed: " << e.what() << "\n";
    return 1;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << path << "\n";
    return 1;
  }

  std::vector<uint8_t> buf(6400);  // 200ms of 16kHz 16bit mono PCM
  while (file) {
    file.read(reinterpret_cast<char*>(buf.data()), buf.size());
    std::streamsize n = file.gcount();
    if (n <= 0) break;
    try {
      recognizer.Write(buf.data(), static_cast<size_t>(n));
    } catch (const trtc_asr::ASRError& e) {
      std::cerr << "write failed: " << e.what() << "\n";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // simulate realtime
  }

  try {
    recognizer.Stop();
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "stop failed: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
