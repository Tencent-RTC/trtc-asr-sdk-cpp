// Realtime speech recognition example over the v3 protocol (/asr/v3).
//
// Reads a PCM file (16kHz 16bit mono) and streams it in 200ms chunks.
//
// Credentials come from environment variables:
//   TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
// (v3 does not need the Tencent Cloud APPID.)
//
// Prerequisite: the server has enabled the EnableV3Route gray switch for
// your SDKAppID, otherwise Start() fails with 4001.
//
// Usage: ./v3_realtime_asr <audio.pcm> [engine]

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
  }
  void OnSentenceEnd(const trtc_asr::SpeechRecognitionResponse& resp) override {
    std::cout << "[end] " << resp.result.voice_text_str << "\n";
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
  const std::string engine = argc > 2 ? argv[2] : "16k_zh_en";

  // v3 credentials need only SdkAppID + SecretKey (no Tencent Cloud APPID).
  Printer listener;
  trtc_asr::v3::SpeechRecognizer recognizer(
      trtc_asr::v3::NewCredential(sdk_app_id, secret_key), engine, &listener);

  // Start() waits synchronously for the server's ack: authentication (4002)
  // and gray-switch (4001) errors are thrown here, not via OnFail.
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
