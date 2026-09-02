// One-shot sentence recognition example (audio <= 60s / 3MB).
//
// Credentials come from environment variables:
//   TRTC_ASR_APP_ID, TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
//
// Usage: ./sentence_asr <audio.pcm> [format=pcm] [engine=16k_zh_en]

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
    std::cerr << "usage: " << argv[0] << " <audio-file> [format=pcm] [engine=16k_zh_en]\n";
    return 1;
  }
  std::string path = argv[1];
  std::string format = argc > 2 ? argv[2] : "pcm";
  std::string engine = argc > 3 ? argv[3] : "16k_zh_en";

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
    auto result = recognizer.RecognizeData(data, format, engine);
    std::cout << "识别结果: " << result.result << "\n";
    std::cout << "音频时长: " << result.audio_duration << " ms\n";
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "识别失败: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
