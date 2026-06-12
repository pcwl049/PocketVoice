cmake_minimum_required(VERSION 3.22.1)
project("stt_native")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(STT_USE_QNN "Link against sherpa-onnx QNN Android libraries" OFF)

set(SHERPA_ONNX_DIR "${CMAKE_SOURCE_DIR}/../../../../../../third_party")
set(SHERPA_ONNX_ANDROID_LIB_DIR "${SHERPA_ONNX_DIR}/lib/android/${ANDROID_ABI}")
if(STT_USE_QNN)
    set(SHERPA_ONNX_ANDROID_LIB_DIR "${SHERPA_ONNX_DIR}/lib/android/${ANDROID_ABI}-qnn")
    add_compile_definitions(STT_USE_QNN=1)
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../../../../../../src/common
    ${SHERPA_ONNX_DIR}/sherpa-onnx-src
    ${SHERPA_ONNX_DIR}/sherpa-onnx-v1.12.39-win-x64-shared-MD-Release/include
)

add_library(sherpa-onnx-c-api SHARED IMPORTED)
set_target_properties(sherpa-onnx-c-api PROPERTIES
    IMPORTED_LOCATION ${SHERPA_ONNX_ANDROID_LIB_DIR}/libsherpa-onnx-c-api.so
)

add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${SHERPA_ONNX_ANDROID_LIB_DIR}/libonnxruntime.so
)

add_library(stt_native SHARED
    audio_job_queue.cpp
    audio_job_queue.h
    stt_engine.cpp
    stt_engine.h
    network.cpp
    network.h
    text_postprocess.cpp
    text_postprocess.h
    runtime_state.cpp
    runtime_state.h
    jni_bridge.cpp
)

target_link_libraries(stt_native
    sherpa-onnx-c-api
    onnxruntime
    log
)
