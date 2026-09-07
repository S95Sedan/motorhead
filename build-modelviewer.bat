@echo off
setlocal EnableExtensions EnableDelayedExpansion
pushd "%~dp0"

set "TOOLS_ROOT=%CD%\.tools"
set "DOWNLOAD_ROOT=!TOOLS_ROOT!\downloads"

echo [1/3] Preparing pinned CMake, Ninja, and LLVM-MinGW tools...
call :prepare_tools
if errorlevel 1 goto :failed

set "CMAKE=!TOOLS_ROOT!\cmake\bin\cmake.exe"

echo [2/3] Configuring the Model Viewer...
pushd "tools\modelviewer"
"!CMAKE!" --preset windows-release
if errorlevel 1 (
    popd
    goto :failed
)

echo [3/3] Building Modelviewer.exe...
"!CMAKE!" --build --preset windows-release --parallel 1
if errorlevel 1 (
    popd
    goto :failed
)
popd

echo.
echo Build complete:
echo   %CD%\bin\Modelviewer.exe
popd
pause
exit /b 0

:failed
set "BUILD_EXIT=!ERRORLEVEL!"
if "!BUILD_EXIT!"=="0" set "BUILD_EXIT=1"
echo.
echo Model Viewer build failed with exit code !BUILD_EXIT!.
popd
pause
exit /b !BUILD_EXIT!

:prepare_tools
if exist "!TOOLS_ROOT!\cmake\bin\cmake.exe" if exist "!TOOLS_ROOT!\ninja\ninja.exe" if exist "!TOOLS_ROOT!\llvm-mingw\bin\x86_64-w64-mingw32-clang.exe" (
    echo Pinned build tools are ready under .tools.
    exit /b 0
)

where curl.exe >nul 2>nul
if errorlevel 1 (
    echo Required Windows tool was not found: curl.exe
    exit /b 1
)
where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo Required Windows tool was not found: powershell.exe
    exit /b 1
)
where certutil.exe >nul 2>nul
if errorlevel 1 (
    echo Required Windows tool was not found: certutil.exe
    exit /b 1
)

if not exist "!TOOLS_ROOT!" mkdir "!TOOLS_ROOT!"
if errorlevel 1 exit /b 1
if not exist "!DOWNLOAD_ROOT!" mkdir "!DOWNLOAD_ROOT!"
if errorlevel 1 exit /b 1

set "TOOL_ID=cmake"
set "TOOL_VERSION=4.3.3"
set "TOOL_ARCHIVE=cmake-4.3.3-windows-x86_64.zip"
set "TOOL_URL=https://github.com/Kitware/CMake/releases/download/v4.3.3/cmake-4.3.3-windows-x86_64.zip"
set "TOOL_HASH=935ADE9E5E8723583C07F44C5592CEA2A1C8F65C56CA7E07B34C025C880E0BD6"
set "TOOL_ROOT_DIR=cmake-4.3.3-windows-x86_64"
set "TOOL_SENTINEL=bin\cmake.exe"
call :ensure_tool
if errorlevel 1 exit /b 1

set "TOOL_ID=ninja"
set "TOOL_VERSION=1.13.2"
set "TOOL_ARCHIVE=ninja-win-1.13.2.zip"
set "TOOL_URL=https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip"
set "TOOL_HASH=07FC8261B42B20E71D1720B39068C2E14FFCEE6396B76FB7A795FB460B78DC65"
set "TOOL_ROOT_DIR=."
set "TOOL_SENTINEL=ninja.exe"
call :ensure_tool
if errorlevel 1 exit /b 1

set "TOOL_ID=llvm-mingw"
set "TOOL_VERSION=20260616"
set "TOOL_ARCHIVE=llvm-mingw-20260616-ucrt-x86_64.zip"
set "TOOL_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-x86_64.zip"
set "TOOL_HASH=B9B68A4D276E16FA25802AABA458E4638F64B3884C290AACCDC2D87083B6CA35"
set "TOOL_ROOT_DIR=llvm-mingw-20260616-ucrt-x86_64"
set "TOOL_SENTINEL=bin\x86_64-w64-mingw32-clang.exe"
call :ensure_tool
if errorlevel 1 exit /b 1

echo Pinned build tools are ready under .tools.
exit /b 0

