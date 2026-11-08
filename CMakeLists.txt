cmake_minimum_required(VERSION 3.25)

# vcpkg consumes the triplet during project(), before target configuration.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        set(VCPKG_TARGET_TRIPLET arm64-osx CACHE STRING "vcpkg target triplet")
    elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(VCPKG_TARGET_TRIPLET x64-osx CACHE STRING "vcpkg target triplet")
    elseif(NOT CMAKE_OSX_ARCHITECTURES)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE updater_host_arch
            OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        if(updater_host_arch STREQUAL "arm64")
            set(VCPKG_TARGET_TRIPLET arm64-osx CACHE STRING "vcpkg target triplet")
        else()
            set(VCPKG_TARGET_TRIPLET x64-osx CACHE STRING "vcpkg target triplet")
        endif()
    else()
        message(FATAL_ERROR "Build macOS x86_64 and arm64 separately; universal builds are not supported")
    endif()
endif()
project(Updater VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(updater_download_source src/download_macos.mm)
else()
    find_package(CURL CONFIG REQUIRED)
    set(updater_download_source src/download.cpp)
endif()
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
    src/resume.cpp
    ${updater_download_source}
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
    nlohmann_json::nlohmann_json miniz::miniz zstd::libzstd Threads::Threads
)

if(APPLE)
    set_source_files_properties(src/download_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(updater PRIVATE "-framework Foundation")
else()
    target_link_libraries(updater PRIVATE CURL::libcurl)
endif()

if(MSVC)
    set_property(TARGET updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(updater PRIVATE /utf-8)
    target_link_libraries(updater PRIVATE shell32 user32)
elseif(updater_os STREQUAL "linux")
    target_link_options(updater PRIVATE -static)
endif()

install(TARGETS updater RUNTIME DESTINATION .)
