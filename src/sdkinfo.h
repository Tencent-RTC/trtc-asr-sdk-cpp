#pragma once

// SDK self-identification carried on every request.
//
// Every request (WebSocket handshake and HTTP API calls) reports which SDK
// language, version and OS platform produced it. Without this, a customer
// issue can only be traced to an AppID — not to the concrete client build that
// triggered it, which is what makes cross-version regressions diagnosable.
//
// The values travel as URL query parameters rather than headers because a
// browser-originated WebSocket handshake cannot set custom headers, and the
// three transports must report identically.
//
// This is an internal header: the values are a wire-protocol detail, not part
// of the public API. Consumers who need the version use trtc_asr/version.h.

#include <map>
#include <string>

#include "trtc_asr/version.h"

namespace trtc_asr {
namespace internal {

/// Released version of this SDK. Sourced from trtc_asr/version.h, the single
/// place a release has to be bumped (CMake parses the same macro).
inline constexpr const char* kSdkVersion = TRTC_ASR_VERSION_STRING;

/// Identifies the SDK implementation language.
inline constexpr const char* kSdkLanguage = "cpp";

/// Distinguishes this family of SDKs from the client-side ones. All six
/// language bindings here run server-side, so the value is constant; it exists
/// so server-side telemetry can bucket traffic the same way it does for the
/// mobile/desktop client SDKs.
inline constexpr const char* kSdkType = "server";

/// Reports the OS platform the SDK is running on, normalized to the vocabulary
/// the service expects: windows, linux, mac, android, ios. Any other platform
/// is reported verbatim so it shows up in telemetry instead of being silently
/// misattributed.
const char* SdkPlatform();

/// Returns the SDK identification parameters shared by every transport.
std::map<std::string, std::string> SdkReportParams();

/// Returns the SDK identification parameters as an encoded query fragment
/// (no leading "&"), for the transports that build their URL by string
/// concatenation.
std::string SdkReportQuery();

}  // namespace internal
}  // namespace trtc_asr
