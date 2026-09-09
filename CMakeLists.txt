cmake_minimum_required(VERSION 3.16)

# Version comes from include/trtc_asr/version.h so headers, library and packaged
# artifacts can never drift apart.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/trtc_asr/version.h" _trtc_asr_version_header)
string(REGEX MATCH "TRTC_ASR_VERSION_STRING[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
       _trtc_asr_version_match "${_trtc_asr_version_header}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "Cannot parse TRTC_ASR_VERSION_STRING from include/trtc_asr/version.h")
endif()
set(TRTC_ASR_VERSION "${CMAKE_MATCH_1}")
unset(_trtc_asr_version_header)

project(trtc-asr-sdk-cpp VERSION ${TRTC_ASR_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake < 3.21 has no PROJECT_IS_TOP_LEVEL.
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PROJECT_IS_TOP_LEVEL ON)
  else()
    set(PROJECT_IS_TOP_LEVEL OFF)
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Examples and tests are for SDK developers. Consumers embedding the SDK via
# add_subdirectory()/FetchContent must not pay for building them, nor for
# downloading googletest.
option(TRTC_ASR_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(TRTC_ASR_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(TRTC_ASR_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
option(TRTC_ASR_BUILD_SHARED "Build a shared library instead of a static one" OFF)

include(GNUInstallDirs)

# --- Dependencies -----------------------------------------------------------

# OpenSSL (HMAC-SHA256, TLS). Homebrew on macOS needs a hint.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
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

if(TRTC_ASR_BUILD_SHARED)
  set(_trtc_asr_lib_type SHARED)
else()
  set(_trtc_asr_lib_type STATIC)
endif()

add_library(trtc_asr ${_trtc_asr_lib_type}
  src/usersig.cc
  src/sdkinfo.cc
  src/signature_params.cc
  src/params.cc
  src/types.cc
  src/ws_client.cc
  src/http_client.cc
  src/speech_recognizer.cc
  src/sentence_recognizer.cc
  src/file_recognizer.cc
  src/v3.cc
)
add_library(trtc_asr::trtc_asr ALIAS trtc_asr)

set_target_properties(trtc_asr PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
)

target_compile_features(trtc_asr PUBLIC cxx_std_17)

target_include_directories(trtc_asr
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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

if(TRTC_ASR_BUILD_EXAMPLES)
  add_executable(realtime_asr examples/realtime_asr.cc)
  add_executable(v3_realtime_asr examples/v3_realtime_asr.cc)
  target_link_libraries(v3_realtime_asr PRIVATE trtc_asr)
  add_executable(v3_sentence_asr examples/v3_sentence_asr.cc)
  target_link_libraries(v3_sentence_asr PRIVATE trtc_asr)
  add_executable(v3_file_asr examples/v3_file_asr.cc)
  target_link_libraries(v3_file_asr PRIVATE trtc_asr)
  target_link_libraries(realtime_asr PRIVATE trtc_asr)

  add_executable(sentence_asr examples/sentence_asr.cc)
  target_link_libraries(sentence_asr PRIVATE trtc_asr)

  add_executable(file_asr examples/file_asr.cc)
  target_link_libraries(file_asr PRIVATE trtc_asr)
endif()

# --- Tests ------------------------------------------------------------------

if(TRTC_ASR_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  # On macOS, gtest needs no extra config; prevent overriding compiler flags.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(trtc_asr_tests
    tests/usersig_test.cc
    tests/signature_test.cc
    tests/params_test.cc
    tests/sentence_recognizer_test.cc
    tests/file_recognizer_test.cc
    tests/speech_recognizer_test.cc
    tests/sdkinfo_test.cc
    tests/site_test.cc
    tests/mock_servers.cc
    tests/v3_test.cc
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

# --- Install / package ------------------------------------------------------

if(TRTC_ASR_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS trtc_asr
    EXPORT trtc_asrTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  install(DIRECTORY include/trtc_asr
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
  )

  set(TRTC_ASR_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/trtc_asr)

  install(EXPORT trtc_asrTargets
    FILE trtc_asrTargets.cmake
    NAMESPACE trtc_asr::
    DESTINATION ${TRTC_ASR_CMAKE_DIR}
  )

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/trtc_asrConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/trtc_asrConfig.cmake
    INSTALL_DESTINATION ${TRTC_ASR_CMAKE_DIR}
  )
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/trtc_asrConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/trtc_asrConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/trtc_asrConfigVersion.cmake
    DESTINATION ${TRTC_ASR_CMAKE_DIR}
  )

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/trtc_asr.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/trtc_asr.pc
    @ONLY
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/trtc_asr.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  )

  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    OPTIONAL
  )
endif()
