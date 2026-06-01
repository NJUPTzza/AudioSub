# client/capi/CMakeLists.txt
# ==========================
# audiosub_capi.dll —— C ABI 共享库。把 audiosub_engine(/MT, 含 WebRTC) 整个吃进来，
# 只导出 extern "C" 函数。Qt 前端(/MD) 通过这层 C 边界调用，绕开 CRT 不一致问题。

add_library(audiosub_capi SHARED
    audiosub_capi.cc
)

# AUDIOSUB_CAPI_BUILD：本 DLL 编译时用 dllexport，外部包含头时用 dllimport。
target_compile_definitions(audiosub_capi PRIVATE
    AUDIOSUB_CAPI_BUILD
)

# 公开头目录，方便其它 target（如 Qt 前端）include "audiosub_capi.h"。
target_include_directories(audiosub_capi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接核心引擎（静态，/MT）。webrtc 等依赖随 audiosub_engine 传递进来。
target_link_libraries(audiosub_capi PRIVATE
    audiosub_engine
)

# 我们用 __declspec(dllexport) 显式导出，无需全量导出。
set_target_properties(audiosub_capi PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)
