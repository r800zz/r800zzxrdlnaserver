@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "DEPS=%CD%\deps"
set "RUNTIME_DLLS=%DEPS%\runtime_dlls"
if exist "%CD%\deps_env.bat" call "%CD%\deps_env.bat"

if not defined FFMPEG_ROOT if exist "%DEPS%\ffmpeg-N-124714-g49a77d37be-win64-lgpl-shared\bin" set "FFMPEG_ROOT=%DEPS%\ffmpeg-N-124714-g49a77d37be-win64-lgpl-shared"
if not defined ONNXRUNTIME_ROOT if exist "%DEPS%\onnxruntime-win-x64-gpu-1.26.0\lib" set "ONNXRUNTIME_ROOT=%DEPS%\onnxruntime-win-x64-gpu-1.26.0"

if not defined CUDNN_BIN (
  for /f "delims=" %%F in ('dir /s /b /a-d "%DEPS%\cudnn-windows-x86_64-9.10.1.4_cuda12-archive\cudnn64_9.dll" 2^>nul') do if not defined CUDNN_DLL set "CUDNN_DLL=%%F"
  if defined CUDNN_DLL for %%F in ("!CUDNN_DLL!") do set "CUDNN_BIN=%%~dpF"
)

if not defined CUDA_ROOT_LOCAL if defined CUDA_PATH if exist "%CUDA_PATH%\bin\nvcc.exe" set "CUDA_ROOT_LOCAL=%CUDA_PATH%"
if not defined CUDA_ROOT_LOCAL (
  for /f "delims=" %%I in ('where nvcc.exe 2^>nul') do if not defined NVCC_FOR_RUNTIME set "NVCC_FOR_RUNTIME=%%I"
  if defined NVCC_FOR_RUNTIME for %%I in ("!NVCC_FOR_RUNTIME!\..\..") do set "CUDA_ROOT_LOCAL=%%~fI"
)
if not defined CUDA_ROOT_LOCAL if exist "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe" set "CUDA_ROOT_LOCAL=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"

if not defined FFMPEG_ROOT (
  echo ERROR: FFMPEG_ROOT could not be resolved.
  exit /b 1
)
if not defined ONNXRUNTIME_ROOT (
  echo ERROR: ONNXRUNTIME_ROOT could not be resolved.
  exit /b 1
)
if not defined CUDNN_BIN (
  echo ERROR: CUDNN_BIN could not be resolved.
  exit /b 1
)
if not defined CUDA_ROOT_LOCAL (
  echo ERROR: CUDA Toolkit root could not be resolved.
  exit /b 1
)

if exist "%RUNTIME_DLLS%" rmdir /s /q "%RUNTIME_DLLS%"
mkdir "%RUNTIME_DLLS%" || exit /b 1

echo Collecting CUDA-EP runtime DLLs into:
echo   %RUNTIME_DLLS%

rem Copy only the FFmpeg shared libraries linked by CMakeLists.txt.  The shared
rem package also contains large unused libraries such as avfilter/avdevice.
call :copy_required_pattern "%FFMPEG_ROOT%\bin\avformat-*.dll" "FFmpeg avformat" || exit /b 1
call :copy_required_pattern "%FFMPEG_ROOT%\bin\avcodec-*.dll" "FFmpeg avcodec" || exit /b 1
call :copy_required_pattern "%FFMPEG_ROOT%\bin\avutil-*.dll" "FFmpeg avutil" || exit /b 1
call :copy_required_pattern "%FFMPEG_ROOT%\bin\swresample-*.dll" "FFmpeg swresample" || exit /b 1
call :copy_required_pattern "%FFMPEG_ROOT%\bin\swscale-*.dll" "FFmpeg swscale" || exit /b 1

rem The application uses CUDA EP only.  Do not include the unused TensorRT
rem provider DLL that may also be present in the ONNX Runtime GPU package.
call :copy_required_pattern "%ONNXRUNTIME_ROOT%\lib\onnxruntime.dll" "ONNX Runtime" || exit /b 1
call :copy_required_pattern "%ONNXRUNTIME_ROOT%\lib\onnxruntime_providers_shared.dll" "ONNX Runtime shared provider" || exit /b 1
call :copy_required_pattern "%ONNXRUNTIME_ROOT%\lib\onnxruntime_providers_cuda.dll" "ONNX Runtime CUDA provider" || exit /b 1
call :copy_all "%CUDNN_BIN%" "cuDNN" || exit /b 1

