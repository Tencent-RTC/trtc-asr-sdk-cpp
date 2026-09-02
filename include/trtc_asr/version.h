#pragma once

/// Single source of truth for the SDK version. CMakeLists.txt parses
/// TRTC_ASR_VERSION_STRING from this file to set the project version, so the
/// library, the installed CMake package and the packaged artifacts always
/// agree with the headers a customer compiles against.
#define TRTC_ASR_VERSION_MAJOR 0
#define TRTC_ASR_VERSION_MINOR 1
#define TRTC_ASR_VERSION_PATCH 0
#define TRTC_ASR_VERSION_STRING "0.1.0"

/// Comparable integer form, e.g. 0.1.0 -> 000100.
#define TRTC_ASR_VERSION_NUMBER                                    \
  (TRTC_ASR_VERSION_MAJOR * 10000 + TRTC_ASR_VERSION_MINOR * 100 + \
   TRTC_ASR_VERSION_PATCH)
