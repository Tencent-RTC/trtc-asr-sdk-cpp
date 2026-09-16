// Sentence (one-shot) speech recognition example over the v3 protocol
// (POST /v3/transcribe).
//
// Recognizes a local audio file (<=60s, <=3MB).
//
// Credentials come from environment variables:
//   TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
// (v3 does not need the Tencent Cloud APPID.)
//
// Usage: ./v3_sentence_asr <audio.pcm> [engine]

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "trtc_asr/v3.h"

int main(int argc, char** argv) {
  const char* sdk_app_id_env = std::getenv("TRTC_ASR_SDK_APP_ID");
  const char* secret_key_env = std::getenv("TRTC_ASR_SECRET_KEY");
  const int64_t sdk_app_id = sdk_app_id_env ? std::atoll(sdk_app_id_env) : 0;
  const std::string secret_key = secret_key_env ? secret_key_env : "";
  if (sdk_app_id == 0 || secret_key.empty()) {
    std::cerr << "Set TRTC_ASR_SDK_APP_ID and TRTC_ASR_SECRET_KEY first.\n";
    return 1;
  }
  const std::string path = argc > 1 ? argv[1] : "audio.pcm";
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

  // v3 credentials need only SdkAppID + SecretKey (no Tencent Cloud APPID).
  trtc_asr::v3::SentenceRecognizer recognizer(
      trtc_asr::v3::NewCredential(sdk_app_id, secret_key));

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << path << "\n";
    return 1;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());

  try {
    trtc_asr::v3::TranscribeRequest req;
    req.engine_model_type = engine;
    req.voice_format = "pcm";
    if (!lang.empty()) {
      req.language = lang;
    }
    auto resp = recognizer.RecognizeDataWithOptions(data, &req);
    std::cout << "Result: " << resp.result << "\n";
    std::cout << "Duration: " << resp.audio_duration << " ms  RequestId: " << resp.request_id
              << "\n";
    for (const auto& w : resp.word_list) {
      std::cout << "  [" << w.start_time << " - " << w.end_time << "] " << w.word << "\n";
    }
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "recognize failed: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
