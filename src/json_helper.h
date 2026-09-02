#pragma once

// Internal JSON helpers built on the vendored nlohmann/json single header.

#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "trtc_asr/signature_params.h"

namespace trtc_asr {
namespace internal {

inline std::string SerializeSpeakerRoles(const std::vector<SpeakerRole>& roles) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& role : roles) {
    nlohmann::json obj;
    obj["RoleName"] = role.role_name;
    obj["AudioUrl"] = role.audio_url;
    arr.push_back(std::move(obj));
  }
  return arr.dump();
}

inline std::string SerializeStringArray(const std::vector<std::string>& values) {
  nlohmann::json arr = values;
  return arr.dump();
}

/// Reads a string field without throwing on type mismatch (a malformed
/// server response like "Code":123 or "RequestId":{} yields "" instead of
/// nlohmann::type_error leaking through the API boundary).
inline std::string StringFieldOrEmpty(const nlohmann::json& obj, const char* key) {
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) {
    return "";
  }
  return it->get<std::string>();
}

}  // namespace internal
}  // namespace trtc_asr
