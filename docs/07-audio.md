# 音軌狀態：WASAPI loopback + MF AAC + muxer 第二軌

> 日期：2026-10-17
> 機器：RTX 3060 Ti（hostname `tokyo`）/ console session / 工作目錄 `D:\workspace\CapWGC`
> 產出：`out/CapCli.exe --audio`、`out/mp4_check.exe --audio <file>`
> 原始碼：`src/core/audio_capture.*`（loopback）、`src/core/aac_encoder.*`（AAC）、
> `src/core/mp4_muxer.cpp`（第二條 trak/traf）、`src/cli/main.cpp`（接線）

---

## 1. 結論先講

錄屏軟體沒有聲音不能用。在這之前 CapWGC 只寫影像軌，這一份把音軌從擷取到容器
整條打通，並且用系統解碼器驗證過。

| 驗收標準 | 結果 | 數據 |
|---|---|---|
| 能錄到系統聲音 | **通過** | 同一個 `tone.wav` 兩次量到 `peak=-6.0` 與 `-13.0 dBFS`（不同的播放方式／系統音量），跟著源變 |
| 產出的檔案音軌可解 | **通過** | `decodes OK`，`44100 Hz 2 ch` |
| 音軌長度對齊影像 | **通過** | 6 秒錄影：影像 5.968s / 音訊 5.968s（差 0 ms） |
| fMP4 + HEVC | **通過** | 每個 `moof` 都有雙 `traf`（trk1 影像 + trk2 音訊） |
| progressive + H.264 | **通過** | 檔案可解，兩條軌都在 |
| 強制 kill 後檔案仍可播 | **通過** | 影像 360 幀 / 音訊 6.014s，`decodes OK` |
| 系統安靜時音軌仍然有長度 | **通過** | 7.964s、`decodes OK (silent)`（見 §4） |

一句話：**它是能用的**。但有兩個地方不是「照文件寫就會對」的：esds 的 descriptor
少包一層最外殼（§3.2），以及 WASAPI loopback 在系統安靜時一個 packet 都不送（§4）。
這兩個的症狀都不指向自己，是這份文件主要的內容。

---

## 2. 三段接線

```
WASAPI loopback（裝置 mix format）
  → 降混成 2ch / 16-bit interleaved
  → AacEncoder（MF 的 AAC encoder MFT，raw AAC-LC）
  → Encode 回呼（每個 AU = 1024 取樣點）
  → Encoder::WriteAudio → Mp4Muxer::WriteAudioSample
  → 第二條 trak（progressive）/ 第二個 traf（fMP4）
```

### 2.1 為什麼是 WASAPI loopback

錄屏的預設是錄「電腦發出的聲音」。WASAPI loopback 是唯一不用驅動、不用虛擬音效卡
就能拿到系統混音的方法（Vista 之後內建）。麥克風是另一條路，但那是「另外一個功能」，
不是替代品。

### 2.2 為什麼是 MF 的 AAC

`MFT_CATEGORY_AUDIO_ENCODER` + `MFAudioFormat_AAC` 是 Windows 內建的。用它的理由
是授權：FAAC / libfdk-aac 拉進來會讓整個專案從 MIT 變成混合授權，跟「不能用 FFmpeg」
是同一條線。

---

## 3. 容器：第二條軌

### 3.1 音軌的 timescale 用取樣率，不用 90000

影像軌用 `kTimescale = 90000`（能整除常見 fps）。音軌**不能沿用**這個值，而是直接
用取樣率（44100）當 timescale。這樣 AAC-LC 每個 AU 剛好是 1024 個 tick，`stts` 只
需要一個 run —— 如果 timescale 是 90000，每個 AU 會變成 90000×1024/44100 = 2089.79，
不是整數，`stts` 就得逐筆描述或容忍累積誤差。

時間軸累加用取樣點而不是 `pts100ns`：

