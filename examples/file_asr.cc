// Async file recognition example (long audio, <= 12h).
//
// Credentials come from environment variables:
//   TRTC_ASR_APP_ID, TRTC_ASR_SDK_APP_ID, TRTC_ASR_SECRET_KEY
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
    std::cerr << "usage: " << argv[0]
              << " <audio-file> <engine> [lang] | -u <url> <engine> [lang]\n";
    return 1;
  }
  const bool url_mode = std::string(argv[1]) == "-u";
  // engine is required (the protocol has no default); lang is optional and an
  // empty lang falls back to server-side language detection.
  const int engine_idx = url_mode ? 3 : 2;
  if (argc <= engine_idx) {
    std::cerr << "error: engine argument is required (engine model type, e.g. bigmodel)\n";
    return 2;
  }
  const std::string engine = argv[engine_idx];
  std::string lang = argc > (url_mode ? 4 : 3) ? argv[url_mode ? 4 : 3] : "";
  // The bigmodel engine is best used with an explicit language; every other
  // engine falls back to server-side detection unless lang is given.
  if (lang.empty() && engine == "bigmodel") {
    lang = "zh";
  }

  trtc_asr::Credential credential(EnvInt("TRTC_ASR_APP_ID"), EnvInt("TRTC_ASR_SDK_APP_ID"),
                                  EnvStr("TRTC_ASR_SECRET_KEY"));
  trtc_asr::FileRecognizer recognizer(credential);

  try {
    trtc_asr::CreateRecTaskRequest req;
    req.engine_model_type = engine;
    req.channel_num = 1;
    req.res_text_format = 1;
    if (!lang.empty()) {
      req.language = lang;
    }
    std::string task_id;
    if (url_mode) {
      if (argc < 3) {
        std::cerr << "missing url after -u\n";
        return 1;
      }
      req.source_type = trtc_asr::SentenceRecognizer::kSourceTypeURL;
      req.url = argv[2];
      task_id = recognizer.CreateTask(req);
    } else {
      std::ifstream file(argv[1], std::ios::binary);
      if (!file) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
      }
      std::vector<uint8_t> data(std::istreambuf_iterator<char>(file), {});
      task_id = recognizer.CreateTaskFromDataWithOptions(data, &req);
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
