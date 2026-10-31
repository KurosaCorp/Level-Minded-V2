@echo off
rem ===========================================================================================
rem  build_release.bat - builds an optimized release of LevelMinded and packages it.
rem
rem  Output:  dist\LevelMinded_v<major>.<minor>\      LevelMinded.exe, README.txt, license
rem           dist\LevelMinded_v<major>.<minor>.zip  the same folder, zipped for sharing
rem
rem  Steps:   clean configure ("dist" preset: Release + shipping options)
rem           full rebuild (/O2 /Ob3, LTCG, static runtime, no debug info)
rem           unit tests
rem           verify the exe: GUI subsystem (no console window), no debug symbols / PDB link,
rem             only Windows system DLLs (no Visual C++ runtime to install)
rem           package with `cmake --install`, mark the exe as a release image, zip
rem
rem  Needs Visual Studio 2022 or later with "Desktop development with C++" (finds it itself).
rem  Extra arguments go to CMake configure, e.g.:  build_release.bat -DLM_ENABLE_AVX2=OFF
rem  Set LM_NO_PAUSE=1 to never wait for a key press (automation).
rem ===========================================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "BUILD_DIR=build\dist"
set "EXE=%BUILD_DIR%\bin\LevelMinded.exe"

call :setup_msvc || goto :fail

echo.
echo [1/6] Configuring (clean)...
cmake --preset dist --fresh %* || goto :fail

echo.
echo [2/6] Building...
cmake --build --preset dist --clean-first || goto :fail

echo.
echo [3/6] Running unit tests...
ctest --test-dir "%BUILD_DIR%" --output-on-failure || goto :fail

echo.
echo [4/6] Verifying %EXE% ...
call :verify || goto :fail

set /p SHORT_VERSION=<"%BUILD_DIR%\package_version.txt"
if not defined SHORT_VERSION goto :fail
set "PACKAGE=dist\LevelMinded_v%SHORT_VERSION%"

echo.
echo [5/6] Packaging %PACKAGE% ...
if exist "%PACKAGE%" rmdir /s /q "%PACKAGE%"
if exist "%PACKAGE%" (
    echo   ERROR: could not remove the old %PACKAGE% folder. Is LevelMinded.exe still running from it?
    goto :fail
)
if exist "%PACKAGE%.zip" del /q "%PACKAGE%.zip"
cmake --install "%BUILD_DIR%" --prefix "%PACKAGE%" || goto :fail
rem Sets the PE checksum and marks the image as a release build.
editbin /nologo /release "%PACKAGE%\LevelMinded.exe" || goto :fail
if exist "%PACKAGE%\*.pdb" (
    echo   ERROR: debug symbol files found in the package.
    goto :fail
)

echo.
echo [6/6] Zipping...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%PACKAGE%\*' -DestinationPath '%PACKAGE%.zip' -Force" || goto :fail

echo.
echo ===========================================================================================
echo  Release ready: %CD%\%PACKAGE%
for %%F in ("%PACKAGE%\*") do echo    %%~nxF  [%%~zF bytes]
for %%F in ("%PACKAGE%.zip") do echo  Zip: %CD%\%%~F  [%%~zF bytes]
echo ===========================================================================================
exit /b 0


:fail
echo.
echo *** build_release.bat FAILED (see the messages above) ***
if not defined LM_NO_PAUSE pause
exit /b 1


rem -------------------------------------------------------------------------------------------
rem  Loads the MSVC x64 environment unless it is already active (Developer Command Prompt),
rem  and puts the CMake + Ninja that ship with Visual Studio on PATH.
rem -------------------------------------------------------------------------------------------
:setup_msvc
where cl.exe >nul 2>nul && where dumpbin.exe >nul 2>nul && where editbin.exe >nul 2>nul && goto :msvc_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio not found. Install Visual Studio 2022+ with "Desktop development with C++".
    exit /b 1
)
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo ERROR: no Visual Studio install with the MSVC x64 compiler was found.
    exit /b 1
)
rem vcvars64.bat calls vswhere by name.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
)
set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
:msvc_ready
where cmake.exe >nul 2>nul || (
    echo ERROR: CMake not found. Install the "C++ CMake tools for Windows" Visual Studio component.
    exit /b 1
)
exit /b 0


rem -------------------------------------------------------------------------------------------
rem  Checks the built exe: Windows GUI subsystem, no CodeView/PDB debug record, and no
rem  dependency on DLLs that are not part of Windows (e.g. the Visual C++ runtime).
rem -------------------------------------------------------------------------------------------
:verify
set "HEADERS=%BUILD_DIR%\exe_headers.txt"
set "DEPENDENTS=%BUILD_DIR%\exe_dependents.txt"
dumpbin /nologo /headers "%EXE%" > "%HEADERS%" || exit /b 1
dumpbin /nologo /dependents "%EXE%" > "%DEPENDENTS%" || exit /b 1

findstr /c:"subsystem (Windows GUI)" "%HEADERS%" >nul || (
    echo   ERROR: the exe is not a Windows GUI application; it would open a console window.
    exit /b 1
)
echo   OK  Windows GUI subsystem: no console window
findstr /r /c:" cv  *[0-9A-F]" "%HEADERS%" >nul && (
    echo   ERROR: the exe contains a debug record pointing at a PDB symbol file.
    exit /b 1
)
echo   OK  no debug symbols or PDB reference
findstr /i /r /c:"vcruntime" /c:"msvcp" /c:"ucrtbase" /c:"api-ms-win-crt" /c:"concrt" /c:"d3dx" "%DEPENDENTS%" >nul && (
    echo   ERROR: the exe needs a runtime DLL that Windows does not include:
    findstr /i /r /c:"vcruntime" /c:"msvcp" /c:"ucrtbase" /c:"api-ms-win-crt" /c:"concrt" /c:"d3dx" "%DEPENDENTS%"
    exit /b 1
)
echo   OK  depends only on Windows system DLLs:
for /f "tokens=1" %%d in ('findstr /i /r /c:"^ *[a-z0-9_]*\.dll" "%DEPENDENTS%"') do echo         %%d
exit /b 0
