@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "DEPS=%CD%\deps"
set "RUNTIME_DLLS=%DEPS%\runtime_dlls"
if not exist "%DEPS%" mkdir "%DEPS%"
if not exist "%RUNTIME_DLLS%" mkdir "%RUNTIME_DLLS%"

rem ONNX Runtime 1.26 GPU is built for CUDA 12.8 + cuDNN 9.x.
set "ORT_VER=1.26.0"
set "ORT_DIR=%DEPS%\onnxruntime-win-x64-gpu-%ORT_VER%"
set "ORT_ZIP=%DEPS%\onnxruntime-win-x64-gpu-%ORT_VER%.zip"
if not exist "%ORT_DIR%\include\onnxruntime_cxx_api.h" (
  echo Downloading ONNX Runtime GPU %ORT_VER%...
  curl.exe -L --fail --retry 3 --retry-delay 2 -o "%ORT_ZIP%" "https://github.com/microsoft/onnxruntime/releases/download/v%ORT_VER%/onnxruntime-win-x64-gpu-%ORT_VER%.zip" || exit /b 1
  powershell -NoProfile -Command "Expand-Archive -Force -Path '%ORT_ZIP%' -DestinationPath '%DEPS%'" || exit /b 1
)

rem Pin FFmpeg to the 2026-05-31 BtbN LGPL shared build.
rem That build predates nv-codec-headers SDK 13.1 (2026-06-09), so NVENC
rem is compiled against API 13.0 instead of 13.1. Do not replace this with
rem the floating master-latest package unless the minimum driver requirement
rem is intentionally raised.
set "FF_BUILD=N-124714-g49a77d37be"
set "FF_DIR=%DEPS%\ffmpeg-%FF_BUILD%-win64-lgpl-shared"
set "FF_ZIP=%DEPS%\ffmpeg-%FF_BUILD%-win64-lgpl-shared.zip"
set "FF_URL=https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-05-31-13-22/ffmpeg-%FF_BUILD%-win64-lgpl-shared.zip"
if not exist "%FF_DIR%\include\libavformat\avformat.h" (
  echo Downloading pinned FFmpeg LGPL shared build ^(NVENC API 13.0 generation^)...
  echo   %FF_URL%
  curl.exe -L --fail --retry 3 --retry-delay 2 -o "%FF_ZIP%" "%FF_URL%" || exit /b 1
  powershell -NoProfile -Command "Expand-Archive -Force -Path '%FF_ZIP%' -DestinationPath '%DEPS%'" || exit /b 1
)
if not exist "%FF_DIR%\include\libavformat\avformat.h" (
  echo ERROR: Pinned FFmpeg package did not extract to the expected directory:
  echo   %FF_DIR%
  exit /b 1
)
echo FFmpeg pinned: %FF_BUILD% ^(NVENC API 13.0 generation; minimum NVIDIA driver 570.0^)

rem Use the official unmodified RVM ONNX model. No Python/model rewriting.
set "RVM_STOCK=%DEPS%\rvm_mobilenetv3_fp16_official.onnx"
set "RVM_FINAL=%CD%\rvm_mobilenetv3_fp16.onnx"
set "RVM_CPU_STOCK=%DEPS%\rvm_mobilenetv3_fp32_official.onnx"
set "RVM_CPU_FINAL=%CD%\rvm_mobilenetv3_fp32.onnx"
if not exist "%RVM_STOCK%" (
  echo Downloading official RVM MobileNetV3 FP16 ONNX model...
  curl.exe -L --fail --retry 3 --retry-delay 2 -o "%RVM_STOCK%" "https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp16.onnx" || exit /b 1
)
copy /y "%RVM_STOCK%" "%RVM_FINAL%" >nul || exit /b 1
if not exist "%RVM_CPU_STOCK%" (
  echo Downloading official RVM MobileNetV3 FP32 ONNX model...
  curl.exe -L --fail --retry 3 --retry-delay 2 -o "%RVM_CPU_STOCK%" "https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx" || exit /b 1
)
copy /y "%RVM_CPU_STOCK%" "%RVM_CPU_FINAL%" >nul || exit /b 1
echo RVM models: official unmodified FP16 ^(CUDA EP^) and FP32 ^(CPU EP^)

