#!/usr/bin/env bash
#
# Builds redistributable binary packages of the TRTC ASR C++ SDK.
#
# Each package contains the public headers, the library, a CMake package config
# (so consumers can `find_package(trtc_asr)`) and a pkg-config file. Static and
# shared linkage are packaged separately because their exported CMake targets
# describe different link requirements.
#
# Usage:
#   scripts/package.sh [--static] [--shared] [--output DIR] [--jobs N]
#                      [--build-type TYPE] [--no-verify]
#
# With no linkage flag both static and shared packages are produced.

set -euo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_STATIC=0
BUILD_SHARED=0
BUILD_TYPE="Release"
OUTPUT_DIR="${ROOT_DIR}/dist"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
VERIFY=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --static) BUILD_STATIC=1; shift ;;
    --shared) BUILD_SHARED=1; shift ;;
    --output) OUTPUT_DIR="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --build-type) BUILD_TYPE="$2"; shift 2 ;;
    --no-verify) VERIFY=0; shift ;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ ${BUILD_STATIC} -eq 0 && ${BUILD_SHARED} -eq 0 ]]; then
  BUILD_STATIC=1
  BUILD_SHARED=1
fi

VERSION="$(sed -n 's/^#define TRTC_ASR_VERSION_STRING "\(.*\)"$/\1/p' \
  "${ROOT_DIR}/include/trtc_asr/version.h")"
if [[ -z "${VERSION}" ]]; then
  echo "cannot read version from include/trtc_asr/version.h" >&2
  exit 1
fi

OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

# Verifies the installed package is actually consumable: configures and builds a
# throwaway project that links the SDK through find_package(). Catches missing
# headers, broken export sets and missing transitive dependencies before the
# tarball ever reaches a customer.
verify_package() {
  local stage_dir="$1"
  local work_dir="$2"

  mkdir -p "${work_dir}/src"
  cat >"${work_dir}/src/main.cc" <<'EOF'
#include <cstdio>
#include <string>

#include "trtc_asr/credential.h"
#include "trtc_asr/file_recognizer.h"
#include "trtc_asr/sentence_recognizer.h"
#include "trtc_asr/sigpipe.h"
#include "trtc_asr/speech_recognizer.h"
#include "trtc_asr/usersig.h"
#include "trtc_asr/version.h"

int main() {
  // Also proves the optional SIGPIPE switch is exported by the installed lib.
  const bool sigpipe_ignored = trtc_asr::IgnoreSigpipeProcessWide();
  const trtc_asr::Credential credential(1400000000, 1400000001, "secret");
  const std::string sig = trtc_asr::GenUserSig(
      credential.sdk_app_id(), credential.secret_key(), "smoke-user", 60);
  std::printf("trtc_asr %s, usersig len=%zu, endpoint=%s, sigpipe_ignored=%d\n",
              TRTC_ASR_VERSION_STRING, sig.size(),
              trtc_asr::SpeechRecognizer::kEndpoint,
              static_cast<int>(sigpipe_ignored));
  return sig.empty() ? 1 : 0;
}
EOF
  cat >"${work_dir}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(trtc_asr_package_smoke LANGUAGES CXX)
find_package(trtc_asr REQUIRED)
add_executable(smoke src/main.cc)
target_link_libraries(smoke PRIVATE trtc_asr::trtc_asr)
EOF

  cmake -S "${work_dir}" -B "${work_dir}/build" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_PREFIX_PATH="${stage_dir}" >/dev/null
  cmake --build "${work_dir}/build" -j "${JOBS}" >/dev/null

  # Shared builds must also be loadable, not just linkable.
  DYLD_LIBRARY_PATH="${stage_dir}/lib:${DYLD_LIBRARY_PATH:-}" \
  LD_LIBRARY_PATH="${stage_dir}/lib:${LD_LIBRARY_PATH:-}" \
    "${work_dir}/build/smoke"
}

build_package() {
  local linkage="$1"
  local shared_flag="$2"

  local build_dir="${ROOT_DIR}/build/package-${linkage}"
  local name="trtc-asr-sdk-cpp-${VERSION}-${OS}-${ARCH}-${linkage}"
  local stage_root="${ROOT_DIR}/build/stage-${linkage}"
  local stage_dir="${stage_root}/${name}"

  echo "==> building ${linkage} package (${BUILD_TYPE})"
  rm -rf "${build_dir}" "${stage_root}"

  cmake -S "${ROOT_DIR}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DTRTC_ASR_BUILD_SHARED="${shared_flag}" \
    -DTRTC_ASR_BUILD_EXAMPLES=OFF \
    -DTRTC_ASR_BUILD_TESTS=OFF \
    -DTRTC_ASR_INSTALL=ON \
    -DCMAKE_INSTALL_PREFIX="${stage_dir}" \
    -DCMAKE_INSTALL_LIBDIR=lib
  cmake --build "${build_dir}" -j "${JOBS}"
  cmake --install "${build_dir}"

  if [[ ${VERIFY} -eq 1 ]]; then
    echo "==> verifying ${linkage} package"
    verify_package "${stage_dir}" "${build_dir}/smoke"
  fi

  mkdir -p "${OUTPUT_DIR}"
  tar -czf "${OUTPUT_DIR}/${name}.tar.gz" -C "${stage_root}" "${name}"
  echo "==> ${OUTPUT_DIR}/${name}.tar.gz"
}

[[ ${BUILD_STATIC} -eq 1 ]] && build_package static OFF
[[ ${BUILD_SHARED} -eq 1 ]] && build_package shared ON

echo
echo "packages in ${OUTPUT_DIR}:"
ls -1 "${OUTPUT_DIR}"
