# 更新日志

本文件记录 TRTC-ASR C++ SDK 的所有重要变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 变更

- 示例与 README 的默认引擎由 `16k_zh_en` 改为 `bigmodel`（推荐用法）；
  `engine_model_type` 为必填项，示例不再提供默认值（缺失时打印用法并以
  退出码 2 结束）；`language` 未显式指定时仅 `bigmodel` 默认补 `zh`，
  其他引擎保持服务端自动检测；参数表与引擎列表同步更新。
- 补齐 v3 的 `FileRecognizer::CreateTaskFromDataWithOptions` 与
  `SentenceRecognizer::RecognizeDataWithOptions`，与 v2 及其他语言 SDK
  的 `*WithOptions` 形态保持一致。

### 修复

- **录音文件识别（v2）词级时间戳恒为 0**：`DescribeTaskStatus` 返回的
  `ResultDetail[].Words[]` 里时间偏移字段名是 `StartTime` / `EndTime`，
  SDK 此前按 `OffsetStartMs` / `OffsetEndMs` 解析，取不到值，导致
  `SentenceWords::offset_start_ms` / `offset_end_ms` 全部为 0。现在优先读
  `StartTime` / `EndTime`，并保留 `OffsetStartMs` / `OffsetEndMs` 作为
  回退兼容（显式 0 不会被回退覆盖）。

## [1.2.1] - 2026-09-10

### 变更

- README 鉴权章节按**在线（流式）/ 离线（HTTP）**拆分，各自给出 `auth` 字段表，
  并补充 UserSig 规则：identifier 绑定（在线 `voice_id` / 离线 `request_id`）、
  自动签名有效期 86400 秒且每条连接 / 每次请求重新生成、调用 `credential.set_user_sig()`
  传入固定签名后不再刷新、签名与站点（`credential.set_site(kSiteIntl)`）绑定。
  中英文 README 同步。

## [1.2.0] - 2026-09-09

### 新增

- 新增 v3 协议客户端，位于独立命名空间 `trtc_asr::v3`（`include/trtc_asr/v3.h`），与 v2/v1 客户端（`trtc_asr::` 顶层 API）完全解耦，两者可独立选用、互不影响：
  - `v3::SpeechRecognizer`：WebSocket `/asr/v3`，URL 仅携带 `voice_id`，鉴权与识别参数通过首帧 JSON（`{"type":"start","auth":{...},"params":{...}}`）下发；`Start()` 同步等待服务端 ack，鉴权失败（4002）/参数非法（4001）等错误同步抛出
  - `v3::SentenceRecognizer`：`POST /v3/transcribe`，body 为 `{auth, params}` 分块，请求/响应均为 snake_case 扁平结构（无 `Response` 外壳）
  - `v3::FileRecognizer`：`POST /v3/create_transcription` + `/v3/describe_transcription`，任务 ID 为 `transcription_id`（与 v1 `RecTaskId` 不通用）
  - `v3::NewCredential(sdk_app_id, secret_key)`：v3 不再需要腾讯云 AppID
  - 服务端数字错误码（4xxx/5xxx）直接作为 `ASRError::code()` 抛出，与 SDK 本地错误码（10xx）区间不冲突；离线错误一律以响应 body 的 `code` 为准（鉴权失败也是 HTTP 200）
  - `needvad`/`convert_num_mode` 显式传 0 会真正下发（v2 query 传参会吞掉 0 值）；说话人分离的 `speaker_roles` 元素序列化为 snake_case（`role_name`/`audio_url`），声纹 ID 列表为 `voiceprint_ids`
  - v3 新增能力：`word_with_space`、`context`（识别上下文 text/terms/general）；录音文件支持 `audio_urls` 分布式录音
  - 下行消息结构与 v2 完全一致，listener 类（`trtc_asr::SpeechRecognitionListener`）两版共用
  - 使用前提：服务端已为对应 SDKAppID 开启 `EnableV3Route` 灰度
  - 注：协议中服务端内部的 `business` 灰度字段不属于公开 API，SDK 不暴露、不发送
- 新增 `examples/v3_realtime_asr.cc` / `v3_sentence_asr.cc` / `v3_file_asr.cc` 示例
- README 改为只承载 v3 协议文档；v2 / v1 协议与客户端说明移至 `docs/v2_protocol.md`

### 变更

- 仓库迁移至 `github.com/Tencent-RTC/trtc-asr-sdk-cpp`，功能与 API 无任何变化。
  旧仓库保留 `v1.0.0` 并归档，不再更新。

### 修复

- `src/usersig.cc` 补充 `#include <ctime>`：`std::time` 不再依赖传递包含，
  修复 GCC/libstdc++（Linux）下的编译失败。

## [1.0.0] - 2026-09-02

首个正式版本。

### 新增

- Credential 可通过 `set_site` 选择国内站（默认，`asr.cloud-rtc.com`）或国际站（`asr-intl.cloud-rtc.com`），三个识别器共用（须在构造识别器之前设置）
- 实时语音识别（WebSocket），支持流式写入与优雅停止
- 一句话识别（HTTP）
- 录音文件识别（异步 HTTP，CreateRecTask + DescribeTaskStatus）
- 说话人分离：匿名聚类与声纹角色认证两种模式
- VAD 调优、热词、自定义语言模型、脏词/语气词/标点过滤等识别参数
- 所有请求上报 SDK 自身标识（`platform` / `sdk_lang` / `sdk_type` / `version`），
  便于服务端按语言、版本、平台定位客户问题
- `install()` 与 export 规则：安装公共头文件、库、CMake 包配置和 pkg-config 文件，
  支持 `find_package(trtc_asr)` 与 `trtc_asr::trtc_asr` 目标
- `scripts/package.sh`：产出 `trtc-asr-sdk-cpp-<版本>-<系统>-<架构>-{static,shared}.tar.gz`。
  打包前会用独立最小工程通过 `find_package` 链接并运行产物，验证交付完整可用
- `TRTC_ASR_BUILD_SHARED` 选项，支持构建动态库（带 `SOVERSION`）
- `include/trtc_asr/version.h`，提供 `TRTC_ASR_VERSION_STRING` 等编译期版本宏
- MIT LICENSE
- GitHub Actions CI：Linux/macOS × Debug/Release 的构建与测试，外加两个平台上的
  打包验证与产物上传

### 变更

- `TRTC_ASR_BUILD_EXAMPLES` / `TRTC_ASR_BUILD_TESTS` / `TRTC_ASR_INSTALL` 改为
  仅在本工程为顶层项目时默认开启。此前通过 `add_subdirectory` 集成 SDK 的调用方
  会被迫下载 googletest 并编译示例程序

### 修复

- `include/trtc_asr/credential.h` 使用了 `int64_t` 却未包含 `<cstdint>`，
  此前依赖标准库的传递包含才能编译通过
