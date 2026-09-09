// Async audio file recognition example over the v3 protocol
// (POST /v3/create_transcription + /v3/describe_transcription).
//
// Credentials come from environment variables:
//   TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
// (v3 does not need the Tencent Cloud APPID.)
//
// Prerequisite: the server has enabled the EnableV3Route gray switch for
// your SDKAppID, otherwise requests fail with 404/4001.
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
    std::cerr << "Usage: v3_file_asr <local.wav> | v3_file_asr -u <url> [engine]\n";
    return 1;
  }
  const std::string first = argv[1];
  const std::string engine = (first == "-u" && argc > 3) ? argv[3]
                             : (first != "-u" && argc > 2) ? argv[2] : "16k_zh_en";

  // v3 credentials need only SdkAppID + SecretKey (no Tencent Cloud APPID).
  trtc_asr::v3::FileRecognizer recognizer(
      trtc_asr::v3::NewCredential(sdk_app_id, secret_key));

  try {
    std::string task_id;
    if (first == "-u") {
      task_id = recognizer.CreateTaskFromUrl(argv[2], engine);
    } else {
      std::ifstream file(first, std::ios::binary);
      if (!file) {
        std::cerr << "cannot open " << first << "\n";
        return 1;
      }
      const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
      task_id = recognizer.CreateTaskFromData(data, engine);
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
