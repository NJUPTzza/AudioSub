# 顶层 CMakeLists.txt
# ===================
# AudioSub 项目的根 CMake 文件。
#
# WebRTC 依赖通过 third_party/webrtc-sdk/ 提供（不提交到 git）：
#   third_party/webrtc-sdk/lib/webrtc.lib
#   third_party/webrtc-sdk/include/          <- 等价于 webrtc/src 根目录
#   third_party/webrtc-sdk/VERSION.txt
#
# 首次 clone 后运行：
#   .\scripts\fetch-webrtc-sdk.ps1 -Url <GitHub-Release-zip-url>
# 或维护者本地：
#   .\scripts\pack-webrtc-sdk.ps1

cmake_minimum_required(VERSION 3.20)
project(AudioSub CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WebRTC Release 静态库使用 /MT，必须保持一致
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- WebRTC SDK 路径 ---
set(WEBRTC_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/webrtc-sdk" CACHE PATH
    "Directory containing lib/webrtc.lib and include/")
set(WEBRTC_LIB "${WEBRTC_SDK_DIR}/lib/webrtc.lib" CACHE FILEPATH
    "WebRTC monolithic static library")
set(WEBRTC_INCLUDE_DIR "${WEBRTC_SDK_DIR}/include" CACHE PATH
    "WebRTC public headers root (contains api/, rtc_base/, ...)")

if(NOT EXISTS "${WEBRTC_LIB}")
    message(FATAL_ERROR
        "WebRTC SDK not found at ${WEBRTC_LIB}\n"
        "Run one of:\n"
        "  .\\scripts\\fetch-webrtc-sdk.ps1 -Url <release-zip-url>\n"
        "  .\\scripts\\fetch-webrtc-sdk.ps1 -LocalPath <sdk-dir>\n"
        "  .\\scripts\\pack-webrtc-sdk.ps1   (maintainer, local webrtc build)\n"
    )
endif()

if(NOT EXISTS "${WEBRTC_INCLUDE_DIR}/api")
    message(FATAL_ERROR
        "WebRTC headers not found under ${WEBRTC_INCLUDE_DIR}\n"
        "Your SDK package looks incomplete.")
endif()

if(EXISTS "${WEBRTC_SDK_DIR}/VERSION.txt")
    file(READ "${WEBRTC_SDK_DIR}/VERSION.txt" WEBRTC_SDK_VERSION_RAW)
    message(STATUS "WebRTC SDK:\n${WEBRTC_SDK_VERSION_RAW}")
endif()

# --- whisper.cpp ---
# 关闭非必要目标，避免把示例/工具都编进去
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/whisper.cpp" EXCLUDE_FROM_ALL)

# whisper.cpp 不同版本 target 名称可能不同，做兼容探测
set(AUDIOSUB_WHISPER_TARGET "")
if (TARGET whisper)
    set(AUDIOSUB_WHISPER_TARGET whisper)
elseif (TARGET whisper-lib)
    set(AUDIOSUB_WHISPER_TARGET whisper-lib)
elseif (TARGET ggml-whisper)
    set(AUDIOSUB_WHISPER_TARGET ggml-whisper)
else()
    message(FATAL_ERROR
        "Cannot find whisper target. Checked: whisper, whisper-lib, ggml-whisper")
endif()

# --- imported target: webrtc::webrtc ---
add_library(webrtc::webrtc STATIC IMPORTED GLOBAL)
set_target_properties(webrtc::webrtc PROPERTIES
    IMPORTED_LOCATION "${WEBRTC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES
        "${WEBRTC_INCLUDE_DIR};${WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp"
    INTERFACE_COMPILE_DEFINITIONS
        "WEBRTC_WIN;NOMINMAX;WIN32_LEAN_AND_MEAN;_WIN32_WINNT=0x0A00;_HAS_EXCEPTIONS=0;_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS"
)
target_link_libraries(webrtc::webrtc INTERFACE
    winmm.lib
    secur32.lib
    dmoguids.lib
    wmcodecdspuuid.lib
    msdmo.lib
    strmiids.lib
    iphlpapi.lib
    ws2_32.lib
    crypt32.lib
    bcrypt.lib
    dwmapi.lib
    d3d11.lib
    dxgi.lib
)

if(MSVC)
    add_compile_options(/utf-8 /EHs-c- /GR-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Shared header-only contracts for multi-person parallel development.
add_library(audiosub_core INTERFACE)
add_library(audiosub::core ALIAS audiosub_core)
target_include_directories(audiosub_core INTERFACE
    "${CMAKE_SOURCE_DIR}/include"
)

add_library(audiosub_audio STATIC
    "${CMAKE_SOURCE_DIR}/src/audio/pcm_ring_buffer.cc"
    "${CMAKE_SOURCE_DIR}/src/audio/asr_audio_converter.cc"
)
add_library(audiosub::audio ALIAS audiosub_audio)
target_link_libraries(audiosub_audio PUBLIC audiosub::core)
target_include_directories(audiosub_audio PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
)

add_library(audiosub_asr STATIC
    "${CMAKE_SOURCE_DIR}/src/asr/whisper_cpp_engine.cc"
)
add_library(audiosub::asr ALIAS audiosub_asr)
target_link_libraries(audiosub_asr PUBLIC
    audiosub::core
    ${AUDIOSUB_WHISPER_TARGET}
)
target_include_directories(audiosub_asr PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/include"
)

add_subdirectory(client)

# --- Qt GUI 前端（可选）---
# 通过 C ABI DLL(audiosub_capi) 访问引擎，自身用 /MD，独立于 WebRTC 的 /MT。
option(AUDIOSUB_BUILD_GUI "Build the Qt GUI front-end" ON)
if(AUDIOSUB_BUILD_GUI)
    set(QT_PREFIX_PATH "E:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt install prefix")
    list(APPEND CMAKE_PREFIX_PATH "${QT_PREFIX_PATH}")
    add_subdirectory(gui)
endif()
