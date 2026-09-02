# TRTC-ASR C++ SDK

基于 TRTC 鉴权体系的语音识别（ASR）C++ SDK，与 [Go SDK](../trtc-asr-sdk-go) 行为对齐，支持实时语音识别（WebSocket）、一句话识别（HTTP）和录音文件识别（异步 HTTP）三种模式。

## 前提条件

1. **获取腾讯云 APPID** — 在 [CAM API 密钥管理](https://console.cloud.tencent.com/cam/capi) 页面查看
2. **创建 TRTC 应用** — 在 [实时音视频控制台](https://console.cloud.tencent.com/trtc/app) 创建应用，获取 `SDKAppID`
3. **获取 SDK 密钥** — 在应用概览页点击「SDK密钥」查看，即用于计算 UserSig 的加密密钥

协议细节（WebSocket 参数、响应字段、说话人分离）与 Go SDK 完全一致，参见 [Go SDK README](../trtc-asr-sdk-go/README.md)。

## 构建

要求：CMake ≥ 3.16、C++17 编译器、OpenSSL、zlib、libcurl。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

产物：
- `build/libtrtc_asr.a` — SDK 静态库（公共头文件在 `include/trtc_asr/`）
- `build/realtime_asr` / `sentence_asr` / `file_asr` — 示例程序
- `build/trtc_asr_tests` — 测试（googletest 由 CMake FetchContent 自动拉取）

集成方式（CMake）：

```cmake
add_subdirectory(path/to/trtc-asr-sdk-cpp)
target_link_libraries(your_app PRIVATE trtc_asr)
```

## 快速开始

### 实时语音识别

```cpp
#include "trtc_asr/speech_recognizer.h"

class Printer : public trtc_asr::SpeechRecognitionListener {
 public:
  void OnSentenceEnd(const trtc_asr::SpeechRecognitionResponse& r) override {
    std::cout << "[end] " << r.result.voice_text_str << "\n";
    for (const auto& seg : r.result.speaker_segments) {
      std::string name = seg.speaker_name.empty()
          ? "spk" + std::to_string(seg.speaker_id) : seg.speaker_name;
      std::cout << "  [" << name << "] " << seg.text << "\n";
    }
  }
  void OnFail(const trtc_asr::SpeechRecognitionResponse*,
              const trtc_asr::ASRError& e) override {
    std::cerr << "[fail] " << e.what() << "\n";
  }
};

trtc_asr::Credential credential(app_id, sdk_app_id, "your-sdk-secret-key");
Printer listener;
trtc_asr::SpeechRecognizer recognizer(credential, "16k_zh", &listener);

// 可选配置（全部在 Start 前调用）：
// recognizer.SetHotwordList("词1|5,词2|11");
// recognizer.SetSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster);
// recognizer.SetWordInfo(1);
// recognizer.SetNoiseThreshold(1.5);   // VAD 噪声微调（0.0-4.0）

recognizer.Start();                  // 连接 WebSocket
recognizer.Write(pcm.data(), n);     // 发送音频（PCM）
recognizer.Stop();                   // 发送结束信号并等待最终结果
```

所有 API 失败时抛出 `trtc_asr::ASRError`（`code()` 与 Go SDK 错误码一致，1001-1010，服务端错误码原样透传）。

### 一句话识别

```cpp
#include "trtc_asr/sentence_recognizer.h"

trtc_asr::SentenceRecognizer recognizer(credential);
auto result = recognizer.RecognizeData(pcm_bytes, "pcm", "16k_zh_en");
// result.result / result.audio_duration / result.word_list
// 或从 URL：recognizer.RecognizeURL("https://example.com/a.wav", "wav", "16k_zh_en");
```

### 录音文件识别

```cpp
#include "trtc_asr/file_recognizer.h"

trtc_asr::FileRecognizer recognizer(credential);
std::string task_id = recognizer.CreateTaskFromData(pcm_bytes, "pcm", "16k_zh_en");
auto status = recognizer.WaitForResult(task_id);  // 默认 1s 轮询，10min 超时
// status.result / status.result_detail（句级 + 词级时间戳 + 说话人）
// 或从 URL（≤1GB / ≤12h）：recognizer.CreateTaskFromURL(url, "16k_zh_en");
```

## 设计说明

- **生命周期**：`SpeechRecognizer` 单例使用——stopped 后不可重启；回调在 SDK 内部 reader 线程上顺序派发；`Stop()` 可在回调中安全调用（通过 reader 线程 ID 检测重入，非终态回调里发送 end 后立即返回，看门狗线程兜底超时强关）。
- **并发安全**：运行时状态（连接、写锁、done 条件变量、终态标记）收拢在 `internal::RecognizerSharedState` 里由 `shared_ptr` 共享——reader 线程与看门狗线程持有的是状态而非 `this`，识别器析构不会造成悬垂访问。锁分层：连接锁（临界区不含网络 I/O）+ 写锁（串行化音频帧与 end 信号）。
- **三态参数**：`vad_level` / `noise_threshold` / `filter_empty_result` 用 `std::optional` 区分「显式传 0」与「不配置」。
- **UserSig**：内置 TLS sig API v2 兼容实现（SHA-256/HMAC-SHA256 自带实现、zlib 压缩、腾讯 base64url 变体），不依赖 OpenSSL 的已弃用低层 API。
- **WebSocket**：内置最小 RFC 6455 客户端（ws/wss，TLS 走 OpenSSL 系统根证书 + 主机名校验；客户端帧按规范 mask；ping 自动应答 pong）。
- **HTTP**：libcurl，强制 TLS 证书校验。

## 测试

```bash
cmake -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
# 或直接运行：./build/trtc_asr_tests
```

67 个测试全部在本机回环 mock 服务器上运行（无需真实凭证/网络；HTTP 与 WebSocket mock 均为测试内置的最小实现，WS 复用 SDK 的帧编解码）：

- `tests/usersig_test.cc` — UserSig 结构/zlib/base64url 往返，HMAC-SHA256 与 OpenSSL EVP_MAC 交叉校验
- `tests/signature_test.cc` — URL query 构建（说话人分离、VAD 三态、转义、排序）
- `tests/params_test.cc` — 参数校验（声纹 URL、VAD 范围含 NaN/Inf、枚举）
- `tests/sentence_recognizer_test.cc` / `file_recognizer_test.cc` — mock HTTP：请求头/query/body 断言、错误路径、轮询、超时
- `tests/speech_recognizer_test.cc` — mock WebSocket：握手鉴权参数、ack 帧不误派 sentence begin、服务端错误/final/终态状态机、回调重入 Stop、监听器异常恢复、Stop 超时强关、并发写+Stop 不死锁

## 示例

```bash
export TRTC_ASR_APP_ID=13xxxxxxxx
export TRTC_ASR_SDK_APP_ID=14xxxxxxxx
export TRTC_ASR_SECRET_KEY=your-sdk-secret-key

./build/realtime_asr path/to/audio.pcm [16k_zh_en]
./build/sentence_asr path/to/audio.pcm pcm 16k_zh_en
./build/file_asr path/to/audio.pcm
./build/file_asr -u https://example.com/audio.wav
```

## License

MIT License
