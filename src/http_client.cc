#include "http_client.h"

#include <curl/curl.h>

namespace trtc_asr {
namespace internal {
namespace {

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

}  // namespace

HttpResponse HttpPost(const std::string& url, const std::string& json_body,
                      const std::vector<std::string>& headers, int timeout_seconds,
                      std::string* err) {
  HttpResponse resp;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    *err = "curl init failed";
    return resp;
  }

  struct curl_slist* header_list = nullptr;
  for (const auto& h : headers) {
    header_list = curl_slist_append(header_list, h.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  // TLS verification with system root CAs (curl default behavior; explicit
  // for clarity since the SDK talks to the Tencent Cloud endpoint).
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    *err = std::string("http request failed: ") + curl_easy_strerror(rc);
    resp.status = 0;
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  }

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return resp;
}

}  // namespace internal
}  // namespace trtc_asr