```cpp
s.pts = I.audioTs;
s.dur = (int64_t)frames;
I.audioTs += (int64_t)frames;
```

`pts100ns` 只用來決定**起點**（第一次呼叫時換算成音軌 tick）：

```cpp
if (!I.audioStarted) {
    int64_t start = (pts100ns * (int64_t)I.audioRate + 5000000) / 10000000;
    I.audioStartTs = start > 0 ? start : 0;
    I.audioTs = I.audioStartTs;
    I.audioStarted = true;
}
```

這樣音軌自己內部完全沒有累積誤差，只跟影像有一份固定的起點偏移。

### 3.2 esds 的 descriptor 要包三層，少一層症狀極具誤導性

`mp4a` sample entry 裡的 `esds` 是 MPEG-4 descriptor 的嵌套結構：

```
ES_Descriptor(0x03)
  ├── ES_ID / flags
  └── DecoderConfigDescriptor(0x04)
        ├── objectTypeIndication = 0x40（MPEG-4 Audio）
        ├── streamType = 0x05（audio）
        ├── bufferSizeDB / maxBitrate / avgBitrate
        └── DecoderSpecificInfo(0x05)   ← AudioSpecificConfig（ASC）
              └── SLConfigDescriptor(0x06)
```

**最外層的 `ES_Descriptor(0x03)` 很容易漏掉**，因為裡面的 `0x04 → 0x05 → 0x06`
三層看起來就已經是完整的樹了。漏掉的症狀是：

- box 結構怎麼看都合法，`moov` 裡確實有兩條 `trak`
- `mp4_check` 的一般模式打得開、影像完全正常
- 但 Media Foundation 的 source reader 只認得影像軌，`--audio` 回
  `no audio stream (0xC00D36B3)`

修法（`Mp4Muxer::Impl::BuildStsdAudio()`）：

```cpp
Bytes esAll;
Desc(esAll, 0x03, esP);                          // 最外層 ES_Descriptor
Bytes esds = FullBox("esds", 0, 0, esAll);       // 然後才是 full box
```

`Desc()` helper 一律用 4-byte expandable 長度（不是 1-byte），避免 payload 超過 127
bytes 時長度欄位靜默截斷。

### 3.3 ASC 從 `MF_MT_USER_DATA` 拿，不要自己拼

輸出型別的 `MF_MT_USER_DATA` 是一個 `HEAACWAVEINFO`（12 bytes）+ AudioSpecificConfig
的 blob。前 12 bytes 之後的那段就是 esds 的 `DecoderSpecificInfo`，直接原樣放進去。

自己拼 ASC（從取樣率和聲道數算 `objectType` + `freqIndex` + `channelConfig`）很容易
跟編碼器實際用的參數不一致 —— 特別是 SBR（HE-AAC）或非 48k 的情況。備援路徑留著
（`FreqIndex()` + 2 聲道自拼 2-byte ASC），但正常情況下不該走到。

實測這台的 ASC 是 2 bytes，日誌會印：

```
[aac] MF_MT_USER_DATA 14 bytes -> ASC 2 bytes
```

`MF_MT_AAC_PAYLOAD_TYPE = 0` 也是必須的：預設輸出是帶 ADTS 標頭的 AU，MP4 要的是
raw。ADTS 標頭進了 `mdat`，播放器會把它當成音訊資料解，結果是雜音。

### 3.4 fMP4：一個 moof 兩個 traf，共用一個 mdat

有音軌時：

```
moof
  ├── mfhd
  ├── traf (track 1 = video)   trun.data_offset → video payload 起點
  └── traf (track 2 = audio)   trun.data_offset → audio payload 起點
mdat
  ├── video payload（在前）
  └── audio payload（在後）
```

兩個 `trun` 的 `data_offset` 各自指向自己的第一筆樣本。偏移的計算方式：

