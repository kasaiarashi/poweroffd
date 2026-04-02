@echo off
REM Build poweroffd for Windows using MSVC
REM Run from a "Developer Command Prompt for VS" or after running vcvarsall.bat

echo Building poweroffd for Windows...

if not exist build mkdir build
cd build

cmake .. -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo.
    echo CMake configuration failed. Trying NMake...
    cd ..
    rmdir /s /q build 2>nul
    mkdir build
    cd build
    cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 (
        echo.
        echo CMake failed. Trying direct compilation...
        cd ..
        rmdir /s /q build 2>nul

        echo Compiling poweroffd.exe...
        cl /std:c++17 /O2 /EHsc /W4 /Fe:poweroffd.exe poweroffd-win.cpp /link ws2_32.lib bcrypt.lib advapi32.lib user32.lib wtsapi32.lib userenv.lib
        if errorlevel 1 goto :fail

        echo Compiling poweroff-send.exe...
        cl /std:c++17 /O2 /EHsc /W4 /Fe:poweroff-send.exe poweroff-send-win.cpp /link ws2_32.lib bcrypt.lib
        if errorlevel 1 goto :fail

        echo.
        echo Build complete!
        goto :done
    )
)

cmake --build . --config Release
if errorlevel 1 goto :fail

echo.
echo Build complete! Binaries in build\Release\
goto :done

:fail
echo.
echo Build FAILED.
exit /b 1

:done
echo.
echo To install as a service (run as Administrator):
echo   powershell -ExecutionPolicy Bypass -File install.ps1
