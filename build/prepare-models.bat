@echo off
rem ===========================================================================
rem prepare-models.bat - copy the OCR ONNX models into models\
rem
rem Keep this file ASCII-only (cmd.exe parses .bat in the OEM codepage).
rem
rem Why a script instead of committing the weights:
rem   det is 9.4MB and rec is 20MB. They are Apache-2.0 (PP-OCRv6) so they
rem   *could* be committed, but a 30MB binary blob in git is a permanent cost
rem   for everyone who only wants the recording code. models\ppocr_dict.txt
rem   (75KB, the index -> character table) IS committed because a missing dict
rem   silently turns every recognised line into garbage.
rem
rem Source: the paddlex cache, which is where the previous Python backend
rem downloaded them. If you do not have it, fetch the ONNX exports from the
rem PaddlePaddle model zoo and drop them here with these exact names:
rem   models\ppocr_det.onnx   PP-OCRv6_small_det
rem   models\ppocr_rec.onnx   PP-OCRv6_small_rec
rem ===========================================================================
setlocal

set "ROOT=%~dp0.."
set "CACHE=%USERPROFILE%\.paddlex\official_models"
set "DET=%CACHE%\PP-OCRv6_small_det_onnx\inference.onnx"
set "REC=%CACHE%\PP-OCRv6_small_rec_onnx\inference.onnx"

if not exist "%ROOT%\models" mkdir "%ROOT%\models"

if exist "%ROOT%\models\ppocr_det.onnx" if exist "%ROOT%\models\ppocr_rec.onnx" (
  echo [models] already present, nothing to do
  goto :done
)

if not exist "%DET%" goto :download
if not exist "%REC%" goto :download

echo [models] copying det
copy /y "%DET%" "%ROOT%\models\ppocr_det.onnx" >nul
if errorlevel 1 (echo [models] FAILED & exit /b 1)
echo [models] copying rec
copy /y "%REC%" "%ROOT%\models\ppocr_rec.onnx" >nul
if errorlevel 1 (echo [models] FAILED & exit /b 1)
goto :done

:download
rem No local paddlex cache. Fetch the ONNX exports straight from the model
rem repo instead of failing: curl.exe ships with Windows 10 1803+ and CI has
rem no paddlex cache at all, so this is the only path the build runner can take.
echo [models] paddlex cache not found - downloading from huggingface.co
if not exist "%ROOT%\models" mkdir "%ROOT%\models"
curl.exe -fL --retry 3 --retry-delay 2 -o "%ROOT%\models\ppocr_det.onnx" ^
  "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx/resolve/main/inference.onnx"
if errorlevel 1 (echo [models] FAILED: det download & exit /b 1)
curl.exe -fL --retry 3 --retry-delay 2 -o "%ROOT%\models\ppocr_rec.onnx" ^
  "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/main/inference.onnx"
if errorlevel 1 (echo [models] FAILED: rec download & exit /b 1)

:done
if not exist "%ROOT%\models\ppocr_dict.txt" (
  echo [models] WARNING: models\ppocr_dict.txt missing - OCR output will be garbage
)
echo [models] OK
exit /b 0