```cpp
const size_t baseV = 8 + mfhd.size();
const size_t baseA = baseV + trafV.size();
const uint32_t dataOffV = (uint32_t)(moof.size() + 8);
const uint32_t dataOffA = (uint32_t)(dataOffV + fragData.size());
```

分片仍然自包含（`default-base-is-moof`），所以被 kill 之後每一片都能單獨解。
音訊分片累積到自己滿 3 秒而影像分片還是空的時候（畫面完全不動），音訊會自己落一片，
`traf` 只有音軌、`tfdt` 照樣接得下去。

---

## 4. loopback 在系統安靜時不送 packet

這是整個任務裡最不直覺的一個。

### 症狀

- 錄 12.98 秒，AU 數 559（看起來很正常），但 `mp4_check --audio` 回
  `peak=-999.0 dBFS`，也就是**整條軌全是靜音**
- `loop.frames()` 是 **0** —— 一個 PCM 取樣點都沒收到
- 影像完全正常

### 原因

沒有人在放聲音時，Windows 的音訊引擎會進入 idle，loopback 串流**一個 packet 都不送**。
不是「送靜音」——是完全沒有事件。所以 `WaitForSingleObject` 一直等不到東西，
`frames()` 停在 0。但 AAC 編碼器那側因為收到 0 個取樣點，反而什麼都沒編，AU 數
看起來「正常」是因為那是別的原因（見下）。

### 解法：自建靜音渲染串流當 keep-alive

在同一個端點上另外開一條 `IAudioClient` 播放串流，持續餵零：

```cpp
void KeepAliveFill() {
    if (!keepAlive || !rrender || !rclient || rbufferFrames == 0) return;
    UINT32 pad = 0;
    if (FAILED(rclient->GetCurrentPadding(&pad))) return;
    const UINT32 avail = pad < rbufferFrames ? rbufferFrames - pad : 0;
    if (avail == 0) return;
    BYTE* p = nullptr;
    if (FAILED(rrender->GetBuffer(avail, &p)) || !p) return;
    memset(p, 0, (size_t)avail * (size_t)(wf ? wf->nBlockAlign : 4));
    rrender->ReleaseBuffer(avail, 0);
}
```

它混進系統混音裡什麼都聽不到，但引擎會一直運轉，loopback 就會穩定送資料
（安靜時送帶 `AUDCLNT_BUFFERFLAGS_SILENT` 的 packet）。執行緒迴圈每 20 ms
`Drain()` 一次之後 `KeepAliveFill()`。

### 驗證

修好之後錄 7.964 秒全靜音：音軌 7.964s、`decodes OK (silent)` —— 長度正確了。
有聲音時也正常：放測試音錄 5 秒，`peak=-13.0 dBFS`（跟著系統音量變），
`decodes OK`。

---

## 5. 其他兩個坑

### 5.1 `AUDCLNT_BUFFERFLAGS_SILENT` 一定要看

loopback 在靜音時**仍然會送資料**，只是 flag 帶 `AUDCLNT_BUFFERFLAGS_SILENT`，
而且緩衝區內容是**未定義**的（不是零）。不看 flag 直接送出去會得到雜音。
這是 loopback 最經典的 bug。

### 5.2 尾端不足一幀：AAC 是 1024 取樣點一幀

最後不足 1024 的部分留在編碼器裡，`END_OF_STREAM` 不一定會吐出來 ——
實測 6 秒的錄影會少掉 58 ms 的尾巴（audio 5.875s vs video 5.988s）。

修法：`AacEncoder::Flush(targetFrames, ...)` 先用靜音補到 `targetFrames`（對齊 1024）
再收尾。`targetFrames` 由影像長度換算（`lastPts - firstPts + lastDur`），
代價是最後多幾毫秒靜音，換來兩條軌時長一致。修好後是 0 ms 差。

---

## 6. 其他實作細節

**降混與整流**（`Convert()`）：裝置可能是 6ch float32，一律降到 2ch / 16-bit
interleaved，>2 聲道取前兩聲道。float32 → int16 用 **32767** 而不是 32768，
避免 +1.0 溢出成一個爆音。

