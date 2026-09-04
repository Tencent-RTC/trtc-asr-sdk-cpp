# TRTC-ASR C++ SDK

基于 TRTC 鉴权体系的语音识别（ASR）C++ SDK，支持实时语音识别（WebSocket）、一句话识别（HTTP）和录音文件识别（异步 HTTP）三种模式。

> 其他语言 SDK：[Go](https://github.com/Tencent-RTC/trtc-asr-sdk-go) | [Python](https://github.com/Tencent-RTC/trtc-asr-sdk-python) | [Node.js](https://github.com/Tencent-RTC/trtc-asr-sdk-nodejs) | [Java](https://github.com/Tencent-RTC/trtc-asr-sdk-java) | [Rust](https://github.com/Tencent-RTC/trtc-asr-sdk-rust)

## 前提条件

使用本 SDK 前，您需要准备三个凭证：`AppID`、`SDKAppID`、`SecretKey`。国内站与国际站的账号体系不同，请按您的站点参照官方快速接入指南完成注册、创建应用与服务开通：

- **国内站**：[快速接入指南](https://xai.cloud-rtc.com/#gettingStarted) — 注册腾讯云账号并完成实名认证 → 在 [TRTC 控制台](https://console.cloud.tencent.com/trtc/app)创建应用 → 开通「AI 智能识别」（体验版可免费试用）
- **国际站**：[Quick Start](https://xai-intl.cloud-rtc.com/#gettingStarted) — 在 [trtc.io](https://www.trtc.io) 注册（自动开通 Tencentcloud 账号，无需实名认证）→ 在 [console.trtc.io](https://console.trtc.io) 创建应用 → 开通「AI Speech Recognition」（仅 RTC Engine Lite 及以上包月套餐，Free Trial 不支持）

> **注意**：国际站的 `AppID` 不在 trtc.io 控制台显示，需在 Tencentcloud 控制台「账号信息」页查看（头像 → Account Information）；`SDKAppID` 与 `SecretKey` 均在应用详情页获取。

## 协议说明

### WebSocket 连接

- **连接地址**：
  - 国内站：`wss://asr.cloud-rtc.com/asr/v2/<appid>?{请求参数}`
  - 国际站：`wss://asr-intl.cloud-rtc.com/asr/v2/<appid>?{请求参数}`（`credential.set_site(trtc_asr::kSiteIntl)`，须在构造识别器之前调用）

其中 `<appid>` 为腾讯云账号的 APPID，国内站可通过 [API 密钥管理页面](https://console.cloud.tencent.com/cam/capi) 获取，国际站见 Tencentcloud 控制台「账号信息」。

### 鉴权方式

鉴权信息携带在 URL query 参数中（浏览器原生 WebSocket 无法自定义 header，因此走 query 传递）：

| 参数 | 说明 |
|------|------|
| `sdkappid` | TRTC 应用 ID，从 TRTC 控制台获取（[国内站](https://console.cloud.tencent.com/trtc/app) / [国际站](https://console.trtc.io)） |
| `usersig` | TRTC 签名，[计算文档](https://cloud.tencent.com/document/product/647/17275)，UserID 等于 `voice_id` |

两者均由 SDK 自动填充，用户无需关心。

### 请求参数

| 参数 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `secretid` | 是 | String | SDK 内部自动用 APPID 填充 |
| `sdkappid` | 是 | Integer | TRTC 应用 ID，SDK 内部自动填充 |
| `usersig` | 是 | String | TRTC 签名，SDK 内部自动生成（值与 `signature` 一致） |
| `timestamp` | 是 | Integer | 当前 UNIX 时间戳（秒） |
| `expired` | 是 | Integer | 签名有效期截止时间戳，必须大于 timestamp |
| `nonce` | 是 | Integer | 随机正整数，最长10位 |
| `engine_model_type` | 是 | String | 引擎类型：`8k_zh`(中文电话)、`16k_zh`(中文通用)、`16k_zh_en`(中英文) |
| `voice_id` | 是 | String | 音频流全局唯一标识（推荐 UUID），最长128位 |
| `voice_format` | 否 | Integer | 语音编码：`1` PCM（默认） |
| `needvad` | 否 | Integer | `0` 关闭 VAD，`1` 开启（默认） |
| `hotword_id` | 否 | String | 热词表 ID |
| `hotword_list` | 否 | String | 临时热词列表：`词1|权重1,词2|权重2`（单词 ≤30 字节，权重 1-11 或 100） |
| `customization_id` | 否 | String | 自学习模型 ID |
| `replace_text_id` | 否 | String | 替换词表 ID |
| `filter_dirty` | 否 | Integer | 过滤脏词：`0` 不过滤，`1` 过滤，`2` 替换为 * |
| `filter_modal` | 否 | Integer | 过滤语气词：`0` 不过滤，`1` 部分，`2` 严格 |
| `filter_punc` | 否 | Integer | 过滤句末句号：`0` 不过滤，`1` 过滤 |
| `filter_empty_result` | 否 | Integer | 空结果回调：`0` 回调，`1` 不回调（服务端默认） |
| `convert_num_mode` | 否 | Integer | 数字转换：`0` 不转，`1` 智能转换（默认），`3` 数学转换 |
| `word_info` | 否 | Int | 显示词级时间：`0` 不显示，`1` 显示，`2` 含标点 |
| `vad_silence_time` | 否 | Integer | 静音断句阈值（ms），范围 240-2000，默认 800 |
| `vad_level` | 否 | Integer | VAD 场景档：`0` 高召回，`1` 远场过滤（服务端默认） |
| `noise_threshold` | 否 | Float | VAD 噪声微调，范围 `0.0-4.0`；设置后覆盖 `vad_level` 档位 |
| `max_speak_time` | 否 | Integer | 强制断句时间（ms），范围 5000-90000，默认 60000 |
| `input_sample_rate` | 否 | Integer | 输入 PCM 采样率，仅支持 `8000`（8k 音频喂 16k 引擎） |
| `speaker_diarization` | 否 | Integer | 说话人分离：`0` 关闭（默认），`1` 匿名聚类，`3` 声纹角色认证 |
| `speaker_number` | 否 | Integer | 说话人数量提示（分离开启时生效，用于在线聚类）；`0` 自动检测 |
| `speaker_roles` | 否 | String | 临时声纹角色 JSON 数组，仅 `speaker_diarization=3`，如 `[{"RoleName":"teacher","AudioUrl":"https://.../a.wav"}]` |
| `voiceprintids` | 否 | String | 已注册声纹 ID JSON 数组，仅 `speaker_diarization=3` |
| `language` | 否 | String | 指定识别语言（如 `zh`、`en`），留空为自动检测 |
| `signature` | 是 | String | 接口签名参数，值与 `usersig` 一致 |

### 实时识别响应

| 字段 | 类型 | 说明 |
|------|------|------|
| `code` / `message` | Integer / String | 错误码与提示，`0` 表示成功 |
| `voice_id` / `message_id` | String | 音频流 ID / 单条消息 ID |
| `final` | Integer | `1` 表示会话结束包 |
| `result.slice_type` | Integer | `0` 句子开始，`1` 中间结果，`2` 句末稳定结果 |
| `result.index` | Integer | 句子序号 |
| `result.start_time` / `end_time` | Integer | 当前结果起止时间（ms） |
| `result.voice_text_str` | String | 当前结果文本 |
| `result.word_size` / `word_list` | Integer / Array | 词级（字级）时间戳，需 `word_info != 0` |
| `result.speaker_segments` | Array | 说话人分段，开启说话人分离后返回 |
| `result.language` | String | 识别语言（引擎上报时） |
| `result.finish_silence_ms` | Integer | 触发断句的尾部静音时长（ms） |
| `result.last_token_runtime_ms` | Integer | 末字服务端解码耗时（ms） |

### 说话人分离（实时）

开启 `speaker_diarization` 后，说话人归属通过两个入口返回：

- `result.speaker_segments[]`：**推荐入口**。一个 `result` 可能包含多个说话人，句子级归属天然有歧义，因此协议按说话人切段返回。`len(speaker_segments) == 1` 即为单说话人句。
- `result.word_list[].speaker_id`：字级归属，需同时设置 `word_info != 0`。

`speaker_id` 语义：会话内有效，从 `1` 开始编号，`-1` 表示未知，`0` 为保留值。

`speaker_segments[]` 字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `speaker_id` | Integer | 说话人编号 |
| `speaker_name` | String | 角色名，仅 `speaker_diarization=3` 命中注册声纹时返回，等于请求侧 `RoleName` |
| `start_time` / `end_time` | Integer | 该分段起止时间（ms） |
| `text` | String | 该分段文本 |
| `word_start` / `word_end` | Integer | 对应 `word_list` 的闭区间下标，即 `word_list[word_start:word_end+1]`；`word_info=0` 时不返回 |
| `stable_flag` | Integer | 该分段是否稳定：`1` 稳定，`0` 非稳定 |

C++ 用法示例：

```cpp
trtc_asr::SpeechRecognizer recognizer(credential, "16k_zh", &listener);
recognizer.SetWordInfo(1);  // 需要字级说话人时开启
recognizer.SetSpeakerDiarization(trtc_asr::kSpeakerDiarizationCluster);  // 1：匿名聚类

// 声纹角色认证（返回角色名）：
// recognizer.SetSpeakerDiarization(trtc_asr::kSpeakerDiarizationVoiceprint); // 3
// recognizer.SetSpeakerRoles({{"teacher", "https://example.com/teacher.wav"}});
// recognizer.SetVoiceprintIds({"vp-1"});
// recognizer.SetSpeakerNumber(2); // 0 = 自动检测

void OnSentenceEnd(const trtc_asr::SpeechRecognitionResponse& r) override {
  for (const auto& seg : r.result.speaker_segments) {
    std::string name = seg.speaker_name.empty()
        ? "spk" + std::to_string(seg.speaker_id) : seg.speaker_name;
    std::cout << "[" << name << "] " << seg.text << "\n";
  }
}
```

### VAD 调优（noise_threshold / vad_level）

| 方法 | 取值 | 说明 |
|------|------|------|
| `SetVadLevel(level)` | `0` / `1` | `0` 高召回，`1` 远场过滤（服务端默认） |
| `SetNoiseThreshold(v)` | `0.0` - `4.0` | 噪声抑制微调，值越大抑制越强、召回越低；设置后覆盖 `vad_level` 档位 |
| `SetVadSilenceTime(ms)` | 240 - 2000 | 静音断句阈值 |

两者都是三态语义：**只有显式调用 setter 才会下发**，因此显式传 `0` 与「不配置」可以区分（服务端 `vad_level` 默认是 `1`）。超出范围会在 `Start()` 阶段本地报错，不会浪费一次连接。

### 一句话识别接口

- **请求地址**：
  - 国内站：`https://asr.cloud-rtc.com/v1/SentenceRecognition?{请求参数}`
  - 国际站：`https://asr-intl.cloud-rtc.com/v1/SentenceRecognition?{请求参数}`
- **请求方法**：HTTP POST，Content-Type 为 `application/json; charset=utf-8`

#### 鉴权方式

HTTP 接口的鉴权信息携带在请求 Header 中（与流式不同，不走 query）：

| Header | 说明 |
|--------|------|
| `X-TRTC-SdkAppId` | TRTC 应用 ID，从 TRTC 控制台获取（[国内站](https://console.cloud.tencent.com/trtc/app) / [国际站](https://console.trtc.io)） |
| `X-TRTC-UserSig` | TRTC 签名，UserID 等于 URL 参数中的 `RequestId`（SDK 内部自动生成） |

#### URL 请求参数

| 参数 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `AppId` | 是 | String | 腾讯云 APPID |
| `Secretid` | 是 | String | SDK 内部自动用 APPID 填充 |
| `RequestId` | 是 | String | 全局请求唯一 ID（UUID），用于生成 UserSig |
| `Timestamp` | 是 | Integer | 当前 UNIX 时间戳（秒） |

#### 请求体参数（JSON）

| 参数 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `EngSerViceType` | 是 | String | 引擎类型：`16k_zh`(中文)、`16k_zh_en`(中英文) |
| `SourceType` | 是 | Integer | `0` URL 上传、`1` 本地数据（base64） |
| `VoiceFormat` | 是 | String | 音频格式：`wav`、`pcm`、`ogg-opus`、`mp3`、`m4a` |
| `Data` | 条件 | String | base64 编码的音频数据（SourceType=1 时必填） |
| `DataLen` | 条件 | Integer | 音频数据原始长度（SourceType=1 时必填） |
| `Url` | 条件 | String | 音频 URL（SourceType=0 时必填） |
| `WordInfo` | 否 | Integer | 词级时间：`0` 不显示、`1` 显示、`2` 含标点 |
| `FilterDirty` | 否 | Integer | 脏词过滤：`0` 不过滤、`1` 过滤、`2` 替换 |
| `FilterModal` | 否 | Integer | 语气词过滤：`0` 不过滤、`1` 部分、`2` 严格 |
| `FilterPunc` | 否 | Integer | 标点过滤：`0` 不过滤、`2` 过滤全部 |
| `ConvertNumMode` | 否 | Integer | 数字转换：`0` 不转、`1` 智能转换（默认） |
| `HotwordId` | 否 | String | 热词表 ID |
| `HotwordList` | 否 | String | 临时热词列表 |
| `CustomizationId` | 否 | String | 自学习模型 ID |
| `InputSampleRate` | 否 | Integer | PCM 输入采样率（仅 PCM 格式，支持 8000） |
| `Language` | 否 | String | 指定识别语言，留空为自动检测 |

**限制**：音频时长 ≤ 60s，文件大小 ≤ 3MB，单账号并发 ≤ 30次/秒

### 录音文件识别接口

录音文件识别是异步接口，适用于较长音频（≤12h）。工作流程为：提交任务 → 轮询结果。

#### 创建任务：CreateRecTask

- **请求地址**：
  - 国内站：`https://asr.cloud-rtc.com/v1/CreateRecTask?{请求参数}`
  - 国际站：`https://asr-intl.cloud-rtc.com/v1/CreateRecTask?{请求参数}`
- **请求方法**：HTTP POST，Content-Type 为 `application/json; charset=utf-8`
- **并发限制**：默认 20次/秒

鉴权方式（Header 中的 `X-TRTC-SdkAppId` / `X-TRTC-UserSig`）与 URL 请求参数（AppId、Secretid、RequestId、Timestamp）均与一句话识别相同。

##### 请求体参数（JSON）

| 参数 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `EngineModelType` | 是 | String | 引擎类型：`16k_zh`(中文)、`16k_zh_en`(中英文) |
| `ChannelNum` | 是 | Integer | 声道数：`1` 单声道；`2` 双声道（8k 电话，自动区分说话人并返回 `ChannelId`：1=左/2=右） |
| `ResTextFormat` | 是 | Integer | 结果格式：`0` 基础、`1` 含词级时间、`2` 含标点时间 |
| `SourceType` | 是 | Integer | `0` URL 上传、`1` 本地数据（base64） |
| `Url` | 条件 | String | 音频 URL（SourceType=0，时长≤12h，大小≤1GB） |
| `Data` | 条件 | String | base64 编码音频数据（SourceType=1，大小≤5MB） |
| `DataLen` | 条件 | Integer | 音频数据原始长度（SourceType=1） |
| `CallbackUrl` | 否 | String | 回调 URL，任务完成后 POST 结果 |
| `FilterDirty` | 否 | Integer | 脏词过滤 |
| `FilterModal` | 否 | Integer | 语气词过滤 |
| `FilterPunc` | 否 | Integer | 标点过滤 |
| `ConvertNumMode` | 否 | Integer | 数字转换 |
| `HotwordId` | 否 | String | 热词表 ID |
| `HotwordList` | 否 | String | 临时热词列表 |
| `CustomizationId` | 否 | String | 自学习模型 ID |
| `ReplaceTextId` | 否 | String | 替换词表 ID |
| `Language` | 否 | String | 指定识别语言，留空为自动检测 |
| `SpeakerDiarization` | 否 | Integer | 说话人分离：`0` 关闭（默认），`1` 匿名聚类，`3` 声纹角色认证 |
| `SpeakerNumber` | 否 | Integer | 说话人数量提示，`0` 自动检测 |
| `SpeakerRoles` | 否 | Array | 临时声纹角色，元素含 `RoleName` 与 `AudioUrl`，仅 `SpeakerDiarization=3` |
| `VoiceprintIds` | 否 | Array | 已注册声纹 ID 列表，仅 `SpeakerDiarization=3` |
| `VadSilenceMs` | 否 | Integer | 静音断句阈值（ms） |
| `VadLevel` | 否 | Integer | VAD 场景档：`0` 高召回（默认），`1` 远场过滤 |
| `NoiseThreshold` | 否 | Float | VAD 噪声微调，范围 `0.0-4.0`；设置后覆盖 `VadLevel` 档位 |

> `VadLevel` / `NoiseThreshold` 在 C++ 里是 `std::optional`，因为 `0` 是合法取值，用 optional 才能区分「显式传 0」与「不配置」。

##### 响应

返回 `RecTaskId`（任务 ID），用于后续查询。任务有效期 24 小时。

#### 查询结果：DescribeTaskStatus

- **请求地址**：
  - 国内站：`https://asr.cloud-rtc.com/v1/DescribeTaskStatus?{请求参数}`
  - 国际站：`https://asr-intl.cloud-rtc.com/v1/DescribeTaskStatus?{请求参数}`
- **请求方法**：HTTP POST
- **并发限制**：默认 50次/秒

##### 请求体参数（JSON）

| 参数 | 必填 | 类型 | 说明 |
|------|------|------|------|
| `RecTaskId` | 是 | String | CreateRecTask 返回的任务 ID |

##### 响应（TaskStatus）

| 字段 | 类型 | 说明 |
|------|------|------|
| `RecTaskId` | String | 任务 ID |
| `Status` | Integer | `0` 等待、`1` 执行中、`2` 成功、`3` 失败 |
| `StatusStr` | String | waiting / executing / success / failed |
| `Progress` | Integer | 处理进度（0-100） |
| `Result` | String | 识别结果文本 |
| `ErrorMsg` | String | 失败原因 |
| `ResultDetail` | Array | 句级详细结果（含词级时间偏移） |
| `AudioDuration` | Float | 音频时长（秒） |

`ResultDetail[]` 中与说话人相关的字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `SpeakerId` | Integer | 说话人编号，开启 `SpeakerDiarization` 后返回 |
| `SpeakerRoleName` | String | 角色名，`SpeakerDiarization=3` 命中注册声纹时返回 |
| `ChannelId` | Integer | 双声道（`ChannelNum=2`）时的声道编号：1=左、2=右；此场景下优先用它区分说话人 |
| `Language` | String | 该句识别语言（引擎上报时） |

---

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
find_package(trtc_asr 1.0 REQUIRED)
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
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/trtc-asr-sdk-cpp-1.0.0-linux-x86_64-static
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
