cmake_minimum_required(VERSION 3.16)
project(DarkEyeUpdater LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)

# MSVC: 使用 UTF-8 编码，避免日志乱码；静态链接运行时，生成无外部 DLL 依赖的单 exe
if(MSVC)
  add_compile_options(/utf-8)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 依赖：全部使用 3rdparty 本地库
# 3rdparty/
#   picosha2/picosha2.h
#   nlohmann_json/json.hpp
#   miniz/miniz.h, miniz.c
#   zstd/zstd.h, zstddeclib.c, zstd_errors.h
#   curl/include/curl/*.h ，curl/lib/libcurl.lib（静态、/MT、Schannel 与工程一致）
set(THIRDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
set(CURL_LIB "${THIRDPARTY_DIR}/curl/lib/libcurl.lib")
if(NOT EXISTS "${CURL_LIB}")
  message(FATAL_ERROR "缺少 libcurl：请将 libcurl.lib 放到 3rdparty/curl/lib/（与当前 MSVC /MT 一致）")
endif()

add_executable(DarkEyeUpdater
    src/main.cpp
    src/download.cpp
    src/json_parse.cpp
    src/zip_extract.cpp
    src/tarzst_extract.cpp
    src/fs_ops.cpp
    src/process.cpp
    src/utils.cpp
    ${THIRDPARTY_DIR}/miniz/miniz.c
    ${THIRDPARTY_DIR}/zstd/zstddeclib.c
)

target_include_directories(DarkEyeUpdater PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/picosha2
    ${THIRDPARTY_DIR}/nlohmann_json
    ${THIRDPARTY_DIR}/miniz
    ${THIRDPARTY_DIR}/zstd
    ${THIRDPARTY_DIR}/curl/include
)

target_compile_definitions(DarkEyeUpdater PRIVATE CURL_STATICLIB)

target_link_libraries(DarkEyeUpdater PRIVATE
    "${CURL_LIB}"
    ws2_32
    crypt32
    advapi32
    normaliz
    secur32
    bcrypt
    iphlpapi
    shell32
)
