// One-shot sentence recognition example (audio <= 60s / 3MB).
//
// Credentials come from environment variables:
//   TRTC_ASR_APP_ID, TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
//
// Usage: ./sentence_asr <audio.pcm> [format=pcm] [engine=bigmodel]

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "trtc_asr/sentence_recognizer.h"

namespace {

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
    std::cerr << "usage: " << argv[0] << " <audio-file> [format=pcm] <engine> [lang]\n";
    return 1;
  }
  std::string path = argv[1];
  std::string format = argc > 2 ? argv[2] : "pcm";
  if (argc <= 3) {
    std::cerr << "error: engine argument is required (engine model type, e.g. bigmodel)\n";
    return 2;
  }
  std::string engine = argv[3];
  // Empty lang falls back to server-side language detection.
  std::string lang = argc > 4 ? argv[4] : "";
  // The bigmodel engine is best used with an explicit language; every other
  // engine falls back to server-side detection unless lang is given.
  if (lang.empty() && engine == "bigmodel") {
    lang = "zh";
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << path << "\n";
    return 1;
  }
  std::vector<uint8_t> data(std::istreambuf_iterator<char>(file), {});

  trtc_asr::Credential credential(EnvInt("TRTC_ASR_APP_ID"), EnvInt("TRTC_ASR_SDK_APP_ID"),
                                  EnvStr("TRTC_ASR_SECRET_KEY"));
  trtc_asr::SentenceRecognizer recognizer(credential);

  try {
    trtc_asr::SentenceRecognitionRequest req;
    req.eng_service_type = engine;
    req.voice_format = format;
    if (!lang.empty()) {
      req.language = lang;
    }
    auto result = recognizer.RecognizeDataWithOptions(data, &req);
    std::cout << "识别结果: " << result.result << "\n";
    std::cout << "音频时长: " << result.audio_duration << " ms\n";
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "识别失败: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
