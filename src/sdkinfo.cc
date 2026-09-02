#include "sdkinfo.h"

#include "trtc_asr/signature_params.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace trtc_asr {
namespace internal {

const char* SdkPlatform() {
  // Compile-time detection: the target OS cannot change at runtime, and this
  // avoids dragging in a uname()/GetVersionEx() dependency.
#if defined(_WIN32)
  return "windows";
#elif defined(__ANDROID__)
  // Android also defines __linux__, so it must be checked first.
  return "android";
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
  return "ios";
#else
  return "mac";
#endif
#elif defined(__linux__)
  return "linux";
#else
  // Reported verbatim rather than guessed, so a new platform is visible in
  // telemetry instead of being misattributed to an existing one.
  return "unknown";
#endif
}

std::map<std::string, std::string> SdkReportParams() {
  return {
      {"platform", SdkPlatform()},
      {"sdk_lang", kSdkLanguage},
      {"sdk_type", kSdkType},
      {"version", kSdkVersion},
  };
}

std::string SdkReportQuery() {
  // std::map iterates keys in sorted order, matching the Go SDK's output.
  std::string out;
  for (const auto& [k, v] : SdkReportParams()) {
    if (!out.empty()) out += '&';
    out += k;
    out += '=';
    out += QueryEscape(v);
  }
  return out;
}

}  // namespace internal
}  // namespace trtc_asr
