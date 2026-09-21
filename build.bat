@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if not exist deps_env.bat (
  echo deps_env.bat was not found. Run setup_dependencies.bat first.
  exit /b 1
)
call deps_env.bat

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
if exist "!BUILD_DIR!" (
  rmdir /s /q "!BUILD_DIR!"
  if exist "!BUILD_DIR!" (
    echo ERROR: !BUILD_DIR! could not be removed. Close any r800zz_dlna_server.exe or r800zz_ai_worker.exe running from that folder and run build again.
    exit /b 1
  )
)

cmake -S . -B "!BUILD_DIR!" -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CUDA_COMPILER="!NVCC_EXE!" ^
  -DFFMPEG_ROOT="%FFMPEG_ROOT%" ^
  -DONNXRUNTIME_ROOT="%ONNXRUNTIME_ROOT%" ^
  -DR800ZZ_RUNTIME_DLLS="%R800ZZ_RUNTIME_DLLS%"
if errorlevel 1 exit /b 1

cmake --build "!BUILD_DIR!"
if errorlevel 1 exit /b 1

rem Runtime DLLs are authoritative in deps\runtime_dlls. Copy the whole bundle.
if not exist "!BUILD_DIR!\bin" mkdir "!BUILD_DIR!\bin"
copy /y "%R800ZZ_RUNTIME_DLLS%\*.dll" "!BUILD_DIR!\bin\" >nul
if errorlevel 1 (
  echo ERROR: Failed to copy runtime bundle to !BUILD_DIR!\bin.
  exit /b 1
)

echo.
echo Runtime check OK: CUDA EP runtime bundle copied beside the worker EXE.
echo Built: !BUILD_DIR!\bin\r800zz_dlna_server.exe
echo Worker: !BUILD_DIR!\bin\r800zz_ai_worker.exe
echo Model:  !BUILD_DIR!\bin\rvm_mobilenetv3_fp16.onnx
echo CPU Model: !BUILD_DIR!\bin\rvm_mobilenetv3_fp32.onnx
endlocal
