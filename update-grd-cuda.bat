@echo off
setlocal
cd /d "%~dp0"

set "PAUSE_ON_EXIT=0"
if /i "%~1"=="--pause" set "PAUSE_ON_EXIT=1"

set "MINGW=C:\msys64\ucrt64"
set "MSYS=C:\msys64\usr"
set "VSDIR=C:\BuildTools\VS2022"
set "CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3"
set "LLVM=C:\Users\gochi\llvm22\clang+llvm-22.1.8-x86_64-pc-windows-msvc"
set "DEP_INC=C:\Users\gochi\grd-headers"
set "BUILD_DIR=build\release-cuda"
set "DIST_DIR=dist\windows-nvenc"
set "REQUESTED_CUDA_ARCH=%CUDA_ARCH%"
set "CUDA_ARCH="
set "CUDA_BUILD=0"

rem --- prerequisite checks ---
where git >nul 2>nul
if errorlevel 1 goto :missing_git
if not exist "%MINGW%\bin\cmake.exe" goto :missing_toolchain
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto :missing_msvc
if not exist "%LLVM%\bin\clang-cl.exe" goto :missing_llvm

for /f "delims=" %%V in ('dir /b "%VSDIR%\VC\Tools\MSVC" 2^>nul') do set "MSVCVER=%%V"
if not defined MSVCVER goto :missing_msvc
set "MSVCBIN=%VSDIR%\VC\Tools\MSVC\%MSVCVER%\bin\Hostx64\x64"

echo.
echo [1/7] git pull
git pull --ff-only origin main
if not "%ERRORLEVEL%"=="0" goto :pull_failed

echo.
echo [2/7] detect local NVIDIA GPU
call :select_cuda_target
if "%CUDA_BUILD%"=="1" if not exist "%CUDA%\bin\nvcc.exe" goto :missing_cuda

rem --- MSVC toolchain environment ---
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not "%ERRORLEVEL%"=="0" goto :failed
set "PATH=%MINGW%\bin;%MSYS%\bin;%CUDA%\bin\x64;%CUDA%\bin;%LLVM%\bin;%PATH%"
set "CUDA_PATH=%CUDA%"

rem --- dependency header shim (only package headers, no MinGW std headers) ---
if not exist "%DEP_INC%\SDL3" (
    echo Preparing dependency headers...
    if not exist "%DEP_INC%" mkdir "%DEP_INC%"
    xcopy /e /i /y "%MINGW%\include\SDL3" "%DEP_INC%\SDL3" >nul
    xcopy /e /i /y "%MINGW%\include\libavcodec" "%DEP_INC%\libavcodec" >nul
    xcopy /e /i /y "%MINGW%\include\libavutil" "%DEP_INC%\libavutil" >nul
    xcopy /e /i /y "%MINGW%\include\libswresample" "%DEP_INC%\libswresample" >nul
    xcopy /e /i /y "%MINGW%\include\libswscale" "%DEP_INC%\libswscale" >nul
    xcopy /e /i /y "%MINGW%\include\sodium" "%DEP_INC%\sodium" >nul
    copy /y "%MINGW%\include\sodium.h" "%DEP_INC%\" >nul
)
set "GRD_DEP_INCLUDE=%DEP_INC%"

echo.
echo [3/7] configure CMake
call :configure_build
if not "%ERRORLEVEL%"=="0" goto :failed

echo.
echo [4/7] stop GRD and build (release)
call :stop_running_grd
if not "%ERRORLEVEL%"=="0" goto :stop_failed
cmake --build "%BUILD_DIR%" --parallel
if not "%ERRORLEVEL%"=="0" goto :failed

echo.
echo [5/7] verify build
echo Running tests...
ctest --test-dir "%BUILD_DIR%" --output-on-failure
if not "%ERRORLEVEL%"=="0" goto :tests_failed
if "%CUDA_BUILD%"=="1" goto :verify_cuda_kernel
goto :publish

:verify_cuda_kernel
echo.
echo Running a real CUDA kernel compatibility test...
"%BUILD_DIR%\grd_diag.exe" --cuda-selftest
if not errorlevel 1 goto :publish
echo.
echo [WARNING] CUDA kernel execution is incompatible with this host.
echo [WARNING] Rebuilding with CPU conversion so clients receive video.
set "CUDA_BUILD=0"
call :configure_build
if not "%ERRORLEVEL%"=="0" goto :fallback_failed
cmake --build "%BUILD_DIR%" --parallel
if not "%ERRORLEVEL%"=="0" goto :fallback_failed
ctest --test-dir "%BUILD_DIR%" --output-on-failure
if not "%ERRORLEVEL%"=="0" goto :tests_failed

