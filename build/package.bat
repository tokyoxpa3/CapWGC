@echo off
rem ===========================================================================
rem CapWGC packaging script - builds a standalone, single-exe distribution.
rem
rem Keep this file ASCII-only: cmd.exe parses .bat in the OEM codepage and
rem non-ASCII comments get executed as commands.
rem
rem Usage:
rem   package.bat           -> dist\CapWGC-<ver>\ + dist\CapWGC-<ver>.zip
rem   package.bat 0.1.0     -> same, with an explicit version
rem
rem The version is an argument because CI derives it from the git tag; a
rem hardcoded number there would ship v0.2.0 tagged releases as "0.1.0".
rem ===========================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "VER=%~1"
if "%VER%"=="" set "VER=0.2.0"
set "NAME=CapWGC-%VER%"
set "DIST=%ROOT%\dist"
set "STAGE=%DIST%\%NAME%"

rem Why the core app is already standalone:
rem   The build uses cl's default static CRT (no /MD), so the exe does not
rem   import VCRUNTIME140.dll / MSVCP140.dll. Every remaining import is a
rem   Windows system DLL (MFPlat, d3d11, d2d1, DWrite, ole32, shcore...).
rem   Verified with:
rem     dumpbin -dependents out\CapWGC.exe | findstr /i vcruntime msvcp
rem   => no output. So there is no redistributable to install and nothing to
rem   copy next to the exe.
rem
rem OCR is now fully in-process: onnxruntime.dll + models\ ship next to the exe.
rem No Python, no venv, no model download on first run. See prepare-models.bat
rem for how the .onnx files get into models\ in the first place.
rem ===========================================================================

echo [pkg] building gui
call "%ROOT%\build\build.bat" gui
if errorlevel 1 (echo [pkg] FAILED: build & exit /b 1)

if not exist "%ROOT%\out\CapWGC.exe" (
  echo [pkg] FAILED: out\CapWGC.exe missing
  exit /b 1
)

echo [pkg] staging %STAGE%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" 2>nul

copy /y "%ROOT%\out\CapWGC.exe" "%STAGE%" >nul
if errorlevel 1 (echo [pkg] FAILED: copy exe & exit /b 1)

rem OCR: the inference engine (onnxruntime.dll) and the two models. All three
rem are required for OCR; without them the OCR segment of the toolbar is greyed
rem out and everything else still works.
copy /y "%ROOT%\third_party\onnxruntime\lib\onnxruntime.dll" "%STAGE%" >nul
if errorlevel 1 (echo [pkg] FAILED: copy onnxruntime.dll & exit /b 1)

if not exist "%STAGE%\models" mkdir "%STAGE%\models" 2>nul
copy /y "%ROOT%\models\ppocr_det.onnx" "%STAGE%\models\" >nul
copy /y "%ROOT%\models\ppocr_rec.onnx" "%STAGE%\models\" >nul
copy /y "%ROOT%\models\ppocr_dict.txt" "%STAGE%\models\" >nul
if not exist "%STAGE%\models\ppocr_dict.txt" (
  echo [pkg] FAILED: models missing - run prepare-models.bat first
  exit /b 1
)

if exist "%ROOT%\LICENSE" copy /y "%ROOT%\LICENSE" "%STAGE%" >nul

rem A short readme for whoever unzips this. Written from PowerShell so the
rem text is UTF-8 with BOM; `echo` in a .bat would mangle anything non-ASCII.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$t = @();" ^
  "$t += 'CapWGC %VER% - standalone build';" ^
  "$t += '';" ^
  "$t += 'Run CapWGC.exe. It lives in the tray; there is no main window.';" ^
  "$t += 'Default hotkeys: Ctrl+Alt+A record, Ctrl+Alt+X snip, Ctrl+Alt+W picker,';" ^
  "$t += 'Ctrl+Alt+S stop. All of them are editable in the settings window.';" ^
  "$t += '';" ^
  "$t += 'Settings and logs: %%LOCALAPPDATA%%\CapWGC\';" ^
  "$t += 'Recordings: the folder set in the settings window.';" ^
  "$t += '';" ^
  "$t += 'Files that must stay together:';" ^
  "$t += '  CapWGC.exe          the app';" ^
  "$t += '  onnxruntime.dll     OCR inference engine';" ^
  "$t += '  models\             OCR models (det / rec / dict)';" ^
  "$t += 'Deleting models\ disables OCR only; recording and screenshots still work.';" ^
  "$t += '';" ^
  "$t += 'No installer, no Python, no VC++ redistributable.';" ^
  "[IO.File]::WriteAllLines('%STAGE%\README.txt', $t, (New-Object Text.UTF8Encoding $true))"
if errorlevel 1 (echo [pkg] FAILED: readme & exit /b 1)

echo [pkg] zipping
set "ZIP=%DIST%\%NAME%.zip"
if exist "%ZIP%" del /f /q "%ZIP%" >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 (echo [pkg] FAILED: zip & exit /b 1)

echo [pkg] OK
echo [pkg]   folder: %STAGE%
echo [pkg]   zip   : %ZIP%
exit /b 0
