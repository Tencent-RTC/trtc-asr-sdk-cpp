// Async audio file recognition example over the v3 protocol
// (POST /v3/create_transcription + /v3/describe_transcription).
//
// Credentials come from environment variables:
//   TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
// (v3 does not need the Tencent Cloud APPID.)
//
// Usage: ./v3_file_asr <local.wav> | ./v3_file_asr -u <url> [engine]

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

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
  if (argc < 2) {
    std::cerr << "Usage: v3_file_asr <local.wav> <engine> [lang]"
                 " | v3_file_asr -u <url> <engine> [lang]\n";
    return 1;
  }
  const std::string first = argv[1];
  const bool url_mode = first == "-u";
  // engine is required (the protocol has no default); lang is optional and an
  // empty lang falls back to server-side language detection.
  const int engine_idx = url_mode ? 3 : 2;
  if (argc <= engine_idx) {
    std::cerr << "error: engine argument is required (engine model type, e.g. bigmodel)\n";
    return 2;
  }
  const std::string engine = argv[engine_idx];
  std::string lang =
      (url_mode && argc > 4) ? argv[4] : (!url_mode && argc > 3) ? argv[3] : "";
  // The bigmodel engine is best used with an explicit language; every other
  // engine falls back to server-side detection unless lang is given.
  if (lang.empty() && engine == "bigmodel") {
    lang = "zh";
  }

  // v3 credentials need only SdkAppID + SecretKey (no Tencent Cloud APPID).
  trtc_asr::v3::FileRecognizer recognizer(
      trtc_asr::v3::NewCredential(sdk_app_id, secret_key));

  try {
    trtc_asr::v3::CreateTranscriptionRequest req;
    req.engine_model_type = engine;
    req.channel_num = 1;
    req.res_text_format = 1;
    if (!lang.empty()) {
      req.language = lang;
    }
    std::string task_id;
    if (first == "-u") {
      req.source_type = trtc_asr::v3::kSourceTypeUrl;
      req.url = argv[2];
      task_id = recognizer.CreateTask(req);
    } else {
      std::ifstream file(first, std::ios::binary);
      if (!file) {
        std::cerr << "cannot open " << first << "\n";
        return 1;
      }
      const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
      task_id = recognizer.CreateTaskFromDataWithOptions(data, &req);
    }
    std::cout << "Task created: " << task_id << "\n";

    auto status = recognizer.WaitForResult(task_id);
    std::cout << "Status: " << status.status_str << "  Duration: " << status.audio_duration
              << " s\n";
    std::cout << "Result: " << status.result << "\n";
    for (const auto& d : status.result_detail) {
      std::string speaker;
      if (!d.speaker_role_name.empty()) {
        speaker = " [" + d.speaker_role_name + "]";
      } else if (d.speaker_id > 0) {
        speaker = " [spk" + std::to_string(d.speaker_id) + "]";
      }
      std::cout << "  [" << d.start_ms << " - " << d.end_ms << "]" << speaker << " "
                << d.final_sentence << "\n";
    }
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "failed: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
