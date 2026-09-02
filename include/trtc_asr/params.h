#pragma once

#include <optional>
#include <string>
#include <vector>

#include "trtc_asr/signature_params.h"

namespace trtc_asr {

/// Server-side accepted noise threshold range.
inline constexpr double kMinNoiseThreshold = 0.0;
inline constexpr double kMaxNoiseThreshold = 4.0;

/// Checks the diarization mode and its enrollment input. roles /
/// voiceprint_ids are only meaningful with mode 3, but supplying them for
/// another mode is a caller mistake worth surfacing.
///
/// Throws ASRError (kErrInvalidParam) on invalid input.
void ValidateSpeakerDiarization(int mode, int speaker_number,
                                const std::vector<SpeakerRole>& roles,
                                const std::vector<std::string>& voiceprint_ids);

/// Checks the VAD profile and noise threshold.
void ValidateVadTuning(const std::optional<int>& vad_level,
                       const std::optional<double>& noise_threshold);

/// Checks a small enumerated option such as input_sample_rate.
void ValidateEnumOption(const char* name, int value, std::vector<int> allowed);

}  // namespace trtc_asr
