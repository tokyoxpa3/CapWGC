# CapWGC

[![build](https://github.com/tokyoxpa3/CapWGC/actions/workflows/build.yml/badge.svg)](https://github.com/tokyoxpa3/CapWGC/actions/workflows/build.yml)

A lightweight Windows screen recorder and screenshot tool built on Windows.Graphics.Capture,
Media Foundation and a hand-written MP4 muxer. No FFmpeg, no installer, no runtime dependencies.

![Picking a capture source](screenshots/picker.png)

> **Status:** 0.3.0 — recording, audio, GUI, screenshot, OCR, start-at-sign-in and
> 17 interface languages all work end to end. The GUI is intentionally minimal:
> tray + hotkeys + region picker. No timeline, no editor.

## Highlights

- **A 1 MB exe, nothing to install.** Static CRT, no VC++ redistributable, no
  FFmpeg, no Python. Unzip and run; deleting the folder uninstalls it.
- **Capture that survives real use.** Windows.Graphics.Capture keeps working when
  a window is resized, moved between monitors, or the resolution changes — no
  borderless-window hacks, no black frames.
- **Hardware encoding, picked for you.** H.264 / HEVC / AV1 through Media
  Foundation MFTs, chosen at startup from what the GPU actually exposes.
- **Audio stays in sync.** The MP4 writer emits `edts/elst` and `tfdt`, so a
  loopback capture that starts a few hundred milliseconds late is compensated
  instead of drifting for the rest of the file.
- **Screenshot → text in one key.** `PrtScn` takes over from the Windows Snipping
  Tool, the snip goes to file + clipboard, and OCR (PP-OCR via ONNX Runtime, in
  process) puts the recognised text straight on the clipboard.
- **Tray-first.** No main window, no dock icon, two hotkeys out of the box, all
  of them rebindable; the floating bar is the only thing on screen while recording.
- **17 languages, switched live.** The settings window rebuilds itself the moment
  you pick a language — no restart, no half-translated screen.
- **Honest defaults.** Start at sign-in is on by default and shows up in Task
  Manager's Startup tab like any other entry; `HKCU` only, no admin rights, and
  unchecking it removes the value instead of leaving a dead one behind.

## Screenshots

The picker at the top is the main window — a tray app's "main window" is the thing
you see when you click the tray icon. The other three surfaces:

**Settings** — three tabs. The start-at-sign-in switch says what it writes, to
which account, and whether it needs admin rights, because a default that touches
the registry should not be a surprise.

![Settings](screenshots/settings.png)

**Screenshot / OCR** — one overlay, two verbs: snip, or snip and recognise. The
toolbar follows the selection, so the two actions are always where you are looking.

![Snip toolbar](screenshots/snip-toolbar.png)

**Recording bar** — stop and mute, nothing else. It is excluded from the capture,
so it never appears in the file.

![Floating bar](screenshots/control-bar.png)

These are real screenshots of `out\CapWGC.exe`, not mockups.

## Why another recorder

The goal is Captura's feature set with LICEcap's startup cost and OBS's encoding quality:

- **Capture** — Windows.Graphics.Capture (WGC), so window/monitor capture keeps working
  across resolution changes and does not need a borderless window hack.
- **Encode** — Media Foundation MFTs only (H.264 / HEVC / AV1), auto-selected by hardware
  capability.
- **Mux** — own MP4 writer (fragmented MP4, `avcC`/`hvcC`/`av1C`), including `edts/elst`
  and `tfdt` so audio delay is compensated instead of drifting.
- **Audio** — WASAPI loopback and microphone → AAC. The GUI mixes them into one track;
  the CLI keeps one track per source so each can be measured.
- **Small** — a single ~1MB exe, statically linked CRT, no VC++ redistributable.

## Features

| | |
|---|---|
| Region / window / monitor recording | drag to select, or single click to pick a window |
| Encoder | H.264, HEVC, AV1 via hardware MFT when available |
| Audio | system loopback + microphone, mixed or separate tracks |
| Screenshot | snip overlay, saves to file and clipboard |
| OCR | in-process ONNX Runtime (PP-OCRv6), text straight to the clipboard |
| Window picker | thumbnail wall, live-refreshing, filters out cloaked windows |
| Print Screen takeover | `PrtScn` replaces the Windows Snipping Tool shortcut (on by default) |
| Start at sign-in | `HKCU\...\Run`, on by default, mirrors the Task Manager Startup switch |
| Settings | three tabs (general / OCR / hotkeys), all hotkeys rebindable and persisted |
| Languages | 17, picked in Settings → General; the switch is immediate, no restart |
| Output | MP4 (fMP4), VFR |

Default hotkeys (all editable in the settings window):

| Action | Hotkey |
|---|---|
| Screenshot / OCR | `PrtScn` |
| Window picker | `Ctrl+Alt+W` |
| Region / window recording | not bound — use the tray menu |
| Record primary monitor | not bound — use the tray menu |
| Stop | not bound — click the red square on the floating bar |

Only two hotkeys are bound by default. Recording is a deliberate action, so it
does not get a shortcut out of the box; bind one in Settings → Hotkeys if you want it.
Leaving a field empty is not a broken state — the tray menu and the floating bar
remain full entry points.

`PrtScn` works by turning off Windows 11's "Use the Print screen key to open
Snipping Tool" (`HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled`),
because Windows swallows the key before `RegisterHotKey` ever sees it. This is on
by default; unchecking it in Settings → OCR restores the Windows behaviour and
falls back to `Ctrl+Alt+X`. `Win+Shift+S` is unaffected either way.

Start at sign-in is also on by default, and it is the same kind of promise: it
writes exactly one value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
so it covers the current account only and needs no administrator rights.
Unchecking it deletes the value and the `StartupApproved` marker, which is why
Task Manager's Startup tab and the settings checkbox always agree — the checkbox
reads the registry, not a second copy in the ini. If the write is blocked (group
policy, security software), the box reverts and a tray balloon says so, rather
than leaving a tick that does nothing.

Settings and logs live in `%LOCALAPPDATA%\CapWGC\`.

## Languages

The interface ships in 17 languages, in this order:

`zh_TW` `zh_CN` `en_US` `es_ES` `ko_KR` `ja_JP` `pt_BR` `fr_FR` `de_DE` `ru_RU`
`it_IT` `vi_VN` `th_TH` `id_ID` `tr_TR` `pl_PL` `nl_NL`

The language is stored in `settings.ini` (`[ui] lang`). On first run the app picks the
Windows UI language; picking one in Settings → General applies immediately (the dialog
rebuilds itself) and is remembered from then on.

`locale\<code>.json` is a flat object keyed by the **Traditional Chinese source string**:

```json
{
    "設定…": "Settings…",
    "已複製 %zu 行／%zu 字，直接貼上即可": "Copied %zu lines / %zu characters — just paste"
}
```

Keys are the source text, not symbolic ids, so a missing entry simply falls back to
Chinese instead of showing `menu.settings` — and `zh_TW.json` is the list of everything
the UI can say. Strings that take parameters carry their `printf` conversions in the
translation (`%s`, `%d`, `%zu`, `%.1f`, `%hs`); keep them, or the C++ side formats
garbage.

To add a language: copy `locale/en_US.json`, translate the values (never the keys),
drop it in as `locale\<code>.json`, and add one line to `kLangs` in
`src\app\i18n.cpp`. `build.bat` copies `locale\` next to the exe, and
`package.bat` puts it in the zip.

Without `locale\` the app still runs — every string falls back to Traditional Chinese.

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

The icons are the project's only resources (`build\app.rc` → `build\app.ico` + `build\app_rec.ico`),
so the GUI target also runs `rc.exe` from the same SDK. `tools\make_icon.py` draws them rather than
cropping `icon.jpg`: that picture is a 490px banner with a film strip, an aperture, a play arrow, the
letters "QA" and a wordmark all in one square, and shrunk to 16px every one of those becomes grey
mush. The drawn mark is a dark tile with an aperture ring, corner brackets at 48px and up, and a
plain bigger ring below that. `app_rec.ico` replaces the ring with a red disc and a white stop square
while a capture is running; it is a second resource rather than a runtime overlay because compositing
an icon onto a DIB does not reliably carry the source alpha.

Other targets: `ocrtest`, `dimtest`, `balloontest`, `toastclicktest`, `prtsctest` — each is a
small harness that pins one behaviour that used to break silently. They print `KEY=VALUE`
lines and are driven by the Python scripts used during development (not part of this repo).

### Package

```bat
build\package.bat 0.3.0    :: dist\CapWGC-0.3.0.zip
```

The zip is the whole product: `CapWGC.exe`, `onnxruntime.dll`, `models\`, `locale\`.
No installer.

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
src/app/     Win32 GUI: tray, hotkeys, overlay, picker, control bar, settings, snip, OCR glue,
             i18n (locale loading + lookup)
src/cli/     CapCli.exe, the measuring harness every script depends on
src/probe/   one-behaviour test harnesses
locale/      one JSON per interface language, keyed by the source string
build/       build.bat, package.bat, prepare-models.bat, app.rc, app.ico, app_rec.ico
models/      OCR dictionary (weights are downloaded, not committed)
screenshots/ the screenshots used above, captured from the real exe
```

## License

MIT — see [LICENSE](LICENSE).

---

## 繁體中文

CapWGC 是一個 Windows 錄屏與截圖工具：WGC 抓畫面、Media Foundation 硬體編碼、
自寫的 MP4 muxer。

- 區域／視窗／螢幕錄影，H.264 / HEVC / AV1 自動選硬體編碼
- 系統聲 + 麥克風（GUI 混成一軌，CLI 分軌方便量測）
- 截圖與 OCR（行程內 ONNX，不經 Python，不用裝任何東西）
- 托盤常駐、熱鍵可自訂、無主視窗
- 開機時自動啟動（只寫 HKCU，預設開啟，工作管理員的「啟動」頁看得到也停得掉）
- 介面 17 種語言，設定視窗裡換了當場生效，不必重開

介面長相：上方 [Screenshots](#screenshots) 那四張就是實際畫面（選擇來源、設定、
截圖工具列、錄影浮動列），不是合成圖。

建置：`build\build.bat`，產物在 `out\`；打包：`build\package.bat 0.3.0`。
介面文字在 `locale\<語言>.json`，key 就是程式裡的繁體中文原文，查不到就顯示原文。