rem Do not copy the entire CUDA bin directory.  It contains development and
rem unrelated runtime libraries (NPP, cuSOLVER, cuSPARSE, nvJPEG, tools, etc.)
rem which make the installer several gigabytes larger.  ONNX Runtime CUDA EP
rem and the native CUDA kernels in this project need only this runtime set.
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\cudart64_*.dll" "CUDA runtime" || exit /b 1
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\cublas64_*.dll" "cuBLAS" || exit /b 1
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\cublasLt64_*.dll" "cuBLAS Lt" || exit /b 1
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\cufft64_*.dll" "cuFFT" || exit /b 1
rem dumpbin confirms that ONNX Runtime CUDA EP does not import cuRAND.
rem Use the normal NVRTC DLL only; the .alt.dll file is an alternative build,
rem not an additional dependency.
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\nvrtc64_120_0.dll" "NVRTC" || exit /b 1
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\nvrtc-builtins64_*.dll" "NVRTC builtins" || exit /b 1
call :copy_required_pattern "%CUDA_ROOT_LOCAL%\bin\nvJitLink_*.dll" "NVJitLink" || exit /b 1

rem These are dependency helpers in some CUDA 12.x patch releases.  Copy them
rem when present, but do not fail when that CUDA installation does not ship
rem them as separate DLLs.
call :copy_optional_pattern "%CUDA_ROOT_LOCAL%\bin\nvfatbin_*.dll"
call :copy_optional_pattern "%CUDA_ROOT_LOCAL%\bin\zlibwapi.dll"

set "MISSING="
for %%D in (
  onnxruntime.dll
  onnxruntime_providers_shared.dll
  onnxruntime_providers_cuda.dll
  cudnn64_9.dll
  cudart64_12.dll
  cublas64_12.dll
  cublasLt64_12.dll
) do (
  if not exist "%RUNTIME_DLLS%\%%D" set "MISSING=!MISSING! %%D"
)
if defined MISSING (
  echo ERROR: CUDA EP runtime bundle is incomplete. Missing:!MISSING!
  echo FFMPEG_ROOT=%FFMPEG_ROOT%
  echo ONNXRUNTIME_ROOT=%ONNXRUNTIME_ROOT%
  echo CUDNN_BIN=%CUDNN_BIN%
  echo CUDA_ROOT_LOCAL=%CUDA_ROOT_LOCAL%
  exit /b 1
)

for /f %%N in ('dir /b /a-d "%RUNTIME_DLLS%\*.dll" 2^>nul ^| find /c /v ""') do set "DLL_COUNT=%%N"
echo CUDA EP runtime bundle OK: !DLL_COUNT! DLLs collected.
endlocal & exit /b 0

:copy_all
if not exist "%~1\*.dll" (
  echo ERROR: %~2 DLL directory is missing or empty: %~1
  exit /b 1
)
copy /y "%~1\*.dll" "%RUNTIME_DLLS%\" >nul
if errorlevel 1 (
  echo ERROR: Failed copying %~2 DLLs from: %~1
  exit /b 1
)
exit /b 0

:copy_required_pattern
set "PATTERN_FOUND="
for /f "delims=" %%F in ('dir /b /s /a-d "%~1" 2^>nul') do (
  copy /y "%%F" "%RUNTIME_DLLS%\" >nul
  if errorlevel 1 (
    echo ERROR: Failed copying %~2 DLL: %%F
    exit /b 1
  )
  set "PATTERN_FOUND=1"
)
if not defined PATTERN_FOUND (
  echo ERROR: Required %~2 DLL was not found: %~1
  exit /b 1
)
exit /b 0

:copy_optional_pattern
for /f "delims=" %%F in ('dir /b /s /a-d "%~1" 2^>nul') do (
  copy /y "%%F" "%RUNTIME_DLLS%\" >nul
  if errorlevel 1 (
    echo ERROR: Failed copying optional CUDA dependency: %%F
    exit /b 1
  )
)
exit /b 0
