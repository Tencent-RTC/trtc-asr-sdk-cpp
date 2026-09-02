cmake_minimum_required(VERSION 3.16)
project(trtc-asr-sdk-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- Dependencies -----------------------------------------------------------

# OpenSSL (HMAC-SHA256, TLS). Homebrew on macOS needs a hint.
if(APPLE)
  if(EXISTS "/opt/homebrew/opt/openssl")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
  elseif(EXISTS "/usr/local/opt/openssl")
    set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
  endif()
endif()
find_package(OpenSSL REQUIRED)

# zlib.
find_package(ZLIB REQUIRED)

# libcurl for the HTTP recognizers.
find_package(CURL REQUIRED)

# --- Library ----------------------------------------------------------------

add_library(trtc_asr
  src/usersig.cc
  src/signature_params.cc
  src/params.cc
  src/types.cc
  src/ws_client.cc
  src/http_client.cc
  src/speech_recognizer.cc
  src/sentence_recognizer.cc
  src/file_recognizer.cc
)

target_include_directories(trtc_asr
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(trtc_asr
  PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
)

# --- Examples ---------------------------------------------------------------

option(TRTC_ASR_BUILD_EXAMPLES "Build examples" ON)
if(TRTC_ASR_BUILD_EXAMPLES)
  add_executable(realtime_asr examples/realtime_asr.cc)
  target_link_libraries(realtime_asr PRIVATE trtc_asr)

  add_executable(sentence_asr examples/sentence_asr.cc)
  target_link_libraries(sentence_asr PRIVATE trtc_asr)

  add_executable(file_asr examples/file_asr.cc)
  target_link_libraries(file_asr PRIVATE trtc_asr)
endif()

# --- Tests ------------------------------------------------------------------

option(TRTC_ASR_BUILD_TESTS "Build tests" ON)
if(TRTC_ASR_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  )
  # On macOS, gtest needs no extra config; prevent overriding compiler flags.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(trtc_asr_tests
    tests/usersig_test.cc
    tests/signature_test.cc
    tests/params_test.cc
    tests/sentence_recognizer_test.cc
    tests/file_recognizer_test.cc
    tests/speech_recognizer_test.cc
    tests/mock_servers.cc
  )
  target_include_directories(trtc_asr_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  )
  target_link_libraries(trtc_asr_tests PRIVATE trtc_asr GTest::gtest GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(trtc_asr_tests)
endif()
