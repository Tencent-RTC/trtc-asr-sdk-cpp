#pragma once

// Internal thin HTTP POST wrapper over libcurl.

#include <string>
#include <vector>

namespace trtc_asr {
namespace internal {

struct HttpResponse {
  long status = 0;
  std::string body;
};

/// POSTs json_body to url with the given headers. Returns the response, or
/// sets err and returns status 0 on transport failure.
HttpResponse HttpPost(const std::string& url, const std::string& json_body,
                      const std::vector<std::string>& headers, int timeout_seconds,
                      std::string* err);

}  // namespace internal
}  // namespace trtc_asr
