@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if not exist deps_env.bat (
  echo deps_env.bat was not found. Run setup_dependencies.bat first.
  exit /b 1
)
call deps_env.bat

rem One executable owns CUDA / DirectML / CPU. There is no DML worker build.
rem The unified CUDA+DML ONNX Runtime must be prepared separately. Normal builds
rem never start the expensive ONNX Runtime source build automatically.
set "UNIFIED_ORT=%CD%\deps\onnxruntime-win-x64-cuda-dml-1.26.0"
if not exist "!UNIFIED_ORT!\include\dml_provider_factory.h" (
  echo ERROR: Unified CUDA+DML ONNX Runtime is not ready.
  echo Run build_onnxruntime_cuda_dml.bat once, then run build.bat again.
  exit /b 1
)
set "ONNXRUNTIME_ROOT=!UNIFIED_ORT!"

rem Never build with a previously patched TensorRT model. Always restore the
rem official RVM ONNX downloaded by setup_dependencies.bat.
if not exist "%CD%\deps\rvm_mobilenetv3_fp16_official.onnx" (
  echo ERROR: Official RVM ONNX is missing. Run setup_dependencies.bat first.
  exit /b 1
)
copy /y "%CD%\deps\rvm_mobilenetv3_fp16_official.onnx" "%CD%\rvm_mobilenetv3_fp16.onnx" >nul || exit /b 1
if not exist "%CD%\deps\rvm_mobilenetv3_fp32_official.onnx" (
  echo Downloading official RVM MobileNetV3 FP32 ONNX model for CPU conversion...
  curl.exe -L --fail --retry 3 --retry-delay 2 -o "%CD%\deps\rvm_mobilenetv3_fp32_official.onnx" "https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx" || exit /b 1
)
copy /y "%CD%\deps\rvm_mobilenetv3_fp32_official.onnx" "%CD%\rvm_mobilenetv3_fp32.onnx" >nul || exit /b 1

call "%CD%\collect_runtime_dlls.bat"
if errorlevel 1 exit /b 1

rem collect_runtime_dlls.bat may still collect an older CUDA-only ORT set.
rem Make the unified CUDA+DML ORT authoritative inside the runtime bundle before
rem CMake/post-build steps can copy that bundle anywhere.
for %%D in (onnxruntime.dll onnxruntime_providers_shared.dll onnxruntime_providers_cuda.dll) do (
  if not exist "!UNIFIED_ORT!\lib\%%D" (
    echo ERROR: Unified ONNX Runtime file is missing: !UNIFIED_ORT!\lib\%%D
    exit /b 1
  )
  copy /y "!UNIFIED_ORT!\lib\%%D" "%R800ZZ_RUNTIME_DLLS%\%%D" >nul
  if errorlevel 1 (
    echo ERROR: Failed to normalize runtime bundle file: %%D
    exit /b 1
  )
)

where nvcc.exe >nul 2>nul
if errorlevel 1 (
  echo nvcc.exe was not found.
  echo Add CUDA Toolkit bin to PATH, for example:
  echo   set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin;%%PATH%%"
  exit /b 1
)
for /f "delims=" %%I in ('where nvcc.exe') do if not defined NVCC_EXE set "NVCC_EXE=%%I"

where cmake.exe >nul 2>nul
if errorlevel 1 (
  echo cmake.exe was not found.
  exit /b 1
)

rem NMake uses nvcc.exe + cl.exe directly and does not require the
rem Visual Studio CUDA BuildCustomizations/toolset integration.
where cl.exe >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" (
    echo Visual Studio C++ Build Tools were not found ^(vswhere.exe missing^).
    exit /b 1
  )
  for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
  if not defined VS_INSTALL (
    echo Visual Studio C++ x64 build tools were not found.
    exit /b 1
  )
  if not exist "!VS_INSTALL!\VC\Auxiliary\Build\vcvars64.bat" (
    echo vcvars64.bat was not found under !VS_INSTALL!.
    exit /b 1
  )
  call "!VS_INSTALL!\VC\Auxiliary\Build\vcvars64.bat"
  if errorlevel 1 exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo cl.exe was not found after initializing Visual Studio build tools.
  exit /b 1
)
where nmake.exe >nul 2>nul
if errorlevel 1 (
  echo nmake.exe was not found after initializing Visual Studio build tools.
  exit /b 1
)

echo NVCC: !NVCC_EXE!
for /f "delims=" %%I in ('where cl.exe') do if not defined CL_EXE set "CL_EXE=%%I"
echo CL:   !CL_EXE!

echo.
echo Configuring with NMake ^(no Visual Studio CUDA toolset integration required^)...
set "BUILD_DIR=build_cuda_ep"
rem Keep the existing CMake/NMake build tree. This preserves object files and
rem dependency timestamps so only changed sources are recompiled.

cmake -S . -B "!BUILD_DIR!" -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CUDA_COMPILER="!NVCC_EXE!" ^
  -DFFMPEG_ROOT="%FFMPEG_ROOT%" ^
  -DONNXRUNTIME_ROOT="%ONNXRUNTIME_ROOT%" ^
  -DR800ZZ_RUNTIME_DLLS="%R800ZZ_RUNTIME_DLLS%"
if errorlevel 1 exit /b 1

cmake --build "!BUILD_DIR!"
if errorlevel 1 exit /b 1

rem Runtime DLLs are authoritative in deps\runtime_dlls. Remove only stale
rem ONNX Runtime core/CUDA DLLs first so an incompatible older set cannot survive.
if not exist "!BUILD_DIR!\bin" mkdir "!BUILD_DIR!\bin"
for %%D in (onnxruntime.dll onnxruntime_providers_shared.dll onnxruntime_providers_cuda.dll) do (
  if exist "!BUILD_DIR!\bin\%%D" del /f /q "!BUILD_DIR!\bin\%%D"
)
copy /y "%R800ZZ_RUNTIME_DLLS%\*.dll" "!BUILD_DIR!\bin\" >nul
if errorlevel 1 (
  echo ERROR: Failed to copy runtime bundle to !BUILD_DIR!\bin.
  exit /b 1
)

rem Final authority: no later build step is allowed to leave an older ORT beside
rem the worker. Copy directly from the unified package after every other copy.
for %%D in (onnxruntime.dll onnxruntime_providers_shared.dll onnxruntime_providers_cuda.dll) do (
  copy /y "!UNIFIED_ORT!\lib\%%D" "!BUILD_DIR!\bin\%%D" >nul
  if errorlevel 1 (
    echo ERROR: Failed to deploy unified ONNX Runtime file: %%D
    exit /b 1
  )
  fc /b "!UNIFIED_ORT!\lib\%%D" "!BUILD_DIR!\bin\%%D" >nul
  if errorlevel 1 (
    echo ERROR: Wrong ONNX Runtime file remains after build: %%D
    echo Source: !UNIFIED_ORT!\lib\%%D
    echo Target: !BUILD_DIR!\bin\%%D
    exit /b 1
  )
)

echo.
echo Runtime check OK: unified CUDA + DirectML ONNX Runtime verified byte-for-byte beside the worker EXE.
echo Built: !BUILD_DIR!\bin\r800zz_dlna_server.exe
echo Worker: !BUILD_DIR!\bin\r800zz_ai_worker.exe
echo Model:  !BUILD_DIR!\bin\rvm_mobilenetv3_fp16.onnx
echo CPU Model: !BUILD_DIR!\bin\rvm_mobilenetv3_fp32.onnx
endlocal
