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
    ${CMAKE_SOURCE_DIR}/../../../../../../build/qnn-android/headers
    ${CMAKE_SOURCE_DIR}/../../../../../../build/qnn-android/_deps/eigen-src
    ${CMAKE_SOURCE_DIR}/../../../../../../build/qnn-android/_deps/json-src/include
)

set(KALDI_NATIVE_FBANK_SRC_DIR "${SHERPA_ONNX_DIR}/sherpa-onnx-src/build-android-arm64-v8a/_deps/kaldi_native_fbank-src/kaldi-native-fbank")
set(KISSFFT_SRC_DIR "${SHERPA_ONNX_DIR}/sherpa-onnx-src/build-android-arm64-v8a/_deps/kissfft-src")

include_directories(
    ${KALDI_NATIVE_FBANK_SRC_DIR}
    ${SHERPA_ONNX_DIR}/sherpa-onnx-src/build-android-arm64-v8a/_deps/kaldi_native_fbank-src
    ${KISSFFT_SRC_DIR}
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
    qwen3_qnn_backend.cpp
    qwen3_qnn_backend.h
    qwen3_tokenizer.cpp
    qwen3_tokenizer.h
    network.cpp
    network.h
    text_postprocess.cpp
    text_postprocess.h
    runtime_state.cpp
    runtime_state.h
    jni_bridge.cpp
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/feature-fbank.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/feature-functions.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/feature-mfcc.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/feature-raw-audio-samples.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/feature-window.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/kaldi-math.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/mel-computations.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/online-feature.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/rfft.cc
    ${KALDI_NATIVE_FBANK_SRC_DIR}/csrc/whisper-feature.cc
    ${KISSFFT_SRC_DIR}/kiss_fft.c
    ${KISSFFT_SRC_DIR}/kiss_fftr.c
    ${SHERPA_ONNX_DIR}/sherpa-onnx-src/sherpa-onnx/csrc/file-utils.cc
    ${SHERPA_ONNX_DIR}/sherpa-onnx-src/sherpa-onnx/csrc/math.cc
    ${SHERPA_ONNX_DIR}/sherpa-onnx-src/sherpa-onnx/csrc/qwen-asr-tokenizer.cc
)

# QNN SDK headers for qwen3_qnn_backend.cpp
set(QNN_SDK_DIR "G:/Program Files/qairt/2.45.0.260326")
set(QNN_INCLUDE_DIR "${QNN_SDK_DIR}/include/QNN")
set(QNN_SAMPLE_INCLUDE_DIR "${QNN_SDK_DIR}/examples/QNN/SampleApp/SampleApp/src")
target_include_directories(stt_native PRIVATE
    ${QNN_INCLUDE_DIR}
    ${QNN_SAMPLE_INCLUDE_DIR}
    ${QNN_SAMPLE_INCLUDE_DIR}/WrapperUtils
)

target_link_libraries(stt_native
    sherpa-onnx-c-api
    onnxruntime
    android
    log
)
