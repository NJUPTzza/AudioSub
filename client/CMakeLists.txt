# client/CMakeLists.txt
# =====================
# 构建 audiosub_client.exe（控制台客户端）。
# 顶层已经定义好了 webrtc::webrtc 这个 imported target，这里直接链接它。

# 三个源文件：
#   main.cc                       入口 + 信令/WebRTC 接线
#   signaling_client.cc           信令客户端（TCP + JSON）
#   peer_connection_client.cc     WebRTC PeerConnection 封装
add_executable(audiosub_client
    main.cc
    signaling_client.cc
    peer_connection_client.cc
    wasapi_mic_capture.cc
)

# 头文件搜索路径：
#   third_party  用来找 nlohmann/json.hpp
#   当前目录     用来找 signaling_client.h 等本地头
target_include_directories(audiosub_client PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 编译定义：
#   JSON_NOEXCEPTION 让 nlohmann/json 内部走"无异常"路径，
#   和我们项目 _HAS_EXCEPTIONS=0 + /EHs-c- 保持兼容。
target_compile_definitions(audiosub_client PRIVATE
    JSON_NOEXCEPTION
)

# 链接：
#   webrtc::webrtc 是顶层定义的 imported target，会自动带上 webrtc.lib +
#                  所有 Windows 系统库 + 必要的头文件路径 + 编译宏。
#   ws2_32.lib     signaling_client.cc 也直接用了 Winsock，再链一次也无害。
target_link_libraries(audiosub_client PRIVATE
    audiosub::core
    audiosub::audio
    audiosub::asr
    webrtc::webrtc
    ws2_32.lib
)
