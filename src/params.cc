#include "trtc_asr/params.h"

#include <cmath>

#include "trtc_asr/errors.h"

namespace trtc_asr {
namespace {

/// Requires an absolute http(s) URL for enrollment audio.
///
/// The URL is fetched by the ASR service, not by the SDK: this is a
/// customer-facing client library, so it only rejects inputs that can never
/// work (bad syntax, non-http scheme, missing host). Reachability and network
/// policies belong to the service-side allow list.
void ValidateEnrollmentURL(size_t index, const std::string& raw_url) {
  auto fail = [&](const std::string& what) {
    throw ASRError(kErrInvalidParam,
                   "SpeakerRoles[" + std::to_string(index) + "].AudioUrl " + what);
  };

  if (raw_url.find_first_not_of(" \t\r\n") == std::string::npos) {
    fail("is empty");
  }
  // Go's url.ParseRequestURI rejects control characters and spaces; mirror
  // that so obviously malformed URLs fail locally with a clear message.
  for (unsigned char c : raw_url) {
    if (c < 0x20 || c == 0x7F) {
      fail("is not a valid URL: contains control character");
    }
  }
  if (raw_url.find(' ') != std::string::npos) {
    fail("is not a valid URL: contains space");
  }
  // Split scheme://authority explicitly to match Go's url.ParseRequestURI
  // semantics: "https:///a.wav" has an empty host there.
  size_t scheme_end = raw_url.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) {
    fail("is not a valid URL: missing scheme");
  }
  std::string scheme = raw_url.substr(0, scheme_end);
  if (scheme != "http" && scheme != "https") {
    fail("must use http or https, got \"" + scheme + "\"");
  }
  std::string rest = raw_url.substr(scheme_end + 3);
  size_t auth_end = rest.find_first_of("/?#");
  std::string authority = rest.substr(0, auth_end);
  if (authority.empty()) {
    fail("has no host");
  }
}

}  // namespace

void ValidateSpeakerDiarization(int mode, int speaker_number,
                                const std::vector<SpeakerRole>& roles,
                                const std::vector<std::string>& voiceprint_ids) {
  if (mode != kSpeakerDiarizationOff && mode != kSpeakerDiarizationCluster &&
      mode != kSpeakerDiarizationVoiceprint) {
    throw ASRError(kErrInvalidParam,
                   "SpeakerDiarization must be 0 (off), 1 (cluster) or 3 "
                   "(voiceprint), got " +
                       std::to_string(mode));
  }

  if (speaker_number < 0) {
    throw ASRError(kErrInvalidParam,
                   "SpeakerNumber must be >= 0 (0 = auto detection), got " +
                       std::to_string(speaker_number));
  }

  if (mode != kSpeakerDiarizationVoiceprint &&
      (!roles.empty() || !voiceprint_ids.empty())) {
    throw ASRError(kErrInvalidParam,
                   "SpeakerRoles/VoiceprintIds require SpeakerDiarization=3");
  }

  for (size_t i = 0; i < roles.size(); i++) {
    if (roles[i].role_name.empty()) {
      throw ASRError(kErrInvalidParam,
                     "SpeakerRoles[" + std::to_string(i) + "].RoleName is empty");
    }
    ValidateEnrollmentURL(i, roles[i].audio_url);
  }

  for (size_t i = 0; i < voiceprint_ids.size(); i++) {
    if (voiceprint_ids[i].empty()) {
      throw ASRError(kErrInvalidParam,
                     "VoiceprintIds[" + std::to_string(i) + "] is empty");
    }
  }
}

void ValidateVadTuning(const std::optional<int>& vad_level,
                       const std::optional<double>& noise_threshold) {
  if (vad_level.has_value() && *vad_level != 0 && *vad_level != 1) {
    throw ASRError(kErrInvalidParam,
                   "VadLevel must be 0 (high recall) or 1 (far-field "
                   "filtering), got " +
                       std::to_string(*vad_level));
  }
  if (noise_threshold.has_value()) {
    double v = *noise_threshold;
    // NaN fails every comparison, so test the valid range positively.
    if (!(v >= kMinNoiseThreshold && v <= kMaxNoiseThreshold)) {
      char buf[96];
      std::snprintf(buf, sizeof(buf),
                    "NoiseThreshold must be between %.1f and %.1f, got %g",
                    kMinNoiseThreshold, kMaxNoiseThreshold, v);
      throw ASRError(kErrInvalidParam, buf);
    }
  }
}

void ValidateEnumOption(const char* name, int value, std::vector<int> allowed) {
  for (int candidate : allowed) {
    if (value == candidate) return;
  }
  std::string list;
  for (size_t i = 0; i < allowed.size(); i++) {
    if (i > 0) list += ", ";
    list += std::to_string(allowed[i]);
  }
  throw ASRError(kErrInvalidParam, std::string(name) + " must be one of [" + list +
                                       "], got " + std::to_string(value));
}

}  // namespace trtc_asr
