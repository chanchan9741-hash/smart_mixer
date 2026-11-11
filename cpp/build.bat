@echo off
echo ==============================================
echo  Building Smart Audio Mixer and HW Simulator
echo ==============================================

echo [1/4] Compiling Main GUI Application (smart_mixer_gui.exe)...
g++ -std=c++17 -O3 -mwindows -Iinclude src/gui_main.cpp -lgdi32 -lcomctl32 -lcomdlg32 -lmsimg32 -lole32 -lws2_32 -lmfplat -lmfreadwrite -lmfuuid -o smart_mixer_gui.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to compile smart_mixer_gui.exe
    exit /b %ERRORLEVEL%
)

echo [2/4] Compiling Hardware Simulator (hardware_sim_gui.exe)...
g++ -std=c++17 -O3 -mwindows -Iinclude src/hardware_sim_main.cpp -lgdi32 -lcomctl32 -lmsimg32 -lole32 -lws2_32 -o hardware_sim_gui.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to compile hardware_sim_gui.exe
    exit /b %ERRORLEVEL%
)

echo [3/4] Compiling CLI Application (smart_mixer.exe)...
g++ -std=c++17 -O3 -Iinclude src/main.cpp -o smart_mixer.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to compile smart_mixer.exe
    exit /b %ERRORLEVEL%
)

echo [4/4] Compiling Unit Tests (test_suite.exe)...
g++ -std=c++17 -O3 -Iinclude tests/test_main.cpp -lole32 -lmfplat -lmfreadwrite -lmfuuid -o test_suite.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to compile test_suite.exe
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] All targets built successfully!
echo - Run Main GUI Mixer:      smart_mixer_gui.exe
echo - Run Hardware Simulator:   hardware_sim_gui.exe
echo - Run Console Pipeline:     smart_mixer.exe
echo - Run Unit Tests:           test_suite.exe

