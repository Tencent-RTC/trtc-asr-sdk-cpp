#include "trtc_asr/types.h"

#include "json_helper.h"
#include "trtc_asr/errors.h"

namespace trtc_asr {
namespace {

using nlohmann::json;

void ParseWordInfo(const json& j, WordInfo* out) {
  out->word = j.value("word", "");
  out->start_time = j.value("start_time", (int64_t)0);
  out->end_time = j.value("end_time", (int64_t)0);
  out->stable_flag = j.value("stable_flag", 0);
  out->speaker_id = j.value("speaker_id", 0);
  out->speaker_name = j.value("speaker_name", "");
}

void ParseSpeakerSegment(const json& j, SpeakerSegment* out) {
  out->speaker_id = j.value("speaker_id", 0);
  out->speaker_name = j.value("speaker_name", "");
  out->start_time = j.value("start_time", (int64_t)0);
  out->end_time = j.value("end_time", (int64_t)0);
  out->text = j.value("text", "");
  if (j.contains("word_start") && j["word_start"].is_number()) {
    out->word_start = j["word_start"].get<int>();
  }
  if (j.contains("word_end") && j["word_end"].is_number()) {
    out->word_end = j["word_end"].get<int>();
  }
  out->stable_flag = j.value("stable_flag", 0);
}

SpeechRecognitionResponse ParseSpeechResponseInner(const json& j) {
  SpeechRecognitionResponse resp;
  resp.code = j.value("code", 0);
  resp.message = j.value("message", "");
  resp.voice_id = j.value("voice_id", "");
  resp.message_id = j.value("message_id", "");
  resp.final_flag = j.value("final", 0);
  if (j.contains("speaker_continue") && j["speaker_continue"].is_object()) {
    const json& sc = j["speaker_continue"];
    resp.speaker_continue = SpeakerContinue{
        sc.value("continue_status", ""),
        sc.value("speaker_context_id", "")};
  }

  if (j.contains("result") && j["result"].is_object()) {
    resp.has_result = true;
    const json& r = j["result"];
    resp.result.slice_type = r.value("slice_type", 0);
    resp.result.index = r.value("index", 0);
    resp.result.start_time = r.value("start_time", (int64_t)0);
    resp.result.end_time = r.value("end_time", (int64_t)0);
    resp.result.voice_text_str = r.value("voice_text_str", "");
    resp.result.word_size = r.value("word_size", 0);
    resp.result.language = r.value("language", "");
    resp.result.finish_silence_ms = r.value("finish_silence_ms", (int64_t)0);
    resp.result.last_token_runtime_ms = r.value("last_token_runtime_ms", (int64_t)0);
    if (r.contains("speaker_id") && r["speaker_id"].is_number()) {
      resp.result.speaker_id = r["speaker_id"].get<int>();
    }
    if (r.contains("word_list") && r["word_list"].is_array()) {
      for (const auto& w : r["word_list"]) {
        WordInfo wi;
        ParseWordInfo(w, &wi);
        resp.result.word_list.push_back(std::move(wi));
      }
    }
    if (r.contains("speaker_segments") && r["speaker_segments"].is_array()) {
      for (const auto& s : r["speaker_segments"]) {
        SpeakerSegment seg;
        ParseSpeakerSegment(s, &seg);
        resp.result.speaker_segments.push_back(std::move(seg));
      }
    }
  }
  return resp;
}

}  // namespace

SpeechRecognitionResponse ParseSpeechResponse(const std::string& text) {
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception& e) {
    throw ASRError(kErrReadFailed,
                   std::string("unmarshal response failed: ") + e.what());
  }

  // Field-type errors (e.g. "code":"oops", "result":[]) raise
  // nlohmann::type_error; converge them onto the SDK error model instead of
  // leaking third-party exception types through the API boundary.
  try {
    return ParseSpeechResponseInner(j);
  } catch (const std::exception& e) {
    throw ASRError(kErrReadFailed,
                   std::string("unmarshal response failed: ") + e.what());
  }
}

}  // namespace trtc_asr
