// Async file recognition example (long audio, <= 12h).
//
// Credentials come from environment variables:
//   TRTC_APP_ID, TRTC_SDK_APP_ID, TRTC_SECRET_KEY
//
// Usage:
//   ./file_asr <audio.pcm>          # local file
//   ./file_asr -u <https-url>       # remote URL

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "trtc_asr/file_recognizer.h"

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
    std::cerr << "usage: " << argv[0] << " <audio-file> | -u <url>\n";
    return 1;
  }

  trtc_asr::Credential credential(EnvInt("TRTC_APP_ID"), EnvInt("TRTC_SDK_APP_ID"),
                                  EnvStr("TRTC_SECRET_KEY"));
  trtc_asr::FileRecognizer recognizer(credential);

  try {
    std::string task_id;
    if (std::string(argv[1]) == "-u") {
      if (argc < 3) {
        std::cerr << "missing url after -u\n";
        return 1;
      }
      task_id = recognizer.CreateTaskFromURL(argv[2], "16k_zh_en");
    } else {
      std::ifstream file(argv[1], std::ios::binary);
      if (!file) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
      }
      std::vector<uint8_t> data(std::istreambuf_iterator<char>(file), {});
      task_id = recognizer.CreateTaskFromData(data, "pcm", "16k_zh_en");
    }
    std::cout << "任务已提交: " << task_id << "\n";

    auto status = recognizer.WaitForResult(task_id);
    std::cout << "识别结果: " << status.result << "\n";
    std::cout << "音频时长: " << status.audio_duration << " s\n";
    for (const auto& d : status.result_detail) {
      std::string speaker = !d.speaker_role_name.empty()    ? d.speaker_role_name
                            : d.channel_id > 0              ? "ch" + std::to_string(d.channel_id)
                                                            : "spk" + std::to_string(d.speaker_id);
      std::cout << "  [" << speaker << "] (" << d.start_ms << "-" << d.end_ms << "ms) "
                << d.final_sentence << "\n";
    }
  } catch (const trtc_asr::ASRError& e) {
    std::cerr << "识别失败: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
