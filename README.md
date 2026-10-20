# CapWGC

A lightweight Windows screen recorder and screenshot tool built on Windows.Graphics.Capture,
Media Foundation and a hand-written MP4 muxer. No FFmpeg, no installer, no runtime dependencies.

> **Status:** 0.1.0 — recording, audio, GUI, screenshot and OCR all work end to end.
> The GUI is intentionally minimal: tray + hotkeys + region picker. No timeline, no editor.

## Why another recorder

The goal is Captura's feature set with LICEcap's startup cost and OBS's encoding quality:

- **Capture** — Windows.Graphics.Capture (WGC), so window/monitor capture keeps working
  across resolution changes and does not need a borderless window hack.
- **Encode** — Media Foundation MFTs only (H.264 / HEVC / AV1), auto-selected by hardware
  capability. FFmpeg is deliberately excluded: LGPL/GPL would poison the MIT license.
- **Mux** — own MP4 writer (fragmented MP4, `avcC`/`hvcC`/`av1C`), including `edts/elst`
  and `tfdt` so audio delay is compensated instead of drifting.
- **Audio** — WASAPI loopback and microphone → AAC. The GUI mixes them into one track;
  the CLI keeps one track per source so each can be measured.
- **Small** — a single ~800KB exe, statically linked CRT, no VC++ redistributable.

## Features

| | |
|---|---|
| Region / window / monitor recording | drag to select, or single click to pick a window |
| Encoder | H.264, HEVC, AV1 via hardware MFT when available |
| Audio | system loopback + microphone, mixed or separate tracks |
| Screenshot | snip overlay, saves to file and clipboard |
| OCR | in-process ONNX Runtime (PP-OCRv6), text straight to the clipboard |
| Hotkeys | fully rebindable, persisted |
| Output | MP4 (fMP4), VFR |

Default hotkeys (all editable in the settings window):

| Action | Hotkey |
|---|---|
| Start / stop recording | `Ctrl+Alt+A` |
| Screenshot | `Ctrl+Alt+X` |
| Source picker | `Ctrl+Alt+W` |
| Stop | `Ctrl+Alt+S` |

Settings and logs live in `%LOCALAPPDATA%\CapWGC\`.

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 (or Build Tools) with the C++ workload,
and a Windows 10/11 SDK. No CMake, no vcpkg.

```bat
build\build.bat            :: everything
build\build.bat gui        :: CapWGC.exe only
build\build.bat cli        :: CapCli.exe only
build\build.bat probe      :: encoder probe + mp4 checker
```

`build.bat` locates the MSVC toolset itself, and falls back to setting `INCLUDE`/`LIB` by hand
when `vcvars64.bat` cannot run. Output goes to `out\`.

Other targets: `ocrtest`, `dimtest`, `balloontest`, `toastclicktest`, `prtsctest` — each is a
small harness that pins one behaviour that used to break silently. They print `KEY=VALUE`
lines and are used by the scripts in `tools\`.

### Package

```bat
build\package.bat 0.1.0    :: dist\CapWGC-0.1.0.zip
```

The zip is the whole product: `CapWGC.exe`, `onnxruntime.dll`, `models\`. No installer.

### OCR models

`models\ppocr_det.onnx` and `models\ppocr_rec.onnx` are **not** committed (30MB of weights).
`models\ppocr_dict.txt` **is** committed, because a missing dictionary turns every recognised
line into garbage while costing only 75KB.

Either copy the weights from a local paddlex cache (`build\prepare-models.bat`), or fetch them
directly:

```bat
curl -L -o models\ppocr_det.onnx https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx/resolve/main/inference.onnx
curl -L -o models\ppocr_rec.onnx https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/main/inference.onnx
```

Without `models\` or `onnxruntime.dll` the OCR button is greyed out; recording and screenshots
keep working. That is why the DLL is linked with `/DELAYLOAD` and probed at startup.

## Continuous integration

`.github/workflows/build.yml` builds on `windows-latest` for every push and pull request,
downloads the OCR weights, packages the zip, and attaches it to a GitHub Release when a `v*`
tag is pushed. The workflow fails if `build.bat` fails, so a red badge means the tree does not
compile — the harnesses are not run in CI because they need a real desktop session.

## Repository layout

```
src/core/    capture, encoders, muxer, audio, recorder engine (shared by GUI and CLI)
src/app/     Win32 GUI: tray, hotkeys, overlay, picker, control bar, settings, snip, OCR glue
src/cli/     CapCli.exe, the measuring harness every script depends on
src/probe/   one-behaviour test harnesses
build/       build.bat, package.bat, prepare-models.bat
tools/       Python/shell verification scripts (see tools/requirements-ocr.txt)
docs/        design notes, measurements and post-mortems, in Chinese
```

`docs/` is the interesting part: each file records what was tried, what the numbers were, and
why the current design won. Start with `docs/01-capture-core-evaluation.md`, then `docs/09-gui.md`
and `docs/11-ocr-deployment.md`.

## License

MIT — see [LICENSE](LICENSE).

---

## 繁體中文

CapWGC 是一個 Windows 錄屏與截圖工具：WGC 抓畫面、Media Foundation 硬體編碼、
自寫的 MP4 muxer，**不用 FFmpeg**（避免 LGPL/GPL 污染 MIT）。

- 區域／視窗／螢幕錄影，H.264 / HEVC / AV1 自動選硬體編碼
- 系統聲 + 麥克風（GUI 混成一軌，CLI 分軌方便量測）
- 截圖與 OCR（行程內 ONNX，不經 Python，不用裝任何東西）
- 托盤常駐、熱鍵可自訂、無主視窗

建置：`build\build.bat`，產物在 `out\`；打包：`build\package.bat 0.1.0`。
設計取捨與量測數據都在 `docs\`，建議先看 `HANDOFF.md`。
