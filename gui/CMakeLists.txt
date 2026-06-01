# gui/CMakeLists.txt
# ==================
# audiosub_gui.exe —— Qt Widgets 前端。
#
# 关键：本 target 必须用「动态 CRT(/MD) + 开异常 + 开 RTTI」，覆盖项目顶层为了
# WebRTC 设的全局 /MT、/EHs-c-、/GR-。它只通过 audiosub_capi 的 C 边界访问引擎，
# 绝不直接链接 webrtc/audiosub_engine，从而避开 CRT 不一致。

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(audiosub_gui WIN32
    main.cpp
    main_window.cpp
    main_window.h
)

# 覆盖全局编译设定：动态运行库 + 异常 + RTTI（Qt 需要）。
set_target_properties(audiosub_gui PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)
target_compile_options(audiosub_gui PRIVATE /EHsc /GR)

target_link_libraries(audiosub_gui PRIVATE
    audiosub_capi     # 只依赖 C ABI DLL 的 import 库
    Qt6::Widgets
)

# --- 运行期部署 ---
# 1) 把 audiosub_capi.dll 拷到 GUI 输出目录（它需要和 exe 同目录）。
add_custom_command(TARGET audiosub_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:audiosub_capi> $<TARGET_FILE_DIR:audiosub_gui>
    COMMENT "Copy audiosub_capi.dll next to audiosub_gui.exe")

# 2) 用 windeployqt 拷贝 Qt 运行库（Qt6Core/Gui/Widgets + platforms 插件）。
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT_PREFIX_PATH}/bin")
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET audiosub_gui POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" --no-translations --no-compiler-runtime
            $<TARGET_FILE:audiosub_gui>
        COMMENT "Deploy Qt runtime via windeployqt")
else()
    message(WARNING "windeployqt not found under ${QT_PREFIX_PATH}/bin; "
                    "you may need to add Qt bin to PATH to run audiosub_gui.")
endif()
