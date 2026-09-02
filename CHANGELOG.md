# 更新日志

本文件记录 TRTC-ASR C++ SDK 的所有重要变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

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
