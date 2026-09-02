#pragma once

#include <cstdint>
#include <string>

namespace trtc_asr {

/// Default UserSig validity: 180 days in seconds (matches the Go SDK).
inline constexpr int64_t kDefaultExpire = 86400 * 180;

/// Generates a TRTC UserSig (TLS sig API v2 compatible).
///
/// Layout of the ticket (identical to the official implementations):
/// 1. Build a JSON document {"TLS.ver":"2.0","TLS.identifier":..,
///    "TLS.sdkappid":..,"TLS.expire":..,"TLS.time":..,"TLS.sig":..} where
///    TLS.sig is the standard base64 of the HMAC-SHA256 of
///    "TLS.identifier:<id>\nTLS.sdkappid:<appid>\nTLS.time:<now>\nTLS.expire:<expire>\n"
///    keyed by the SDK secret key.
/// 2. zlib-compress the JSON document.
/// 3. Encode with the Tencent variant of base64url: alphabet A-Za-z0-9*-,
///    padding _ (i.e. +→*, /→-, =→_).
///
/// - sdk_app_id: TRTC application ID
/// - key: TRTC SDK secret key
/// - user_id: unique user identifier (maps to voice_id in ASR)
/// - expire: signature validity in seconds; 0 uses kDefaultExpire
///
/// Throws ASRError on invalid input or crypto failures.
std::string GenUserSig(int64_t sdk_app_id, const std::string& key,
                       const std::string& user_id, int64_t expire);

/// Deterministic core of GenUserSig with an explicit timestamp, exposed for
/// tests and for callers that need reproducible signatures.
std::string GenUserSigAt(int64_t sdk_app_id, const std::string& key,
                         const std::string& user_id, int64_t expire, int64_t now);

/// Encodes bytes with the Tencent base64url variant used by UserSig.
std::string Base64UrlEncode(const uint8_t* data, size_t len);
std::string Base64UrlEncode(const std::string& data);

/// Decodes the Tencent base64url variant. Provided for tooling/tests.
/// Throws ASRError on malformed input.
std::string Base64UrlDecode(const std::string& s);

/// Standard base64 encode/decode helpers used across the SDK.
std::string Base64Encode(const uint8_t* data, size_t len);
std::string Base64Decode(const std::string& s);

}  // namespace trtc_asr
