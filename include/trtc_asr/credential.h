#pragma once

#include <cctype>
#include <cstdint>
#include <string>

#include "trtc_asr/errors.h"

namespace trtc_asr {

inline constexpr const char* kSiteCN = "cn";
inline constexpr const char* kSiteIntl = "intl";
inline constexpr const char* kHostCN = "asr.cloud-rtc.com";
inline constexpr const char* kHostIntl = "asr-intl.cloud-rtc.com";

/// Returns the ASR hostname for site. Empty / cn is domestic; intl is
/// international. Unknown values throw ASRError (kErrInvalidParam).
inline std::string HostForSite(std::string site) {
  const std::string original = site;
  while (!site.empty() &&
         std::isspace(static_cast<unsigned char>(site.front()))) {
    site.erase(site.begin());
  }
  while (!site.empty() &&
         std::isspace(static_cast<unsigned char>(site.back()))) {
    site.pop_back();
  }
  for (char& c : site) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (site.empty() || site == kSiteCN) {
    return kHostCN;
  }
  if (site == kSiteIntl) {
    return kHostIntl;
  }
  throw ASRError(kErrInvalidParam, "unsupported site \"" + original +
                                       "\", want \"" + kSiteCN + "\" or \"" +
                                       kSiteIntl + "\"");
}

inline std::string WSEndpointForSite(const std::string& site) {
  return std::string("wss://") + HostForSite(site);
}

inline std::string HTTPEndpointForSite(const std::string& site) {
  return std::string("https://") + HostForSite(site);
}

/// Returns override when non-empty, otherwise the site-derived realtime origin.
inline std::string ResolveWSEndpoint(const std::string& override_ep,
                                     const std::string& site) {
  if (!override_ep.empty()) {
    return override_ep;
  }
  return WSEndpointForSite(site);
}

/// Returns override when non-empty, otherwise the site-derived HTTPS origin.
inline std::string ResolveHTTPEndpoint(const std::string& override_ep,
                                       const std::string& site) {
  if (!override_ep.empty()) {
    return override_ep;
  }
  return HTTPEndpointForSite(site);
}

/// Authentication information for the TRTC-ASR service.
///
/// Three values are needed:
/// - app_id: Tencent Cloud account APPID, from
///   https://console.cloud.tencent.com/cam/capi
/// - sdk_app_id: TRTC application ID, from
///   https://console.cloud.tencent.com/trtc/app
/// - secret_key: TRTC SDK secret key, from TRTC console > Application
///   Overview > SDK Key
///
/// Call set_site(kSiteIntl) to use the international cluster. The default
/// is the China site. Because Credential is copied into the recognizer,
/// set the site before constructing the recognizer.
class Credential {
 public:
  Credential(int64_t app_id, int64_t sdk_app_id, std::string secret_key)
      : app_id_(app_id), sdk_app_id_(sdk_app_id), secret_key_(std::move(secret_key)) {}

  int64_t app_id() const { return app_id_; }
  int64_t sdk_app_id() const { return sdk_app_id_; }
  const std::string& secret_key() const { return secret_key_; }

  /// Pre-computed UserSig; auto-generated when left empty.
  const std::string& user_sig() const { return user_sig_; }
  void set_user_sig(std::string user_sig) { user_sig_ = std::move(user_sig); }

  const std::string& site() const { return site_; }
  /// Selects the ASR cluster: kSiteCN (default) or kSiteIntl.
  void set_site(std::string site) { site_ = std::move(site); }

  std::string app_id_str() const { return std::to_string(app_id_); }

 private:
  int64_t app_id_;
  int64_t sdk_app_id_;
  std::string secret_key_;
  std::string user_sig_;
  std::string site_;
};

}  // namespace trtc_asr
