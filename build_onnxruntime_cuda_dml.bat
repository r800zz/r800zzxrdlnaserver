@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ORT_VERSION=1.26.0"
set "DEPS=%CD%\deps"
set "ORT_SRC=%DEPS%\onnxruntime-src-%ORT_VERSION%"
set "ORT_BUILD=%DEPS%\onnxruntime-build-cuda-dml-%ORT_VERSION%"
set "ORT_ROOT=%DEPS%\onnxruntime-win-x64-cuda-dml-%ORT_VERSION%"

if exist "%ORT_ROOT%\include\dml_provider_factory.h" if exist "%ORT_ROOT%\lib\onnxruntime.dll" if exist "%ORT_ROOT%\lib\onnxruntime_providers_cuda.dll" (
  echo Unified CUDA+DirectML ONNX Runtime already exists: %ORT_ROOT%
  endlocal & set "ONNXRUNTIME_ROOT=%ORT_ROOT%" & exit /b 0
)

where git.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: git.exe is required to build unified ONNX Runtime.
  exit /b 1
)
where python.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: python.exe is required to build unified ONNX Runtime.
  exit /b 1
)
if not defined CUDA_PATH (
  echo ERROR: CUDA_PATH is not set.
  exit /b 1
)

set "CUDNN_HOME="
if defined CUDNN_ROOT if exist "%CUDNN_ROOT%\include\cudnn.h" set "CUDNN_HOME=%CUDNN_ROOT%"
if not defined CUDNN_HOME (
  for /f "delims=" %%F in ('dir /s /b /a-d "%DEPS%\cudnn.h" 2^>nul') do if not defined CUDNN_HEADER set "CUDNN_HEADER=%%F"
  if defined CUDNN_HEADER for %%F in ("!CUDNN_HEADER!") do for %%D in ("%%~dpF..") do set "CUDNN_HOME=%%~fD"
)
if not defined CUDNN_HOME (
  echo ERROR: cuDNN root could not be resolved. Expected include\cudnn.h under deps.
  exit /b 1
)

echo.
echo Building one ONNX Runtime with CUDA + DirectML EPs...
echo CUDA:  %CUDA_PATH%
echo cuDNN: !CUDNN_HOME!

if not exist "%ORT_SRC%\.git" (
  if exist "%ORT_SRC%" rmdir /s /q "%ORT_SRC%"
  git clone --branch v%ORT_VERSION% --depth 1 --recurse-submodules --shallow-submodules https://github.com/microsoft/onnxruntime.git "%ORT_SRC%"
  if errorlevel 1 exit /b 1
) else (
  pushd "%ORT_SRC%"
  git submodule update --init --recursive
  if errorlevel 1 (popd & exit /b 1)
  popd
)

rem Keep the ONNX Runtime build tree so interrupted/previous builds can resume incrementally.
rem ONNX Runtime only performs an explicit clean when --clean is requested; we do not use --clean here.
if not exist "%ORT_BUILD%" mkdir "%ORT_BUILD%" || exit /b 1
call "%ORT_SRC%\build.bat" ^
  --config Release ^
  --build_shared_lib ^
  --parallel ^
  --skip_tests ^
  --use_cuda ^
  --cuda_home "%CUDA_PATH%" ^
  --cudnn_home "!CUDNN_HOME!" ^
  --use_dml ^
  --build_dir "%ORT_BUILD%"
if errorlevel 1 exit /b 1

if exist "%ORT_ROOT%" rmdir /s /q "%ORT_ROOT%"
mkdir "%ORT_ROOT%\include" || exit /b 1
mkdir "%ORT_ROOT%\lib" || exit /b 1

rem Public C/C++ session headers used by this project.
copy /y "%ORT_SRC%\include\onnxruntime\core\session\*.h" "%ORT_ROOT%\include\" >nul || exit /b 1
copy /y "%ORT_SRC%\include\onnxruntime\core\providers\dml\dml_provider_factory.h" "%ORT_ROOT%\include\dml_provider_factory.h" >nul || exit /b 1
call :copy_first "DirectML.h" "%ORT_ROOT%\include\DirectML.h" required || exit /b 1

call :copy_first "onnxruntime.lib" "%ORT_ROOT%\lib\onnxruntime.lib" required || exit /b 1
call :copy_first "onnxruntime.dll" "%ORT_ROOT%\lib\onnxruntime.dll" required || exit /b 1
call :copy_first "onnxruntime_providers_shared.dll" "%ORT_ROOT%\lib\onnxruntime_providers_shared.dll" required || exit /b 1
call :copy_first "onnxruntime_providers_cuda.dll" "%ORT_ROOT%\lib\onnxruntime_providers_cuda.dll" required || exit /b 1
call :copy_first "DirectML.dll" "%ORT_ROOT%\lib\DirectML.dll" optional
call :copy_first "onnxruntime_providers_dml.dll" "%ORT_ROOT%\lib\onnxruntime_providers_dml.dll" optional

echo Unified CUDA+DirectML ONNX Runtime ready: %ORT_ROOT%
endlocal & set "ONNXRUNTIME_ROOT=%ORT_ROOT%" & exit /b 0

:copy_first
set "NAME=%~1"
set "DEST=%~2"
set "MODE=%~3"
set "FOUND="
for /f "delims=" %%F in ('dir /s /b /a-d "%ORT_BUILD%\%NAME%" 2^>nul') do if not defined FOUND set "FOUND=%%F"
if not defined FOUND (
  if /i "%MODE%"=="required" (
    echo ERROR: Unified ONNX Runtime build did not produce %NAME%.
    exit /b 1
  )
  exit /b 0
)
copy /y "!FOUND!" "%DEST%" >nul || exit /b 1
exit /b 0
