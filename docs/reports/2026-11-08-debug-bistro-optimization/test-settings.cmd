@echo off
setlocal
pushd "%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto done
set "SETTINGS_TEST_OUT=Build\Debug20Optimize-20261108\settings-repro"
if not exist %SETTINGS_TEST_OUT% mkdir %SETTINGS_TEST_OUT%
set "SETTINGS_TEST_FLAGS=/nologo /utf-8 /std:c++17 /EHsc /W4 /Z7 /D_DEBUG /MDd /Od /RTC1 /IEngine/Engine.Core/Include /IExternal/External.Libraries/inih /IExternal/External.Libraries/spdlog/include /DFMT_HEADER_ONLY /DINI_STOP_ON_FIRST_ERROR=1 /DINI_ALLOW_MULTILINE=0 /DINI_CALL_HANDLER_ON_NEW_SECTION=1 /DINI_MAX_LINE=4096"
set "SETTINGS_TEST_CORE=Engine/Engine.Core/src/ConsoleModule.cpp Engine/Engine.Core/src/Logger.cpp Engine/Engine.Core/src/RuntimePaths.cpp Engine/Engine.Core/src/BenchmarkEngine.cpp Engine/Engine.Core/src/GameTimer.cpp External/External.Libraries/inih/ini.c"
cl %SETTINGS_TEST_FLAGS% Engine/Engine.Core/tests/ConsoleVariableTests.cpp %SETTINGS_TEST_CORE% /Fo%SETTINGS_TEST_OUT%\ /Fe%SETTINGS_TEST_OUT%\ConsoleVariableTests.exe /link kernel32.lib
if errorlevel 1 goto done
%SETTINGS_TEST_OUT%\ConsoleVariableTests.exe
if errorlevel 1 goto done
cl %SETTINGS_TEST_FLAGS% /IExternal/External.Libraries/imgui/include /IExternal/External.Libraries/imgui/include/imgui Engine/Engine.UI/tests/SettingsCacheTests.cpp External/External.Libraries/imgui/src/imgui.cpp External/External.Libraries/imgui/src/imgui_draw.cpp External/External.Libraries/imgui/src/imgui_tables.cpp External/External.Libraries/imgui/src/imgui_widgets.cpp %SETTINGS_TEST_CORE% /Fo%SETTINGS_TEST_OUT%\ /Fe%SETTINGS_TEST_OUT%\SettingsCacheTests.exe /link kernel32.lib
if errorlevel 1 goto done
%SETTINGS_TEST_OUT%\SettingsCacheTests.exe
:done
set "SETTINGS_TEST_RESULT=%errorlevel%"
popd
exit /b %SETTINGS_TEST_RESULT%