**取樣率與聲道數由裝置決定**，不是我們指定。硬要 48k 而裝置是 44.1k 只會拿到
`AUDCLNT_E_UNSUPPORTED_FORMAT`。所以照 mix format 開，再把 >2 聲道降下來。

**時間戳用累計取樣點 / 取樣率**，不用 QPC。音訊是連續的，用裝置時鐘才不會在長錄影
裡慢慢漂走；代價是它與影像的對齊精度只有「啟動延遲」那個量級（實測 10~30 ms）。

**執行緒安全**：`Mp4Muxer` 不是執行緒安全，音訊來自 loopback 的背景執行緒，
`MftEncoder::WriteAudio` 用既有的 `mu` 鎖；`bytes()` 也因為會被音訊執行緒間接碰到，
`mu` 改成 `mutable` 並加鎖（否則會在別人 `fwrite` 中間呼叫 `_ftelli64`）。

---

## 7. 量測指令

```bat
:: 錄系統聲音
out\CapCli.exe --hwnd <hwnd> --out a.mp4 --seconds 6 --codec hevc --quality 60 --audio

:: 改碼率
out\CapCli.exe --hwnd <hwnd> --out a.mp4 --seconds 6 --audio --audio-bitrate 192000

:: 驗證（影像 + 音軌分開量）
out\mp4_check.exe a.mp4
out\mp4_check.exe --audio a.mp4
```

`--audio` 失敗時不會讓錄影失敗 —— 會印 `audio: unavailable (...)` 然後繼續錄無聲影片。
這是刻意的：沒聲音總比沒畫面好。

### 一次完整驗證的輸出

```
audio   : 44100 Hz 2 ch  asc=2B
audio         : 214 AU  pcm=5.02s au=214 bytes=79505 asc=2B  errors=0

aud_tone2.mp4                44100 Hz 2 ch  frames=219136  4.969s  peak=-13.0 dBFS  rms=-16.1 dBFS  decodes OK
```

### 強殺驗證（fMP4）

錄 30 秒，第 7 秒 `taskkill /F`：

```
aud_kill3.mp4                1282x752 (coded 1312x768)  frames=360   last_ts=6.101s  decodes OK
aud_kill3.mp4                44100 Hz 2 ch  frames=265216  6.014s  peak=-999.0 dBFS  decodes OK (silent)

moof#1: trk1=120  trk2=87
moof#2: trk1=120  trk2=86
moof#3: trk1=120  trk2=86
total moof=3
```

每個 `moof` 都有雙 `traf`，影像 120 幀 / 音訊 86~87 AU（各約 2 秒）。修 keep-alive
之前錄的舊檔（`aud_kill.mp4`）只有第 3 個 `moof` 帶 audio —— 那是 idle 不累積造成的。

---

## 8. 還沒做的

- ~~**多軌音訊**（麥克風 + 系統混音分開成兩條軌）~~ **已完成（2026-10-17）**，
  見 `docs/08-multi-audio.md`。現在是「任意條」：`--audio` / `--mic` /
  `--audio-src` 各成一條 `trak`，時鐘與取樣率互不相干。
- ~~**裝置選擇**（`--audio-device`）~~ **已完成**。`--audio-device` / `--mic-device`
  吃索引、名稱子字串或 endpoint ID；`--list-audio-devices` 列出全部端點。
- **AAC 以外的選項**（Opus）。NVENC 那側沒有音訊，MF 有 AAC 但沒有通用的 Opus 編碼器。
- **音視訊偏移的可調補償**。現在偏移只有「啟動延遲 + 一幀間隔」那個量級且不可調。
  多軌之後這件事更明顯：靜止畫面掉到 15fps 時，音訊會早影像約 66ms。
- **`--audio` 的取樣率上限檢查**。192k 裝置會直接照開，碼率可能要跟著調。
