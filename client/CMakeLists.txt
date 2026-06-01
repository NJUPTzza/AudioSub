# client/CMakeLists.txt
# =====================
# 这里构建：
#   1. audiosub_engine   —— 静态库(/MT)，吃掉 WebRTC/信令/ASR/融合的全部接线，
#                            对外暴露 std::function 事件回调。CLI 和 DLL 都复用它。
#   2. audiosub_client.exe —— 命令行外壳，链接 audiosub_engine。
#
# 注：DLL（C ABI，给 Qt 用）在 capi/ 子目录里构建，同样链接 audiosub_engine。

# --- 核心引擎静态库（/MT，与 WebRTC 一致）---
add_library(audiosub_engine STATIC
    audiosub_engine.cc
    signaling_client.cc
    peer_connection_client.cc
    wasapi_mic_capture.cc
)

# third_party  -> nlohmann/json.hpp
# 当前目录     -> signaling_client.h / peer_connection_client.h / audiosub_engine.h
target_include_directories(audiosub_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# JSON_NOEXCEPTION：让 nlohmann/json 走无异常路径，匹配 _HAS_EXCEPTIONS=0 + /EHs-c-。
target_compile_definitions(audiosub_engine PUBLIC
    JSON_NOEXCEPTION
)

target_link_libraries(audiosub_engine PUBLIC
    audiosub::core
    audiosub::audio
    audiosub::asr
    webrtc::webrtc
    ws2_32.lib
)

# --- 命令行客户端 ---
add_executable(audiosub_client
    main.cc
)
target_link_libraries(audiosub_client PRIVATE
    audiosub_engine
)

# --- C ABI DLL（供 Qt 前端动态调用）---
add_subdirectory(capi)
