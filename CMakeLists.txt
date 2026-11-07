cmake_minimum_required(VERSION 3.25)
project(Updater VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_path(PICOSHA2_INCLUDE_DIR NAMES picosha2.h REQUIRED)

if(WIN32)
    set(updater_os windows)
elseif(APPLE)
    set(updater_os macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(updater_os linux)
else()
    message(FATAL_ERROR "Unsupported target system: ${CMAKE_SYSTEM_NAME}")
endif()

if(VCPKG_TARGET_TRIPLET MATCHES "^(x64|arm64)-")
    set(updater_arch "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Use an x64 or arm64 vcpkg triplet")
endif()

add_executable(updater
    src/main.cpp
    src/download.cpp
    src/json_parse.cpp
    src/zip_extract.cpp
    src/tarzst_extract.cpp
    src/fs_ops.cpp
    src/process.cpp
    src/utils.cpp
)
target_include_directories(updater PRIVATE "${PICOSHA2_INCLUDE_DIR}")
target_compile_definitions(updater PRIVATE UPDATER_PLATFORM="${updater_os}-${updater_arch}")
target_link_libraries(updater PRIVATE
    CURL::libcurl nlohmann_json::nlohmann_json miniz::miniz zstd::libzstd Threads::Threads
)

if(MSVC)
    set_property(TARGET updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(updater PRIVATE /utf-8)
    target_link_libraries(updater PRIVATE shell32)
elseif(updater_os STREQUAL "linux")
    target_link_options(updater PRIVATE -static)
endif()

install(TARGETS updater RUNTIME DESTINATION .)
