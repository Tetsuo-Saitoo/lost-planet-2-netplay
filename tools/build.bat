@echo off
rem Build xlive.dll and LP2NetSetup.exe (x86 Release) into build\. Uses the VS 2022 Build Tools' bundled CMake and Ninja.
setlocal
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
rem Elsewhere (CI runners, other editions): the newest Visual Studio with the x86 C++ tools
if not exist "%VS%\VC\Auxiliary\Build\vcvars32.bat" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build build || exit /b 1
echo BUILD OK: %CD%\build\xlive.dll, %CD%\build\LP2NetSetup.exe
