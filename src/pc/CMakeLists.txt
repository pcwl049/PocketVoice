cmake_minimum_required(VERSION 3.20)
project(stt_pc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(SHERPA_ONNX_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../third_party/sherpa-onnx-v1.12.39-win-x64-shared-MD-Release/include" CACHE PATH "sherpa-onnx include directory")
set(SHERPA_ONNX_LIB_DIR "${CMAKE_SOURCE_DIR}/../../third_party/sherpa-onnx-v1.12.39-win-x64-shared-MD-Release/lib" CACHE PATH "sherpa-onnx lib directory")
set(WEBVIEW2_DIR "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/1.0.3967.48" CACHE PATH "Microsoft.Web.WebView2 NuGet package directory")

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../common
    ${SHERPA_ONNX_INCLUDE_DIR}
    ${SHERPA_ONNX_INCLUDE_DIR}/sherpa-onnx/c-api
    ${WEBVIEW2_DIR}/build/native/include
    "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}/winrt"
)

link_directories(${SHERPA_ONNX_LIB_DIR})
link_directories(${WEBVIEW2_DIR}/build/native/x64)

set(COMMON_SOURCES
    ../common/protocol.h
    ../common/config.h
)

set(PC_SOURCES
    main.cpp
    audio/vad.cpp
    audio/vad.h
    audio/buffer.cpp
    audio/buffer.h
    audio/pre_roll_buffer.cpp
    audio/pre_roll_buffer.h
    audio/sample_converter.cpp
    audio/sample_converter.h
    audio/wasapi_capture.cpp
    audio/wasapi_capture.h
    osc/sender.cpp
    osc/sender.h
    network/client.cpp
    network/client.h
    chatbox_formatter.cpp
    chatbox_formatter.h
    chatbox_queue.cpp
    chatbox_queue.h
    pc_app_controller.cpp
    pc_app_controller.h
    pc_runtime.cpp
    pc_runtime.h
    pc_logger.cpp
    pc_logger.h
    pc_status_json.cpp
    pc_status_json.h
    pc_status_page.cpp
    pc_status_page.h
    status_http_server.cpp
    status_http_server.h
    adb_bridge.cpp
    adb_bridge.h
    pc_webview_window.cpp
    pc_webview_window.h
    embedded_vad_model.cpp
    embedded_vad_model.h
    resource.h
    pc_resources.rc
)

add_executable(stt_pc ${PC_SOURCES} ${COMMON_SOURCES})

target_link_libraries(stt_pc
    sherpa-onnx-c-api
    onnxruntime
    WebView2LoaderStatic
    version
    advapi32
    ws2_32
    user32
    ole32
    avrt
    mmdevapi
    propsys
)

target_compile_definitions(stt_pc PRIVATE _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    target_compile_options(stt_pc PRIVATE /utf-8 /W3)
    target_link_options(stt_pc PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

add_custom_command(TARGET stt_pc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${SHERPA_ONNX_LIB_DIR}/.."
    "$<TARGET_FILE_DIR:stt_pc>"
)