:publish
echo.
echo [6/7] update dist
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
copy /y "%BUILD_DIR%\grd.exe" "%DIST_DIR%\grd.exe" >nul
if not "%ERRORLEVEL%"=="0" goto :publish_failed
copy /y README.md SECURITY.md REMOTE_ACCESS.md "%DIST_DIR%\" >nul
if not exist "%DIST_DIR%\scripts" mkdir "%DIST_DIR%\scripts"
copy /y scripts\grd-remote-access.ps1 "%DIST_DIR%\scripts\" >nul
copy /y scripts\grd-remote-access.example.psd1 "%DIST_DIR%\scripts\" >nul
"%MINGW%\bin\objdump.exe" -p "%BUILD_DIR%\grd.exe" | findstr /i /c:"DLL Name" > "%TEMP%\grd_deps.txt"
if not "%ERRORLEVEL%"=="0" goto :publish_failed
for /f "usebackq tokens=3" %%D in ("%TEMP%\grd_deps.txt") do (
    if exist "%MINGW%\bin\%%D" copy /y "%MINGW%\bin\%%D" "%DIST_DIR%\" >nul
)
del "%TEMP%\grd_deps.txt" >nul 2>nul
if exist "%CUDA%\bin\x64\cudart64_13.dll" copy /y "%CUDA%\bin\x64\cudart64_13.dll" "%DIST_DIR%\" >nul

echo.
echo [7/7] launch GRD...
start "" "%DIST_DIR%\grd.exe" >nul 2>nul
if not "%ERRORLEVEL%"=="0" goto :launch_failed
%SystemRoot%\System32\ping.exe -n 2 127.0.0.1 >nul
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq grd.exe" 2>nul | %SystemRoot%\System32\find.exe /i "grd.exe" >nul
if errorlevel 1 goto :launch_failed
if "%CUDA_BUILD%"=="1" goto :cuda_launch_success
echo Done: %DIST_DIR%\grd.exe (CUDA compatibility fallback, CPU conversion)
goto :launch_success

:cuda_launch_success
echo Done: %DIST_DIR%\grd.exe (CUDA sm_%CUDA_ARCH% + NVENC, GPU conversion)

:launch_success
echo.
call :maybe_pause
exit /b 0

:select_cuda_target
where nvidia-smi.exe >nul 2>nul
if errorlevel 1 goto :cuda_detection_unavailable
set "CUDA_DETECT_FILE=%TEMP%\grd-cuda-detect-%RANDOM%-%RANDOM%.txt"
nvidia-smi.exe --id=0 --query-gpu=name,compute_cap --format=csv,noheader,nounits > "%CUDA_DETECT_FILE%" 2>nul
if errorlevel 1 goto :cuda_detection_cleanup
for /f "usebackq tokens=1,* delims=," %%G in ("%CUDA_DETECT_FILE%") do if not defined CUDA_GPU_NAME (
    set "CUDA_GPU_NAME=%%G"
    set "CUDA_COMPUTE_CAP=%%H"
)
del "%CUDA_DETECT_FILE%" >nul 2>nul
set "CUDA_DETECT_FILE="
if not defined CUDA_COMPUTE_CAP goto :cuda_detection_unavailable
for /f "tokens=1,2 delims=. " %%A in ("%CUDA_COMPUTE_CAP%") do set "DETECTED_CUDA_ARCH=%%A%%B"
if not defined DETECTED_CUDA_ARCH goto :cuda_detection_unavailable
for /f "delims=0123456789" %%A in ("%DETECTED_CUDA_ARCH%") do goto :cuda_detection_unavailable
set "CUDA_ARCH=%DETECTED_CUDA_ARCH%"
set "CUDA_BUILD=1"
echo Detected %CUDA_GPU_NAME% ^(compute capability %CUDA_COMPUTE_CAP%, sm_%CUDA_ARCH%^).
if not defined REQUESTED_CUDA_ARCH exit /b 0
if "%REQUESTED_CUDA_ARCH%"=="%CUDA_ARCH%" (
    echo CUDA_ARCH=%REQUESTED_CUDA_ARCH% matches the local GPU.
    exit /b 0
)
echo [WARNING] CUDA_ARCH=%REQUESTED_CUDA_ARCH% does not match this GPU.
echo [WARNING] Using the detected architecture sm_%CUDA_ARCH% instead.
exit /b 0

:cuda_detection_cleanup
del "%CUDA_DETECT_FILE%" >nul 2>nul
set "CUDA_DETECT_FILE="

:cuda_detection_unavailable
set "CUDA_BUILD=0"
set "CUDA_ARCH="
echo [WARNING] The local NVIDIA compute capability could not be detected.
echo [WARNING] Building the safe CPU-conversion fallback.
exit /b 0

