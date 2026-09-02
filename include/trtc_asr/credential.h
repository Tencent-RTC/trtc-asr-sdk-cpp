#pragma once

#include <string>

namespace trtc_asr {

/// Authentication information for the TRTC-ASR service.
///
/// Three values are needed:
/// - app_id: Tencent Cloud account APPID, from
///   https://console.cloud.tencent.com/cam/capi
/// - sdk_app_id: TRTC application ID, from
///   https://console.cloud.tencent.com/trtc/app
/// - secret_key: TRTC SDK secret key, from TRTC console > Application
///   Overview > SDK Key
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

  std::string app_id_str() const { return std::to_string(app_id_); }

 private:
  int64_t app_id_;
  int64_t sdk_app_id_;
  std::string secret_key_;
  std::string user_sig_;
};

}  // namespace trtc_asr