rem Install cuDNN from NVIDIA's official Windows redistributable ZIP.
rem This does not use Python or pip.
set "CUDNN_VER=9.10.1.4"
set "CUDNN_ZIP=%DEPS%\cudnn-windows-x86_64-%CUDNN_VER%_cuda12-archive.zip"
set "CUDNN_ROOT=%DEPS%\cudnn-windows-x86_64-%CUDNN_VER%_cuda12-archive"
set "CUDNN_URL=https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/windows-x86_64/cudnn-windows-x86_64-%CUDNN_VER%_cuda12-archive.zip"
if not exist "%CUDNN_ROOT%" (
  echo Downloading NVIDIA cuDNN %CUDNN_VER% for CUDA 12.x...
  curl.exe -L --fail --retry 3 --retry-delay 2 -o "%CUDNN_ZIP%" "%CUDNN_URL%" || exit /b 1
  powershell -NoProfile -Command "Expand-Archive -Force -Path '%CUDNN_ZIP%' -DestinationPath '%DEPS%'" || exit /b 1
)
set "CUDNN_DLL="
for /f "delims=" %%F in ('dir /s /b /a-d "%CUDNN_ROOT%\cudnn64_9.dll" 2^>nul') do if not defined CUDNN_DLL set "CUDNN_DLL=%%F"
if not defined CUDNN_DLL (
  echo ERROR: cudnn64_9.dll was not found under %CUDNN_ROOT%.
  exit /b 1
)
for %%F in ("!CUDNN_DLL!") do set "CUDNN_BIN=%%~dpF"
echo cuDNN runtime: !CUDNN_BIN!

rem Resolve CUDA Toolkit. CUDA Toolkit is needed both for nvcc and runtime DLLs.
set "CUDA_ROOT_LOCAL="
if defined CUDA_PATH if exist "%CUDA_PATH%\bin\nvcc.exe" set "CUDA_ROOT_LOCAL=%CUDA_PATH%"
if not defined CUDA_ROOT_LOCAL (
  for /f "delims=" %%I in ('where nvcc.exe 2^>nul') do if not defined NVCC_FOUND set "NVCC_FOUND=%%I"
  if defined NVCC_FOUND for %%I in ("!NVCC_FOUND!\..\..") do set "CUDA_ROOT_LOCAL=%%~fI"
)
if not defined CUDA_ROOT_LOCAL if exist "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe" set "CUDA_ROOT_LOCAL=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
if not defined CUDA_ROOT_LOCAL (
  echo ERROR: CUDA Toolkit 12.8 was not found.
  exit /b 1
)

> "%CD%\deps_env.bat" echo @echo off
>>"%CD%\deps_env.bat" echo set "FFMPEG_ROOT=%FF_DIR%"
>>"%CD%\deps_env.bat" echo set "ONNXRUNTIME_ROOT=%ORT_DIR%"
>>"%CD%\deps_env.bat" echo set "R800ZZ_RUNTIME_DLLS=%RUNTIME_DLLS%"
>>"%CD%\deps_env.bat" echo set "CUDNN_BIN=!CUDNN_BIN!"
>>"%CD%\deps_env.bat" echo set "CUDA_ROOT_LOCAL=%CUDA_ROOT_LOCAL%"

call "%CD%\collect_runtime_dlls.bat"
if errorlevel 1 exit /b 1

echo.
echo Dependencies ready.
echo FFMPEG_ROOT=%FF_DIR%
echo ONNXRUNTIME_ROOT=%ORT_DIR%
echo CUDNN_BIN=!CUDNN_BIN!
echo CUDA_ROOT_LOCAL=%CUDA_ROOT_LOCAL%
echo RUNTIME_DLLS=%RUNTIME_DLLS%
echo Python: NOT USED
echo TensorRT: NOT USED
endlocal
