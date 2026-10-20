@echo off
rem ===========================================================================
rem CapWGC build script
rem
rem Keep this file ASCII-only: cmd.exe parses .bat in the OEM codepage and
rem non-ASCII comments get executed as commands.
rem
rem Keep this file CRLF too. With LF-only line endings cmd's `goto :label`
rem seeks to the wrong byte offset: it lands *near* the label, so the batch
rem silently runs the previous target's block. Symptom seen here: `build.bat
rem ocrtest` also built CapCli.exe and CapWGC.exe, and nobody noticed because
rem everything gets rebuilt anyway. Check with:
rem   python -c "b=open('build/build.bat','rb').read(); print(b.count(b'\r\n'), b.count(b'\n'))"
rem
rem Usage:
rem   build.bat            -> build everything that exists
rem   build.bat probe      -> only probe_encoders.exe
rem   build.bat cli        -> only CapCli.exe
rem
rem Why this does not just call vcvars64.bat:
rem   vcvars64.bat shells out to reg.exe (and to VsDevCmd, which calls it too).
rem   In a sandboxed shell reg.exe is blocked, vcvars fails *partially* --
rem   exit code 0, but INCLUDE/LIB never set -- and the build then dies with
rem   "cannot open windows.h", which reads like a missing SDK rather than a
rem   broken environment. So: try vcvars, verify cl actually works, and fall
rem   back to setting INCLUDE/LIB/PATH by hand from the installed versions.
rem ===========================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "OUT=%ROOT%\out"
if not exist "%OUT%" mkdir "%OUT%"

set "VCVARS="
for %%P in (
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do (
  if not defined VCVARS if exist %%P set "VCVARS=%%~P"
)

rem vcvars can fail *silently*: it puts cl on PATH early, then dies on reg.exe,
rem so `where cl` succeeds while INCLUDE is still empty. INCLUDE is the honest
rem test -- a missing INCLUDE is exactly the "cannot open windows.h" symptom.
set "CL_OK="
if defined VCVARS (
  call "%VCVARS%" >nul 2>nul
  set "CHK=!INCLUDE!"
  if not "!CHK:ucrt=!"=="!CHK!" set "CL_OK=1"
)

if not defined CL_OK (
  echo [build] vcvars unusable ^(reg.exe blocked or VS missing^) - setting env manually
  set "MSVC="
  for /d %%D in ("C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\*") do (
    if not defined MSVC set "MSVC=%%~fD"
  )
  if not defined MSVC for /d %%D in ("C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*") do (
    if not defined MSVC set "MSVC=%%~fD"
  )
  set "SDK="
  for /d %%D in ("C:\Program Files (x86)\Windows Kits\10\Include\10.*") do set "SDK=%%~nxD"
  if not defined MSVC (
    echo [build] FAILED: no MSVC toolset found
    exit /b 1
  )
  if not defined SDK (
    echo [build] FAILED: no Windows SDK found
    exit /b 1
  )
  set "KIT=C:\Program Files (x86)\Windows Kits\10"
  rem ! not %: these are assigned inside this parenthesised block, and %VAR%
  rem is expanded when the block is parsed -- i.e. before MSVC/SDK exist.
  set "PATH=!MSVC!\bin\Hostx64\x64;!PATH!"
  set "INCLUDE=!MSVC!\include;!KIT!\Include\!SDK!\ucrt;!KIT!\Include\!SDK!\shared;!KIT!\Include\!SDK!\um;!KIT!\Include\!SDK!\winrt;!KIT!\Include\!SDK!\cppwinrt"
  set "LIB=!MSVC!\lib\x64;!KIT!\Lib\!SDK!\ucrt\x64;!KIT!\Lib\!SDK!\um\x64"
  where cl >nul 2>nul || (echo [build] FAILED: cl still not found & exit /b 1)
)
echo [build] toolset ready

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

set "CFLAGS=/nologo /utf-8 /std:c++20 /EHsc /O2 /GL /DNDEBUG /W4 /WX- /permissive- /Zc:__cplusplus /MP"
set "LTCG=/LTCG"

if /i "%TARGET%"=="probe" goto :probe
if /i "%TARGET%"=="cli" goto :cli
if /i "%TARGET%"=="gui" goto :gui
if /i "%TARGET%"=="test" goto :test
if /i "%TARGET%"=="ocrtest" goto :ocrtest
if /i "%TARGET%"=="dimtest" goto :dimtest
if /i "%TARGET%"=="balloontest" goto :balloontest
if /i "%TARGET%"=="toastclicktest" goto :toastclicktest
if /i "%TARGET%"=="prtsctest" goto :prtsctest
if /i "%TARGET%"=="all" goto :probe

echo [build] unknown target "%TARGET%"
exit /b 1

:ocrtest
rem Native OCR comparison harness (ONNX Runtime, no Python).
rem Links onnxruntime.lib and needs onnxruntime.dll next to the exe at runtime.
echo [build] ocr_onnx_test.exe
if exist "%OUT%\ocr_onnx_test.exe" del /f /q "%OUT%\ocr_onnx_test.exe" >nul 2>nul
cl %CFLAGS% /I"%ROOT%\third_party\onnxruntime\include" ^
   "%ROOT%\src\probe\ocr_onnx_test.cpp" "%ROOT%\src\core\ocr_onnx.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\ocr_onnx_test.exe" ^
   /link /SUBSYSTEM:CONSOLE "%ROOT%\third_party\onnxruntime\lib\onnxruntime.lib" ^
   user32.lib
if errorlevel 1 (echo [build] FAILED: ocr_onnx_test & exit /b 1)
copy /y "%ROOT%\third_party\onnxruntime\lib\onnxruntime.dll" "%OUT%" >nul
copy /y "%ROOT%\third_party\onnxruntime\lib\onnxruntime_providers_shared.dll" "%OUT%" >nul
goto :done

:balloontest
rem Shows why the tray balloon was replaced by our own toast window: three
rem NIF_INFO balloons all report accepted=1 but only one is readable on screen,
rem because Win10+ hands them to the Action Center notification queue.
echo [build] balloon_queue_test.exe
if exist "%OUT%\balloon_queue_test.exe" del /f /q "%OUT%\balloon_queue_test.exe" >nul 2>nul
cl %CFLAGS% "%ROOT%\src\probe\balloon_queue_test.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\balloon_queue_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib shell32.lib gdi32.lib
if errorlevel 1 (echo [build] FAILED: balloon_queue_test & exit /b 1)
goto :done

:dimtest
rem DimPanel reuse harness: proves a panel still dims after Destroy + re-Create
rem at the SAME rect (the "no visible change" early-out used to swallow it).
rem Needs src\app\ui.cpp because DimPanel lives there -- and ui.cpp pulls in
rem d2d1/dwrite for the other widgets, so those libs come along too.
echo [build] dim_panel_test.exe
if exist "%OUT%\dim_panel_test.exe" del /f /q "%OUT%\dim_panel_test.exe" >nul 2>nul
cl %CFLAGS% "%ROOT%\src\probe\dim_panel_test.cpp" "%ROOT%\src\app\ui.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\dim_panel_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib d2d1.lib dwrite.lib ^
   ole32.lib shell32.lib shlwapi.lib windowscodecs.lib
if errorlevel 1 (echo [build] FAILED: dim_panel_test & exit /b 1)
goto :done

:toastclicktest
rem Toast click harness: proves the "已儲存（點一下開啟）" toast actually runs
rem its action. Replacing the Shell balloon silently killed NIN_BALLOONUSERCLICK,
rem so the title promised a click that only hid the toast. This drives real
rem WM_LBUTTONDOWN / WM_RBUTTONDOWN into the toast window and counts callbacks.
echo [build] toast_click_test.exe
if exist "%OUT%\toast_click_test.exe" del /f /q "%OUT%\toast_click_test.exe" >nul 2>nul
cl %CFLAGS% "%ROOT%\src\probe\toast_click_test.cpp" "%ROOT%\src\app\ui.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\toast_click_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib d2d1.lib dwrite.lib ^
   ole32.lib shell32.lib shlwapi.lib windowscodecs.lib
if errorlevel 1 (echo [build] FAILED: toast_click_test & exit /b 1)
goto :done

:prtsctest
rem PrtScn probe: can RegisterHotKey(VK_SNAPSHOT) actually win the key, or does
rem Windows 11's Snipping Tool get there first? The registry flag says one thing
rem and reality says another often enough that this has to be measured.
rem Prints KEY=VALUE lines and waits 8 s for a real keypress.
echo [build] prtsc_probe.exe
if exist "%OUT%\prtsc_probe.exe" del /f /q "%OUT%\prtsc_probe.exe" >nul 2>nul
cl %CFLAGS% "%ROOT%\src\probe\prtsc_probe.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\prtsc_probe.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib
if errorlevel 1 (echo [build] FAILED: prtsc_probe & exit /b 1)
goto :done

:test
rem Retired with the Python OCR backend.
rem
rem This target used to build json_parse_test.exe, which pinned the JSON string
rem parser inside src\app\ocr.cpp (the one that decoded \uXXXX). That parser
rem existed only to read ocr_cli.py's JSON Lines protocol; OCR is now in-process
rem ONNX (src\core\ocr_onnx.cpp) and the protocol - along with the parser - is
rem gone. The equivalent check today is the ocrtest target below: it runs the
rem real engine on a real image and prints every line, which is what the unit
rem test was standing in for.
echo [build] test target retired - use "build.bat ocrtest"
exit /b 0

:probe
echo [build] probe_encoders.exe
cl %CFLAGS% "%ROOT%\src\probe\probe_encoders.cpp" /Fo"%OUT%\\" /Fe"%OUT%\probe_encoders.exe" ^
   /link %LTCG% /SUBSYSTEM:CONSOLE mfplat.lib mfuuid.lib ole32.lib oleaut32.lib d3d11.lib
if errorlevel 1 (echo [build] FAILED: probe_encoders & exit /b 1)

echo [build] anim_window.exe
cl %CFLAGS% "%ROOT%\src\probe\anim_window.cpp" /Fo"%OUT%\\" /Fe"%OUT%\anim_window.exe" ^
   /link %LTCG% /SUBSYSTEM:CONSOLE user32.lib gdi32.lib winmm.lib
if errorlevel 1 (echo [build] FAILED: anim_window & exit /b 1)

if exist "%OUT%\mp4_check.exe" del /f /q "%OUT%\mp4_check.exe" >nul 2>nul
echo [build] mp4_check.exe
cl %CFLAGS% "%ROOT%\src\probe\mp4_check.cpp" /Fo"%OUT%\\" /Fe"%OUT%\mp4_check.exe" ^
   /link %LTCG% /SUBSYSTEM:CONSOLE mfplat.lib mfuuid.lib mfreadwrite.lib ole32.lib oleaut32.lib
if errorlevel 1 (echo [build] FAILED: mp4_check & exit /b 1)
if /i "%TARGET%"=="probe" goto :done

:cli
echo [build] CapCli.exe
rem Delete the old exe first. link cannot overwrite an exe that was just run
rem (Access is denied, with no filename in the message, which reads like a
rem compile failure). Deleting is idempotent and harmless if it fails.
if exist "%OUT%\CapCli.exe" del /f /q "%OUT%\CapCli.exe" >nul 2>nul
cl %CFLAGS% "%ROOT%\src\cli\main.cpp" "%ROOT%\src\core\capture_wgc.cpp" ^
   "%ROOT%\src\core\encoder_mft.cpp" "%ROOT%\src\core\codec_select.cpp" ^
   "%ROOT%\src\core\mft_encoder.cpp" "%ROOT%\src\core\mp4_muxer.cpp" ^
   "%ROOT%\src\core\audio_capture.cpp" "%ROOT%\src\core\aac_encoder.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\CapCli.exe" ^
   /link %LTCG% /SUBSYSTEM:CONSOLE mfplat.lib mfuuid.lib mfreadwrite.lib ole32.lib oleaut32.lib ^
   d3d11.lib dxgi.lib user32.lib shcore.lib windowsapp.lib dwmapi.lib
if errorlevel 1 (echo [build] FAILED: CapCli & exit /b 1)

if /i "%TARGET%"=="cli" goto :done

:gui
echo [build] CapWGC.exe
rem GUI. Same core objects as CapCli plus src\app\*. The app layer must not
rem duplicate the recording loop -- that is why recorder.cpp exists.
if exist "%OUT%\CapWGC.exe" del /f /q "%OUT%\CapWGC.exe" >nul 2>nul
cl %CFLAGS% /I"%ROOT%\third_party\onnxruntime\include" ^
   "%ROOT%\src\app\main.cpp" "%ROOT%\src\app\ui.cpp" ^
   "%ROOT%\src\app\overlay.cpp" "%ROOT%\src\app\picker.cpp" ^
   "%ROOT%\src\app\controlbar.cpp" ^
   "%ROOT%\src\app\settings.cpp" ^
   "%ROOT%\src\app\snip.cpp" "%ROOT%\src\app\ocr.cpp" ^
   "%ROOT%\src\core\ocr_onnx.cpp" ^
   "%ROOT%\src\core\recorder.cpp" "%ROOT%\src\core\capture_wgc.cpp" ^
   "%ROOT%\src\core\encoder_mft.cpp" "%ROOT%\src\core\codec_select.cpp" ^
   "%ROOT%\src\core\mft_encoder.cpp" "%ROOT%\src\core\mp4_muxer.cpp" ^
   "%ROOT%\src\core\audio_capture.cpp" "%ROOT%\src\core\aac_encoder.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\CapWGC.exe" ^
   /link %LTCG% /SUBSYSTEM:WINDOWS mfplat.lib mfuuid.lib mfreadwrite.lib ole32.lib ^
   oleaut32.lib d3d11.lib dxgi.lib user32.lib shcore.lib windowsapp.lib dwmapi.lib ^
   gdi32.lib shell32.lib comctl32.lib d2d1.lib dwrite.lib shlwapi.lib ^
   windowscodecs.lib "%ROOT%\third_party\onnxruntime\lib\onnxruntime.lib" ^
   /DELAYLOAD:onnxruntime.dll delayimp.lib
if errorlevel 1 (echo [build] FAILED: CapWGC & exit /b 1)
rem onnxruntime.dll must sit next to the exe at runtime (it is the inference engine).
rem /DELAYLOAD keeps that a *runtime* requirement of the OCR path instead of a
rem *load-time* requirement of the whole process: without the DLL the app still
rem starts, OCR just stays grey (ocr.cpp probes LoadLibraryW first). Without it
rem the loader kills the process before main(), and on Windows 11 it would even
rem find System32\onnxruntime.dll (an older, API-incompatible build) instead.
copy /y "%ROOT%\third_party\onnxruntime\lib\onnxruntime.dll" "%OUT%" >nul
if errorlevel 1 (echo [build] FAILED: copy onnxruntime.dll & exit /b 1)

:done
rem Careful with "->" in echo: cmd treats ">" as a redirect, so "echo OK -> %OUT%"
rem tries to write to the out\ *directory* and fails with "access denied" -- the
rem build looks broken even though it succeeded. Escape it.
echo [build] OK -^> %OUT%
exit /b 0
