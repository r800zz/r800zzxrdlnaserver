cmake_minimum_required(VERSION 3.24)
project(r800zz_dlna_server LANGUAGES CXX CUDA RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This realtime build currently targets Windows x64.")
endif()

set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "FFmpeg shared development package root")
set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime GPU package root")
set(R800ZZ_RUNTIME_DLLS "$ENV{R800ZZ_RUNTIME_DLLS}" CACHE PATH "Additional CUDA/cuDNN runtime DLL directory")

if(NOT FFMPEG_ROOT OR NOT EXISTS "${FFMPEG_ROOT}/include/libavformat/avformat.h")
    message(FATAL_ERROR "Set FFMPEG_ROOT to the extracted FFmpeg shared development package. Run setup_dependencies.bat first.")
endif()
if(NOT ONNXRUNTIME_ROOT OR NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "Set ONNXRUNTIME_ROOT to the extracted ONNX Runtime GPU package. Run setup_dependencies.bat first.")
endif()

find_package(CUDAToolkit REQUIRED)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_executable(r800zz_dlna_server WIN32
    resources/app.rc
    src/main.cpp
    src/dlna_server.cpp
    src/dlna_server.h
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(r800zz_dlna_server PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_ROOT}/include
)

target_compile_definitions(r800zz_dlna_server PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

target_link_libraries(r800zz_dlna_server PRIVATE
    d3d11 dxgi d3dcompiler ws2_32 iphlpapi comdlg32 ole32 shell32
)

add_executable(r800zz_ai_worker
    resources/app.rc
    src/r800zz_ai_worker.cpp
    src/cpu_converter.cpp
    src/cpu_converter.h
    src/simple_video_player.cpp
    src/simple_video_player.h
    src/rvm_ort.cpp
    src/rvm_ort.h
    src/cuda_kernels.cu
    src/cuda_kernels.h
)

set_target_properties(r800zz_ai_worker PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES native
)

target_include_directories(r800zz_ai_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_ROOT}/include
    ${ONNXRUNTIME_ROOT}/include
)

target_compile_definitions(r800zz_ai_worker PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
)

if(MSVC)
    target_compile_options(r800zz_dlna_server PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(r800zz_ai_worker PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
endif()

find_library(AVFORMAT_LIB avformat PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(AVCODEC_LIB avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(AVUTIL_LIB avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(SWRESAMPLE_LIB swresample PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(SWSCALE_LIB swscale PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(ONNXRUNTIME_LIB onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)

target_link_libraries(r800zz_dlna_server PRIVATE
    ${AVFORMAT_LIB}
    ${AVUTIL_LIB}
)

target_link_libraries(r800zz_ai_worker PRIVATE
    ${AVFORMAT_LIB}
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
    ${SWRESAMPLE_LIB}
    ${SWSCALE_LIB}
    ${ONNXRUNTIME_LIB}
    CUDA::cudart
    user32
    gdi32
    comctl32
    winmm
)

# Put both EXEs together because the DLNA server launches the worker beside itself.
set_target_properties(r800zz_dlna_server r800zz_ai_worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
)

add_dependencies(r800zz_dlna_server r800zz_ai_worker)

# Keep the deployed FFmpeg/ONNX Runtime set aligned with
# collect_runtime_dlls.bat.  Copying every DLL from these development
# packages also deploys large unused components such as avfilter and the
# TensorRT execution provider.
file(GLOB FFMPEG_DLLS
    "${FFMPEG_ROOT}/bin/avformat-*.dll"
    "${FFMPEG_ROOT}/bin/avcodec-*.dll"
    "${FFMPEG_ROOT}/bin/avutil-*.dll"
    "${FFMPEG_ROOT}/bin/swresample-*.dll"
    "${FFMPEG_ROOT}/bin/swscale-*.dll"
)
set(ORT_DLLS
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_cuda.dll"
)
file(GLOB EXTRA_RUNTIME_DLLS "${R800ZZ_RUNTIME_DLLS}/*.dll")

add_custom_command(TARGET r800zz_ai_worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:r800zz_ai_worker>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DLLS} $<TARGET_FILE_DIR:r800zz_ai_worker>
)
if(EXTRA_RUNTIME_DLLS)
    add_custom_command(TARGET r800zz_ai_worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EXTRA_RUNTIME_DLLS} $<TARGET_FILE_DIR:r800zz_ai_worker>
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rvm_mobilenetv3_fp32.onnx")
    add_custom_command(TARGET r800zz_ai_worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/rvm_mobilenetv3_fp32.onnx"
            $<TARGET_FILE_DIR:r800zz_ai_worker>/rvm_mobilenetv3_fp32.onnx
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rvm_mobilenetv3_fp16.onnx")
    add_custom_command(TARGET r800zz_ai_worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/rvm_mobilenetv3_fp16.onnx"
            $<TARGET_FILE_DIR:r800zz_ai_worker>/rvm_mobilenetv3_fp16.onnx
    )
endif()

