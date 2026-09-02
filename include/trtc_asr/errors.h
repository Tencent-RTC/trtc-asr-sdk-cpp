#pragma once

#include <stdexcept>
#include <string>

namespace trtc_asr {

/// Error codes for TRTC-ASR SDK.
inline constexpr int kErrInvalidParam = 1001;
inline constexpr int kErrConnectFailed = 1002;
inline constexpr int kErrWriteFailed = 1003;
inline constexpr int kErrReadFailed = 1004;
inline constexpr int kErrAuthFailed = 1005;
inline constexpr int kErrTimeout = 1006;
inline constexpr int kErrServerError = 1007;
inline constexpr int kErrAlreadyStarted = 1008;
inline constexpr int kErrNotStarted = 1009;
inline constexpr int kErrAlreadyStopped = 1010;

/// An error returned by the TRTC-ASR service or the SDK itself.
class ASRError : public std::exception {
 public:
  ASRError(int code, std::string message)
      : code_(code),
        message_(std::move(message)),
        full_("trtc-asr error [" + std::to_string(code) + "]: " + message_) {}

  int code() const noexcept { return code_; }
  /// Raw message without the "trtc-asr error [code]:" prefix.
  const std::string& message() const noexcept { return message_; }
  const char* what() const noexcept override { return full_.c_str(); }

 private:
  int code_;
  std::string message_;
  std::string full_;
};

}  // namespace trtc_asr