:configure_build
if "%CUDA_BUILD%"=="1" goto :configure_cuda_build
echo Configuring NVENC/software support with CUDA conversion disabled...
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=%LLVM%/bin/clang-cl.exe ^
  -DGRD_BUILD_TESTS=ON -DGRD_REQUIRE_CUDA=OFF -DGRD_ENABLE_CUDA=OFF ^
  -DCMAKE_RC_COMPILER="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
  -DCMAKE_LINKER=%MSVCBIN%/link.exe ^
  -DCMAKE_AR=%MSVCBIN%/lib.exe ^
  "-DCMAKE_C_STANDARD_INCLUDE_DIRECTORIES=%VSDIR%/VC/Tools/MSVC/%MSVCVER%/include;C:/PROGRA~2/WI3CF2~1/10/Include/100261~1.0/ucrt;C:/PROGRA~2/WI3CF2~1/10/Include/100261~1.0/um;C:/PROGRA~2/WI3CF2~1/10/Include/100261~1.0/shared"
exit /b %ERRORLEVEL%

:configure_cuda_build
echo Configuring CUDA sm_%CUDA_ARCH% + NVENC...
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=%LLVM%/bin/clang-cl.exe ^
  -DGRD_BUILD_TESTS=ON -DGRD_REQUIRE_CUDA=ON -DGRD_ENABLE_CUDA=ON ^
  -DCMAKE_CUDA_HOST_COMPILER=%MSVCBIN%/cl.exe ^
  -DCMAKE_RC_COMPILER="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
  -DCMAKE_LINKER=%MSVCBIN%/link.exe ^
  -DCMAKE_AR=%MSVCBIN%/lib.exe ^
  "-DCMAKE_CUDA_COMPILER=%CUDA%/bin/nvcc.exe" ^
  -DCMAKE_CUDA_ARCHITECTURES=%CUDA_ARCH% ^
  "-DCUDAToolkit_ROOT=%CUDA:\=/%" ^
  "-DCMAKE_C_STANDARD_INCLUDE_DIRECTORIES=%VSDIR%/VC/Tools/MSVC/%MSVCVER%/include;C:/PROGRA~2/WI3CF2~1/10/Include/100261~1.0/ucrt;C:/PROGRA~2/WI3CF2~1/10/Include/100261~1.0/um;C:/PROGRA~2/WI3CF2~1/10/Include/100261~1.0/shared" ^
  "-DCMAKE_CUDA_FLAGS=-D_WINDOWS -Xcompiler=/IC:\BuildTools\VS2022\VC\Tools\MSVC\%MSVCVER%\include -Xcompiler=/IC:\PROGRA~2\WI3CF2~1\10\Include\100261~1.0\ucrt -Xcompiler=/IC:\PROGRA~2\WI3CF2~1\10\Include\100261~1.0\um -Xcompiler=/IC:\PROGRA~2\WI3CF2~1\10\Include\100261~1.0\shared"
exit /b %ERRORLEVEL%

:stop_running_grd
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq grd.exe" 2>nul | %SystemRoot%\System32\find.exe /i "grd.exe" >nul
if errorlevel 1 exit /b 0
echo GRD is running: stopping it before relinking the executable...
%SystemRoot%\System32\taskkill.exe /IM grd.exe /F >nul 2>nul
if errorlevel 1 exit /b 1
%SystemRoot%\System32\ping.exe -n 2 127.0.0.1 >nul
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq grd.exe" 2>nul | %SystemRoot%\System32\find.exe /i "grd.exe" >nul
if not errorlevel 1 exit /b 1
exit /b 0

:missing_git
echo [ERROR] Git was not found. Install Git for Windows and try again.
call :maybe_pause
exit /b 1

:missing_toolchain
echo [ERROR] The MSYS2 UCRT64 toolchain was not found in %MINGW%.
call :maybe_pause
exit /b 1

:missing_msvc
echo [ERROR] Visual Studio Build Tools was not found in %VSDIR%.
call :maybe_pause
exit /b 1

:missing_llvm
echo [ERROR] LLVM clang-cl was not found in %LLVM%.
call :maybe_pause
exit /b 1

:missing_cuda
echo [ERROR] The CUDA Toolkit was not found in %CUDA%.
call :maybe_pause
exit /b 1

:pull_failed
echo.
echo [ERROR] git pull failed. Check the network and any uncommitted local changes.
call :maybe_pause
exit /b 1

:failed
echo.
echo [ERROR] The build failed. See the messages above.
call :maybe_pause
exit /b 1

:tests_failed
echo.
echo [ERROR] Unit tests failed. The new executable will not be published to dist.
call :maybe_pause
exit /b 1

:fallback_failed
echo.
echo [ERROR] CUDA failed and the CPU-conversion fallback could not be built.
echo [ERROR] The executable in dist was not replaced.
call :maybe_pause
exit /b 1

:stop_failed
echo.
echo [ERROR] GRD could not be stopped. The build executable is still locked.
call :maybe_pause
exit /b 1

:publish_failed
echo.
echo [ERROR] The updated executable could not be published to %DIST_DIR%.
call :maybe_pause
exit /b 1

:launch_failed
echo.
echo [ERROR] The updated GRD executable could not be launched.
call :maybe_pause
exit /b 1

:maybe_pause
if "%PAUSE_ON_EXIT%"=="1" (
    echo Press any key to continue . . .
    pause >nul
)
exit /b 0
