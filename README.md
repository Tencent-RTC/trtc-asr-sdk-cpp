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

构建选项（作为顶层项目时示例与测试默认开启，被 `add_subdirectory` 引入时默认关闭）：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `TRTC_ASR_BUILD_SHARED` | `OFF` | 构建动态库而非静态库 |
| `TRTC_ASR_BUILD_EXAMPLES` | 顶层为 `ON` | 构建示例程序 |
| `TRTC_ASR_BUILD_TESTS` | 顶层为 `ON` | 构建测试（会下载 googletest） |
| `TRTC_ASR_INSTALL` | 顶层为 `ON` | 生成 install 规则 |

## 集成

### 方式一：源码集成（推荐）

```cmake
add_subdirectory(path/to/trtc-asr-sdk-cpp)
target_link_libraries(your_app PRIVATE trtc_asr::trtc_asr)
```

### 方式二：安装后 find_package

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j
cmake --install build
```

```cmake
find_package(trtc_asr 0.1 REQUIRED)
target_link_libraries(your_app PRIVATE trtc_asr::trtc_asr)
```

也提供 pkg-config：`pkg-config --cflags --libs trtc_asr`。

### 方式三：使用预编译包

```bash
./scripts/package.sh              # 同时产出静态与动态包
./scripts/package.sh --static     # 仅静态
./scripts/package.sh --shared --output /tmp/out
```

脚本会在 `dist/` 下生成
`trtc-asr-sdk-cpp-<版本>-<系统>-<架构>-<static|shared>.tar.gz`，解包后目录结构为：

```
include/trtc_asr/*.h          公共头文件
lib/libtrtc_asr.{a,so,dylib}  库文件
lib/cmake/trtc_asr/           CMake 包配置，供 find_package 使用
lib/pkgconfig/trtc_asr.pc     pkg-config 配置
share/doc/trtc-asr-sdk-cpp/   README
```

使用时把解包目录传给 `CMAKE_PREFIX_PATH` 即可：

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/trtc-asr-sdk-cpp-0.1.0-linux-x86_64-static
```

打包脚本在生成压缩包前，会用一个独立的最小工程通过 `find_package` 链接并运行产物，
确保头文件、导出目标和依赖传递都是完整可用的。

### 二进制兼容性说明

公共接口使用了 `std::string`、`std::vector`、`std::optional` 等标准库类型，因此预编译
产物与调用方需使用**同一套编译器与标准库**（相同的 libstdc++/libc++ ABI、相同的
C++ 标准）。跨编译器场景请使用源码集成方式。

SDK 版本可在编译期通过 `trtc_asr/version.h` 的 `TRTC_ASR_VERSION_STRING` 获取，
CMake 工程版本与打包产物版本均以该头文件为唯一来源。

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
// credential.set_site(trtc_asr::kSiteIntl); // 国际站；须在构造识别器之前调用
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
