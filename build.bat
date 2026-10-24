@echo off
rem Build NovaStar64 on Windows with one command: double-click this file or run build.bat.
rem   build.bat        build it; the program ends up in nova64\build\RelWithDebInfo\nova64.exe
rem   build.bat run    build it, then open the launcher
rem Needs CMake and Visual Studio (or the Build Tools) with the "Desktop development
rem with C++" workload; the first build downloads about a dozen libraries.

setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found. Install it with:  winget install Kitware.CMake
    echo then open a new terminal and run build.bat again.
    exit /b 1
)

if not exist softfloat\source\include\softfloat.h (
    where git >nul 2>nul && git submodule update --init --recursive
)

echo.
echo ==^> Configuring ^(the first time this downloads the libraries^)
cmake -S nova64 -B nova64\build
if errorlevel 1 (
    echo.
    echo Configuring failed. If it says no C++ compiler was found, install Visual Studio's C++ tools:
    echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --passive"
    exit /b 1
)

echo.
echo ==^> Building ^(the first build takes a few minutes^)
cmake --build nova64\build --config RelWithDebInfo --parallel
if errorlevel 1 (
    echo.
    echo The build failed; the errors are above.
    exit /b 1
)

echo.
echo Done: nova64\build\RelWithDebInfo\nova64.exe
if /i "%~1"=="run" start "" "nova64\build\RelWithDebInfo\nova64.exe"
endlocal
