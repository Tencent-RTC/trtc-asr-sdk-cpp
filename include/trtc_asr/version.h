#pragma once

/// Single source of truth for the SDK version. CMakeLists.txt parses
/// TRTC_ASR_VERSION_STRING from this file to set the project version, so the
/// library, the installed CMake package and the packaged artifacts always
/// agree with the headers a customer compiles against.
#define TRTC_ASR_VERSION_MAJOR 1
#define TRTC_ASR_VERSION_MINOR 2
#define TRTC_ASR_VERSION_PATCH 0
#define TRTC_ASR_VERSION_STRING "1.2.0"

/// Comparable integer form, e.g. 1.0.0 -> 10000.
#define TRTC_ASR_VERSION_NUMBER                                    \
  (TRTC_ASR_VERSION_MAJOR * 10000 + TRTC_ASR_VERSION_MINOR * 100 + \
   TRTC_ASR_VERSION_PATCH)