:ensure_tool
set "TOOL_DEST=!TOOLS_ROOT!\!TOOL_ID!"
set "TOOL_READY=!TOOL_DEST!\!TOOL_SENTINEL!"
set "ARCHIVE_PATH=!DOWNLOAD_ROOT!\!TOOL_ARCHIVE!"
set "TEMP_ARCHIVE=!ARCHIVE_PATH!.download"
set "EXTRACT_TEMP=!TOOLS_ROOT!\extract-!TOOL_ID!"
if exist "!TOOL_READY!" (
    echo !TOOL_ID! !TOOL_VERSION!: ready
    exit /b 0
)

if exist "!ARCHIVE_PATH!" (
    call :verify_archive
    if errorlevel 1 (
        echo !TOOL_ID! !TOOL_VERSION!: cached archive failed verification; downloading again
        del /F /Q "!ARCHIVE_PATH!" >nul 2>nul
    )
)

if not exist "!ARCHIVE_PATH!" (
    if exist "!TEMP_ARCHIVE!" del /F /Q "!TEMP_ARCHIVE!" >nul 2>nul
    echo !TOOL_ID! !TOOL_VERSION!: downloading
    curl.exe --fail --location --retry 3 --output "!TEMP_ARCHIVE!" "!TOOL_URL!"
    if errorlevel 1 (
        del /F /Q "!TEMP_ARCHIVE!" >nul 2>nul
        echo Download failed: !TOOL_URL!
        exit /b 1
    )
    move /Y "!TEMP_ARCHIVE!" "!ARCHIVE_PATH!" >nul
    if errorlevel 1 exit /b 1
)

call :verify_archive
if errorlevel 1 (
    del /F /Q "!ARCHIVE_PATH!" >nul 2>nul
    exit /b 1
)

if exist "!TOOL_DEST!" rmdir /S /Q "!TOOL_DEST!"
if exist "!EXTRACT_TEMP!" rmdir /S /Q "!EXTRACT_TEMP!"
mkdir "!TOOL_DEST!"
if errorlevel 1 exit /b 1
mkdir "!EXTRACT_TEMP!"
if errorlevel 1 (
    rmdir /S /Q "!TOOL_DEST!" >nul 2>nul
    exit /b 1
)

echo !TOOL_ID! !TOOL_VERSION!: extracting
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath '!ARCHIVE_PATH!' -DestinationPath '!EXTRACT_TEMP!' -Force"
if errorlevel 1 (
    rmdir /S /Q "!EXTRACT_TEMP!" >nul 2>nul
    rmdir /S /Q "!TOOL_DEST!" >nul 2>nul
    echo Archive extraction failed: !ARCHIVE_PATH!
    exit /b 1
)

if "!TOOL_ROOT_DIR!"=="." (
    set "EXTRACT_SOURCE=!EXTRACT_TEMP!"
) else (
    set "EXTRACT_SOURCE=!EXTRACT_TEMP!\!TOOL_ROOT_DIR!"
)

if not exist "!EXTRACT_SOURCE!" (
    echo Expected archive directory was not found: !EXTRACT_SOURCE!
    rmdir /S /Q "!EXTRACT_TEMP!" >nul 2>nul
    rmdir /S /Q "!TOOL_DEST!" >nul 2>nul
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop'; Get-ChildItem -LiteralPath '!EXTRACT_SOURCE!' -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination '!TOOL_DEST!' -Recurse -Force }"
if errorlevel 1 (
    rmdir /S /Q "!EXTRACT_TEMP!" >nul 2>nul
    rmdir /S /Q "!TOOL_DEST!" >nul 2>nul
    echo Failed to copy !TOOL_ID! files into place.
    exit /b 1
)

rmdir /S /Q "!EXTRACT_TEMP!" >nul 2>nul

if not exist "!TOOL_READY!" (
    echo Tool installation did not produce: !TOOL_READY!
    rmdir /S /Q "!TOOL_DEST!" >nul 2>nul
    exit /b 1
)
exit /b 0

:verify_archive
set "ACTUAL_HASH="
for /F "skip=1 tokens=* delims=" %%H in ('certutil.exe -hashfile "!ARCHIVE_PATH!" SHA256 2^>nul') do if not defined ACTUAL_HASH set "ACTUAL_HASH=%%H"
set "ACTUAL_HASH=!ACTUAL_HASH: =!"
if /I not "!ACTUAL_HASH!"=="!TOOL_HASH!" (
    echo Hash mismatch for !TOOL_ARCHIVE!.
    echo Expected: !TOOL_HASH!
    echo Actual:   !ACTUAL_HASH!
    exit /b 1
)
exit /b 0
