#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "trtc_asr/errors.h"
#include "trtc_asr/params.h"

namespace {

using trtc_asr::ASRError;
using trtc_asr::SpeakerRole;

SpeakerRole ValidRole() { return {"teacher", "https://example.com/a.wav"}; }

void ExpectInvalidParam(const std::function<void()>& fn,
                        const std::string& want_message) {
  try {
    fn();
    FAIL() << "expected ASRError containing: " << want_message;
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), trtc_asr::kErrInvalidParam);
    EXPECT_NE(std::string(e.message()).find(want_message), std::string::npos)
        << "error: " << e.message() << "\nwant: " << want_message;
  }
}

TEST(Params, DiarizationValidCases) {
  EXPECT_NO_THROW(trtc_asr::ValidateSpeakerDiarization(
      trtc_asr::kSpeakerDiarizationOff, 0, {}, {}));
  EXPECT_NO_THROW(trtc_asr::ValidateSpeakerDiarization(
      trtc_asr::kSpeakerDiarizationCluster, 0, {}, {}));
  EXPECT_NO_THROW(trtc_asr::ValidateSpeakerDiarization(
      trtc_asr::kSpeakerDiarizationCluster, 2, {}, {}));
  EXPECT_NO_THROW(trtc_asr::ValidateSpeakerDiarization(
      trtc_asr::kSpeakerDiarizationVoiceprint, 2, {ValidRole()}, {"vp-1"}));
}

TEST(Params, DiarizationInvalidCases) {
  ExpectInvalidParam(
      [] { trtc_asr::ValidateSpeakerDiarization(2, 0, {}, {}); },
      "SpeakerDiarization must be 0");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster, -1,
                                             {}, {});
      },
      "SpeakerNumber must be >= 0");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster, 0,
                                             {ValidRole()}, {});
      },
      "require SpeakerDiarization=3");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(trtc_asr::kSpeakerDiarizationOff, 0, {},
                                             {"vp-1"});
      },
      "require SpeakerDiarization=3");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0,
            {{"", "https://example.com/a.wav"}}, {});
      },
      "RoleName is empty");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0, {{"teacher", ""}}, {});
      },
      "AudioUrl is empty");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0,
            {{"teacher", "file:///etc/passwd"}}, {});
      },
      "must use http or https");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0,
            {{"teacher", "https:///a.wav"}}, {});
      },
      "has no host");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0, {}, {""});
      },
      "VoiceprintIds[0] is empty");
}

TEST(Params, DiarizationRejectsMalformedUrls) {
  // Spaces and control characters: Go's url.ParseRequestURI rejects these.
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0,
            {{"teacher", "https://exa mple.com/a.wav"}}, {});
      },
      "contains space");
  ExpectInvalidParam(
      [] {
        trtc_asr::ValidateSpeakerDiarization(
            trtc_asr::kSpeakerDiarizationVoiceprint, 0,
            {{"teacher", std::string("https://example.com/\x01.wav")}}, {});
      },
      "control character");
}

TEST(Params, DiarizationAllowsInternalHost) {
  // This SDK is customer-facing: internal hosts belong to the caller's own
  // network and stay fetchable for the service.
  EXPECT_NO_THROW(trtc_asr::ValidateSpeakerDiarization(
      trtc_asr::kSpeakerDiarizationVoiceprint, 0,
      {{"teacher", "http://192.168.1.10/a.wav"}}, {}));
}

TEST(Params, VadTuningValidCases) {
  EXPECT_NO_THROW(trtc_asr::ValidateVadTuning(std::nullopt, std::nullopt));
  EXPECT_NO_THROW(trtc_asr::ValidateVadTuning(0, std::nullopt));
  EXPECT_NO_THROW(trtc_asr::ValidateVadTuning(1, std::nullopt));
  EXPECT_NO_THROW(trtc_asr::ValidateVadTuning(std::nullopt, 0.0));
  EXPECT_NO_THROW(trtc_asr::ValidateVadTuning(std::nullopt, 4.0));
}

TEST(Params, VadTuningInvalidCases) {
  ExpectInvalidParam([] { trtc_asr::ValidateVadTuning(2, std::nullopt); },
                     "VadLevel must be 0");
  for (double bad : {-0.5, 4.5, std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity()}) {
    ExpectInvalidParam([bad] { trtc_asr::ValidateVadTuning(std::nullopt, bad); },
                       "NoiseThreshold must be between");
  }
}

TEST(Params, EnumOptionValidation) {
  EXPECT_NO_THROW(trtc_asr::ValidateEnumOption("InputSampleRate", 0, {0, 8000}));
  EXPECT_NO_THROW(trtc_asr::ValidateEnumOption("InputSampleRate", 8000, {0, 8000}));
  ExpectInvalidParam(
      [] { trtc_asr::ValidateEnumOption("InputSampleRate", 16000, {0, 8000}); },
      "InputSampleRate must be one of");
}

}  // namespace
