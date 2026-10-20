# 多音訊源：系統聲與麥克風各自一條軌

> 日期：2026-10-17
> 機器：RTX 3060 Ti（hostname `tokyo`）/ console session / 工作目錄 `D:\workspace\CapWGC`
> 產出：`out/CapCli.exe`（`--audio` / `--mic` / `--audio-src` / `--audio-device` /
> `--mic-device` / `--list-audio-devices`）、`out/mp4_check.exe`（`--audio` / `--tracks` /
> `--streams`，`--tracks` 會印 `edts/elst` 與 `tkhd−mdhd` 的延遲）、
> `out/mp4_frags.py`（每個 `moof` 的 `traf`、樣本數與 `tfdt`）
> 原始碼：`src/core/audio_capture.*`（loopback + 麥克風 + 端點列舉）、
> `src/core/mp4_muxer.*`（N 條音軌、`edts/elst`、`tfdt` 起點）、
> `src/core/encoder.h` / `mft_encoder.*`（多軌介面 + `SetAudioOffset`）、
> `src/cli/main.cpp`（多來源接線、A/V 偏移量測與補償）

---

## 1. 結論先講

上一份（`docs/07-audio.md`）只有一條軌：系統混音。這一輪把它變成**任意條**，
每條來源一個 WASAPI 串流、一個 AAC 編碼器、容器裡一條獨立的 `trak`。

| 驗收標準 | 結果 | 數據 |
|---|---|---|
| 單軌回歸（不能弄壞原本的） | **通過** | 6s：影像 5.313s / 音訊 5.898s，`peak=-6.0 dBFS`，`decodes OK` |
| 系統聲 + 麥克風同時錄 | **通過** | 兩條軌各自可解：44.1k（loopback，rms −9.0）/ 48k（mic，rms −36.5） |
| 三條來源（含第二個播放裝置） | **通過** | 3 條 trak，時長 6.246 / 6.251 / 6.251s，全部 `decodes OK` |
| 兩條 loopback 抓同一個端點 | **通過** | 兩軌完全一致（215 AU、peak −5.3、rms −9.1）—— 證明是真的兩條獨立串流 |
| fMP4 + 雙音軌，強制 kill | **通過** | 影像 360 幀、音訊 6.016s / 6.014s 都可解；每個 `moof` 三個 `traf` |
| progressive + 雙音軌 | **通過** | 影像 4.984s、兩條音軌 4.969 / 4.971s，都可解 |
| 裝置選擇（索引 / 名稱 / 中文名） | **通過** | `--mic-device 麥克風` 命中；`Realtek` 歧義時報錯而不是亂猜 |
| 來源開不起來時不拖垮錄影 | **通過** | 印一行 `略過這條音源`，影片照錄，軌號自動遞補 |
| **A/V 偏移已量化**（不是常數） | **通過** | QPC 記帳單路 56.2~59.4ms、`--ab` 125.8~128.3ms、`--ab --ref` 163.8ms；軌間只差 1.2~1.3ms（§6.3） |
| **A/V 偏移能寫進容器**（progressive / fMP4 兩條路） | **通過** | 宣告 60ms → 實測呈現差 +57.5（`elst`）/ +65.0（`tfdt`）；宣告 500ms 把真實 500ms 延遲抵到 +15.5ms（§6.5） |
| **A/V 偏移的推導值可信** | **通過（2026-10 改寫）** | 推導值 = `firstPacketQpc − videoOrigin` = 音軌比影像零點晚起跑多少，就是該補的量。舊的「不通過」結論是素材源 90ms 延遲與 CLI 86ms gap 互相抵掉造成的假象（§6.6.1） |
| **GUI（`Recorder`）與 CLI 兩條路一致** | **通過** | `verify_sync.sh`：CLI +0.7ms、GUI +0.2ms、兩者差 0.5ms。修正前是 +5.8 / +67.5（差 62ms），那個差就是 `gap` 的差 |
| **負偏移（音訊偏晚）能裁** | **通過** | fMP4：−150ms 指示 → 量到 −136.6（丟 6 AU，floor 量化）；progressive：−150ms 指示 → `elst media_time=6615/44100 = 150.0ms`，量到 −151.0（§6.7） |
| **換硬體不用重寫程式** | **通過** | 偏移由 `out/calibrate.sh` 兩輪量測（2000ms 定向 + 400ms 精修）解混疊後寫成校準檔；`--av-offset auto` 讀它。本機量到 +6.3ms → 落在 ±10ms 門檻內 → 寫 0（§6.7） |
| **校準腳本不會寫出壞值** | **通過** | 兩輪差距 >60ms 就拒絕寫檔；無校準檔時 `auto` 出聲明講而不是靜默當 0；只要負補償小於一個 AU 就警告 fMP4 無效 |

一句話：**能用了，而且兩條軌是真的獨立**（不同取樣率、不同裝置、不同時鐘）。
最花時間的不是容器，是兩個「看起來不像自己」的坑：MF 的 stream 索引不照 `trak`
順序（§5.1），以及命令列的 argv 是 ANSI（§5.2）。

> **2026-10-18 補記：GUI 不再分軌。**
> 這份文件寫的是 **CLI**（`CapCli`）的行為，它仍然是每條來源一條 `trak`。
> GUI 的 `Recorder` 改成把多個來源**混成一條軌**（`RecorderConfig::mixAudio`，
> 預設開）：實測發現 MP4 有多條音軌時，大部分播放器只播第一條，而第一條是
> loopback —— 使用者勾了麥克風卻聽不到，聲音其實在第二條。
> 混音的實作與驗證在 `docs/09` §4.7.1。CLI 保持分軌是刻意的：`mp4_check --audio`
> 要能逐條量測，那是 A/V 對齊量測的基礎。


A/V 偏移這一段最後是**推翻了自己先前的結論**。容器那一半做對了 ——
`elst` 與 `tfdt` 都真的生效，宣告多少播放器就位移多少（§6.5 的 P1/F1）。
但「偏移有多大」那一半量錯了：QPC 記帳得到的 56~128ms 是兩條管線的延遲差，
不是音畫差；真正用內容事件量出來是 −1.3~+10.9ms，也就是**本來就對齊**。
所以補償機制留著（C2 證明它能抵掉真實延遲），但預設不再猜一個值（§6.6）。

---

## 2. 架構

```
loopback（render 端點）→ WASAPI → 降混 16-bit → AAC #0 → trak 2 ┐
mic     （capture 端點）→ WASAPI → 降混 16-bit → AAC #1 → trak 3 ├→ 同一個 muxer
loopback（第二個裝置）  → WASAPI → 降混 16-bit → AAC #2 → trak 4 ┘
```

三條來源各自：

- 一條 `IAudioClient`（loopback 再加一條靜音渲染串流當 keep-alive）
- 一條背景執行緒（`WaitForSingleObject` + `Drain`）
- 一個 `AacEncoder`（MF 的 AAC-LC，碼率各自可調）
- 一個 `track` 索引（0 起算，容器裡是 `track_ID = 2 + index`）

共同的部分只有 muxer，由 `MftEncoder::WriteAudio` 的 `mu` 序列化 —— 三條執行緒
同時寫進來，鎖在 encoder 層，muxer 本身仍然不是執行緒安全的。

**為什麼每條軌都要自己的 AAC 編碼器**：取樣率不同（這台是 44.1k 的喇叭 + 48k 的
麥克風）。一個 AAC MFT 只能有一種輸出格式，硬要共用就得先重取樣 —— 那是另一個
元件的責任，而且會把兩條軌的時鐘綁在一起。

---

## 3. 擷取層：同一個類別，兩種端點

`LoopbackCapture` 改名 `AudioCapture`，加一個 `kind`：

| kind | 端點 | 串流旗標 | keep-alive |
|---|---|---|---|
| `Loopback` | `eRender`（播放） | `LOOPBACK | EVENTCALLBACK` | **需要**（見 `docs/07` §4） |
| `Input` | `eCapture`（錄音） | `EVENTCALLBACK` | 不需要（端點自己會送資料） |

其餘完全相同：照裝置的 mix format 開、`>2` 聲道降到 2ch、float32 → int16 用
32767、`AUDCLNT_BUFFERFLAGS_SILENT` 一定要看、時間戳用累計取樣點 / 取樣率。

### 3.1 端點列舉

`EnumerateAudioDevices()` 回傳「播放端點全部 + 錄音端點全部」，順序固定（先 render
再 capture）。`--list-audio-devices` 就是它：

```
flow     idx  name                                           note
render   0    Realtek Digital Output (Realtek(R) Audio)
render   1    Realtek HD Audio 2nd output (Realtek(R) Audio)  (default)
render   2    27G2G8 (NVIDIA High Definition Audio)
capture  0    立體聲混音 (Realtek(R) Audio)
capture  1    麥克風 (Realtek(R) Audio)                    (default)
```

friendly name 要 `IMMDevice::OpenPropertyStore` + `PKEY_Device_FriendlyName`。
那個 `PROPERTYKEY` 定義在 `functiondiscoverykeys_devpkey.h`，要靠 `INITGUID` 才生得出
符號 —— 這裡直接把 GUID 寫死（`a45c254e-df1c-4efd-8020-67d146a850e0`，pid 14），
省掉一個編譯期開關，代價只是多四行。

### 3.2 裝置規格的三種寫法

`--audio-device` / `--mic-device` 吃三種：完整 endpoint ID、數字索引（該種類裡的
序號）、friendly name 的子字串（不分大小寫，中文照樣比）。

子字串命中多個時**直接報錯**，不挑第一個：

```
audio[0]: loopback unavailable (device spec "Realtek" matches 2 endpoints --
          use the index from --list-audio-devices) -- 略過這條音源
```

挑錯裝置的後果是「錄到了，但錄的是另一個螢幕的聲音」，比明確失敗難查得多。

---

## 4. 容器：N 條音軌

### 4.1 每條軌自己的時間軸

`Impl::AudioTrack` 各自持有 `ts`（已寫入取樣點）、`startTs`、`nextDts`、`startTicks`
（起始延遲，換算成自己的取樣點）。**不能共用**：兩條來源的取樣率可能不同
（44.1k / 48k），共用一個累加器等於假設同一個時鐘。

`timescale = 該軌的取樣率` 這件事在多軌下更重要 —— 44.1k 與 48k 混在同一個檔是
合法的，但每一條的 `mdhd.timescale` 必須是自己的值，`stts` 的 delta 才會剛好是
1024（AAC-LC 一個 AU）。

### 4.2 fMP4：一個 moof、1 + N 個 traf

```
moof
  ├── mfhd
  ├── traf (trk1 = video)   data_offset → 影像 payload
  ├── traf (trk2 = audio0)  data_offset → 第一條音軌 payload
  └── traf (trk3 = audio1)  data_offset → 第二條音軌 payload
mdat
  ├── video payload
  ├── audio0 payload
  └── audio1 payload
```

`data_offset` 的算法是「moof 大小 + 8 + 前面所有成員的 payload」，`traf` 的
`data_offset` 欄位位置則是「前面所有 traf 的大小」累加。兩者順序必須一致：
moof 裡的 traf 順序、mdat 裡的 payload 順序、累加的順序，三份都一樣。

這裡刻意寫成一個 `std::vector<Part>` 迴圈而不是 `trafV` / `trafA` 兩個變數 ——
兩條音軌還能硬寫，三條就會開始出錯，而錯的方向是「檔案結構看起來全對、只有
某一條軌解出來是雜訊」。

### 4.3 三條「不能省」的欄位

| 欄位 | 多軌時的意義 | 寫錯的後果 |
|---|---|---|
| `tkhd.alternate_group` | 全部留 **0** | 設成同一個非零值等於告訴播放器「這幾條是互斥版本」，一次只播一條 —— 系統聲與麥克風聽不到一起 |
| `tkhd.duration` | 用**該軌自己**的時長 | 之前兩條軌都填 `movieDur`，短的那條在播放器裡會被拉長（表現為尾巴一段靜音） |
| `hdlr` 的 name | `loopback: <裝置名>` / `mic: <裝置名>` | 播放器的音軌選單只剩「Track 1 / Track 2」，分不出哪條是麥克風 |

`hdlr` 的 name 是 UTF-8 中文字串（裝置名本身就是中文），VLC / mpv 直接顯示，
MF 不在意。這是最便宜的可辨識性。

第四個欄位只在「這條軌比影片晚開始」時才寫：`trak/edts/elst`。它不影響多軌能不能
同時播，但少了它聲音就跟畫面差一個 `enc_open`（§6.4）。

### 4.4 收尾：每條軌各自補到影像長度

`AacEncoder::Flush(targetFrames)` 的 `targetFrames` 要用**該軌自己的取樣率**換算：

```cpp
const int64_t targetFrames = video100 * s->cap->format().sampleRate / 10000000;
```

共用一個 `targetFrames` 會讓 48k 那條少掉 8% 的尾巴。

---

## 5. 兩個坑

### 5.1 MF 給的 stream 索引不照 moov 的 trak 順序

這是這輪最花時間的一個，因為它不會讓任何東西失敗 —— 只是讓你**驗錯對象**。

雙音軌檔的 moov（`--tracks` 直接讀 box）：

```
trak 0  id=1  vide hvc1  ts=90000     5.842s  1600x900  VideoHandler
trak 1  id=2  soun mp4a  ts=44100     5.921s  0x0  loopback: Realtek HD Audio 2nd output
trak 2  id=3  soun mp4a  ts=48000     5.931s  0x0  mic: 麥克風 (Realtek(R) Audio)
```

Media Foundation 的 source reader 給的索引（`--streams`）：

```
stream 0  audio  48000 Hz 2 ch      ← 這是 mic（trak 3）
stream 1  audio  44100 Hz 2 ch      ← 這是 loopback（trak 2）
stream 2  video  CVEH  0x0
```

也就是說：**音軌在前、影像在後，而且兩條音軌是反序的**。舊的 `mp4_check --audio`
用 `MF_SOURCE_READER_FIRST_AUDIO_STREAM` 只拿第一條，所以這個順序問題在單軌時
完全看不出來；一旦有兩條，就會把麥克風的數字當成系統聲的。

處理方式不是「猜一個規則」（我量到的兩個檔都符合「時長長的在前」，但兩條時長
幾乎相同時這個規則無法驗證，也不該依賴），而是**不要用序號對應**：

- `mp4_check --tracks`：自己解 moov，列出 trak 的 id / 編碼 / timescale / 時長 / 名稱
- `mp4_check --streams`：列出每個 MF stream 索引的真正格式（音訊帶取樣率）
- `mp4_check --audio`：逐條解，印的是 **stream 索引 + 取樣率 + 峰值**，不是「第幾條」

三者交叉看，才不會把兩條軌的數字張冠李戴。

### 5.2 argv 是 ANSI，不是 UTF-8

`SetConsoleOutputCP(CP_UTF8)` 只影響輸出。`main(int argc, char** argv)` 的 argv 是
Windows 用 **CP_ACP**（繁中機器是 936）從 wide 命令列轉出來的，所以：

```
--list-audio-devices        → 麥克風 (Realtek(R) Audio)     ← 印得出來
--mic-device 麥克風          → no capture endpoint matches "???J??"  ← 比不到
```

症狀是「清單裡明明有這個名字，指定它卻說找不到」，很容易誤判成字串比對寫錯。

修法是在 `main` 開頭就換掉命令列來源：

```cpp
LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
// 每個參數 WideCharToMultiByte(CP_UTF8) 之後再解析
```

順帶把中文輸出路徑也修好了（`--out D:\錄影\a.mp4` 以前同樣是亂碼）。

---

## 6. 驗證

### 6.1 指令

```bat
:: 端點清單
out\CapCli.exe --list-audio-devices

:: 系統聲一條
out\CapCli.exe --out a.mp4 --seconds 6 --audio

:: 系統聲 + 麥克風（兩條軌）
out\CapCli.exe --out b.mp4 --seconds 6 --audio --mic

:: 指定裝置（索引 / 名稱子字串 / endpoint ID 都可以）
out\CapCli.exe --out c.mp4 --seconds 6 --audio --audio-device 1
out\CapCli.exe --out c.mp4 --seconds 6 --mic --mic-device 麥克風

:: 任意多條（可重複）
out\CapCli.exe --out d.mp4 --seconds 6 --audio --mic --audio-src loopback:0

:: 每條軌的 AAC 碼率
out\CapCli.exe --out e.mp4 --seconds 6 --audio --mic --audio-bitrate 192000

:: 驗證
out\mp4_check.exe --tracks  b.mp4      :: moov 裡有哪些 trak、叫什麼名字、延遲多少
out\mp4_check.exe --streams b.mp4      :: MF 給的 stream 索引與真正格式
out\mp4_check.exe --audio   b.mp4      :: 逐條解，量峰值/RMS
python out\mp4_frags.py out\m_kill.mp4 :: fMP4 每個 moof 的 traf、樣本數與 tfdt

:: A/V 偏移補償（預設是 fMP4；--progressive 才走 elst）
out\CapCli.exe --out g1.mp4 --seconds 6 --audio --progressive
out\CapCli.exe --out g2.mp4 --seconds 6 --audio --mic --progressive
out\mp4_check.exe --tracks g2.mp4      :: 兩條音軌各一條 elst，影像那條沒有
out\mp4_frags.py f2.mp4                :: 音軌第一片的 tfdt 不為 0 = 延遲
```

### 6.2 量到的數字

**系統聲 + 麥克風（6s，同時播放測試音）**

```
trak 0  id=1  vide hvc1  ts=90000   5.842s  1600x900  VideoHandler
trak 1  id=2  soun mp4a  ts=44100   5.921s  loopback: Realtek HD Audio 2nd output
          audio: 2 ch @ 44100 Hz
trak 2  id=3  soun mp4a  ts=48000   5.931s  mic: 麥克風 (Realtek(R) Audio)
          audio: 2 ch @ 48000 Hz

stream 0  48000 Hz 2 ch  frames=284672  5.931s  peak=-6.8  rms=-36.5  decodes OK   ← mic
stream 1  44100 Hz 2 ch  frames=261120  5.921s  peak=-4.5  rms=-9.0   decodes OK   ← loopback
```

兩條軌的 RMS 差 27 dB —— 系統聲那條是乾淨的測試音，麥克風那條是喇叭漏音（−36.5）。
這個差距本身就是「兩條軌沒有互相污染」的證據。

**三條來源（系統聲 + 麥克風 + 第二個播放裝置）**

```
trak 1  id=2  mp4a  ts=44100  6.246s  loopback: Realtek HD Audio 2nd output
trak 2  id=3  mp4a  ts=48000  6.251s  mic: 麥克風 (Realtek(R) Audio)
trak 3  id=4  mp4a  ts=48000  6.251s  loopback: Realtek Digital Output

stream 0  48000  6.251s  peak=-999.0  decodes OK (silent)   ← 沒人播放的裝置
stream 1  48000  6.251s  peak=-7.2    rms=-41.3            ← mic
stream 2  44100  6.246s  peak=-5.6    rms=-9.3             ← 預設裝置的 loopback
```

閒置的裝置（Digital Output）整條是靜音，但**長度正確、解得開** —— keep-alive 讓
它照樣持續送資料，不是「0 個 packet」。這是 `docs/07` §4 那個坑在多軌下的等價驗證。

**強制 kill（fMP4，雙音軌）**

```
out\mp4_check.exe out\m_kill.mp4        1600x900  frames=360  last_ts=6.105s  decodes OK
out\mp4_check.exe --audio out\m_kill.mp4
  stream 0  48000 Hz  6.016s  peak=-7.9   rms=-40.9  decodes OK
  stream 1  44100 Hz  6.014s  peak=-6.0   rms=-9.0   decodes OK

moof#1  trk1=120  trk2=85  trk3=93
moof#2  trk1=120  trk2=88  trk3=95
moof#3  trk1=120  trk2=86  trk3=94
```

每個分片三條軌都在，kill 之後兩條音軌都解得出 6 秒。`--tracks` 對這個檔顯示
時長 0 —— 那是 fMP4 被 kill 時的正常狀態（`moov` 的時長靠 `Close()` 回填，
播放器改用分片累加）。

**兩條 loopback 抓同一個端點**

```
stream 0  44100 Hz  4.992s  peak=-5.3  rms=-9.1  decodes OK
stream 1  44100 Hz  4.992s  peak=-5.3  rms=-9.1  decodes OK
```

兩軌逐位元相同是預期結果（同一批取樣點、同一個編碼器參數）。這個測試要證明的是
**同一個端點上可以同時開兩條 loopback + 兩條 keep-alive 渲染串流**，不會互相踢掉。

### 6.3 QPC 記帳：`CapCli` 印的那三個數字

> **先看 §6.6。** 這一節的數字是「時間戳記帳」，不是音畫差。它們看起來像偏移，
> 但實測下來跟真正的音畫差差了 60ms 以上 —— 差的那 60ms 是管線延遲，不是偏移。
> 這一節保留是因為它解釋了「為什麼當初會以為有 60ms」。

`CapCli` 每次都會印一行量測結果：

```
sync    : enc_open=77.8ms  audio_start=59.5ms  raw_delay=59.5ms（是管線時間差，不是音畫差）
a/v offset    : raw_delay=59.5 ms  qpc0=72.0 ms（兩個都不是音畫差）  寫入=0.0 ms  (不補償)
              沒寫進容器：推導值不可信，預設不補償（見 docs/08 §6.6）
audio track 0   : loopback raw=+59.5ms qpc0=+72.0ms（都不是音畫差）  drift=-2.2ms  128 AU  ...
audio track 1   : mic      raw=+58.8ms qpc0=+58.8ms（都不是音畫差）  drift=-15.1ms  279 AU  ...
```

量的是三件事，全部用 QPC（WGC 的 `SystemRelativeTime` 就是這個時鐘）：

| 符號 | 定義 | 這一台量到 |
|---|---|---|
| `enc_open` | 第一幀回呼 → 所有 encoder `Open()` 完成 | 73.0 ~ 141.9 ms |
| `raw_delay` | 影像零點（第一筆寫入的幀）→ `Start()` 回來 | 56.2 ~ 127.0 ms |
| `qpc0` | 第一個 loopback packet 的 QPC − 影像零點 | 58 ~ 76 ms |
| 軌間差 | 第 N 條音源 − 第 1 條音源 | 1.2 ~ 1.3 ms |
| `drift` | 累計取樣點/取樣率 − 牆鐘 | 60s: −9.6ms / 120s: −1.1ms（不是漂移，是停止時的緩衝殘量） |

**主導項是 `enc_open`，不是「一幀」。** 這是先前寫錯的地方：之前以為音訊只早一幀
（60fps 16ms），實際上早的是「encoder 初始化」那段。同一台機器、同一支程式，
量到 56 ~ 127ms，遠大於一幀。

它會隨初始化的工作量變：

| 情境 | `raw_delay` |
|---|---|
| 單路 encoder（H.264 或 HEVC，桌面 1600×900） | 56.2 ~ 59.4 ms |
| 單路 encoder（60 秒 / 120 秒長錄影，機器閒置一段時間後） | 99.4 ~ 112.5 ms |
| `--ab`（兩路同時開） | 125.8 ~ 128.3 ms |
| `--ab --ref`（三路同時開） | 163.8 ms |

這段時間是「MFT 建立 + NVENC session 建立 + `SetOutputType` + muxer 開檔」，全部跟
GPU 世代、驅動版本、codec、是不是冷啟動有關。**但它不是音畫差** —— 因為音訊是在
同一個時刻（第一筆寫入的幀）才 `Start()` 的，兩邊的內容起點其實一樣。詳見 §6.6。


多條音軌之間反而很準：兩條來源差 1.2~1.3ms（AAC 一幀是 23ms），聽不出來。
**這個偏移只存在於 A/V 之間，不在音軌之間。**

還有一種「會隨時間變」的偏移：音訊裝置時鐘與 QPC 的漂移。量法是
`累計取樣點 / 取樣率` 對 `牆鐘`，`CapCli` 印在 `drift=` 欄位。實測 60 秒 −9.6ms、
120 秒 −1.1ms —— 沒有隨時間變大，所以那不是漂移，是停止時 loopback 緩衝還沒抽完的
殘量。**兩分鐘內量不到可累積的漂移**，也就是說這個偏移是一次性的常數，不會越錄越歪。

### 6.4 補償：容器能表達延遲的只有兩個地方

直覺做法是把第一筆音訊的 pts 設成 `raw_delay`。但 MP4 的 `stts` 只存**相鄰差**，
第一筆的 decode time 恆等於 0 —— 傳進去的絕對起點會被吃掉。所以「這條軌晚 57ms
開始」只能寫在：

- **progressive**：`trak/edts/elst`，兩筆 entry。第一筆是 empty edit
  （`media_time = -1`，只佔時間軸、不對應任何 sample），`segment_duration` 就是延遲；
  第二筆才是真正的媒體（`media_time = 0`）。`tkhd.duration` 要跟著變成
  `延遲 + 媒體`，`mdhd.duration` 保持媒體本身的長度 —— 兩個數字不一致是正常的，
  差值就是延遲，可以拿來交叉驗證。
- **fMP4**：第一個 `moof` 裡那條軌的 `tfdt`（`baseMediaDecodeTime`）起點。
  `nextDts` 初始化成 `startTicks` 而不是 0，後面每一片自然接得下去。fMP4 不需要
  `elst`，多寫反而多一個出錯的地方。

介面上是 `Encoder::SetAudioOffset(track, offset100ns)` → `Mp4Muxer::SetAudioStartOffset`，
也可以在建檔時從 `AudioTrackConfig::startOffset100ns` 帶進去。實務上走前者，因為
**開檔的當下還不知道延遲是多少**（那要等第一幀回呼跑完 encoder Open 才知道）。

還有一個更根本的問題：**量的基準是什麼**。原本音訊在第一幀回呼裡就起跑，而那時
檔案時間的零點還沒定 —— 影像時間軸的零點其實是「第一筆**寫入**的幀」，也就是
第二幀（第一幀只拿來讀尺寸、開編碼器）。所以現在的做法是：

1. 第一幀：只開編碼器，不寫入，也不啟動音訊。
2. 第二幀（第一筆寫入的幀）：把它的 `pts` 定為檔案時間 0，然後才 `Start()` 音訊。
3. **第一個 AAC AU 產生之前**呼叫 `SetAudioOffset`。這一步不能更早 —— 偏移要用
   「第一個 loopback packet 的 QPC」算，而那個 QPC 要等第一次 `Drain()` 才拿得到。
   也不能更晚 —— `SetAudioStartOffset` 會拒絕（`nextDts` 已經往前跑過就代表 `tfdt`
   已經落地），`CapCli` 會印一行「延遲沒寫進容器」而不是靜默產出一個沒補償的檔案。
   時序上安全：一個 AU 要 1024 個取樣點（44.1k 下 23ms），第一次 `Drain()` 在
   `Start()` 之後 1ms 內就完成。

代價是開頭多丟一幀（~16.7ms 的畫面）。多音軌之間仍然只差 1.2ms 左右。

**預設不寫這個延遲。** 為什麼，見 §6.6 —— 那個「延遲」量錯了。

### 6.5 驗證：把「播放器眼裡的音畫差」變成可印出來的數字

結構驗證（`elst` 的 entry、`tfdt` 的 tick 數、`tkhd−mdhd`）只證明「寫進去了」，
不證明「補得對」。要證明後者，需要一個**內容上同時發生的事件**，在檔案裡量它兩邊
各落在哪裡。這套東西是三支程式湊起來的：

| 角色 | 程式 | 做什麼 |
|---|---|---|
| 素材源 | `anim_window --sync` | 整屏黑↔白反轉的同一瞬間播一個 3kHz 純音 |
| 錄製 | `CapCli --audio` | 正常錄，參數不同就是不同組 |
| 量測 | `mp4_check --sync` | 找亮度跳變與純音起點，相減，再跟容器宣告的延遲比 |

一鍵跑一組（**注意：這是舊的「不補償」語意，見 §6.6.1**）：

```bash
./out/sync_ab.sh P0 --progressive                 # progressive，不補償
./out/sync_ab.sh P1 --progressive --av-offset 60  # progressive，宣告 60ms
./out/sync_ab.sh F0                               # fMP4，不補償
./out/sync_ab.sh C2 --audio-delay 500 --av-offset 500
```

### 6.5.1 素材源必須是樣本鎖定的（2026-10 改寫）

上面的「素材源」原本用 `PlaySoundW(..., SND_ASYNC)`，那是**錯的**：呼叫回來只要
1~3ms，但嗶聲真的進到 loopback 串流要 **85~90ms**。量測端會把這 90ms 原封不動
算成「音訊偏晚」，於是整條驗證鏈的讀數都不可信 —— 詳見 §6.6.1。

現在 `anim_window --sync` 改成：

1. 用 `waveOut` 開一條**一直在播的串流**，內容是一個週期（`--interval`，預設 400ms）
   的樣本：開頭 150ms 是 3kHz 嗶聲，其餘靜音，無限循環。
2. 用 `waveOutGetPosition` 讀「已經播到第幾個樣點」。
3. **畫面反轉發生在播放位置到達嗶聲起點的那一刻** —— 不是用牆鐘排程。

關鍵在第 3 點：畫面的反轉時刻由「聲音播到哪裡」決定，所以裝置的輸出延遲同時作用
在聲音與畫面上，相減時自動抵消。**不需要知道延遲是多少。**

殘差只剩「`waveOutGetPosition` 的回報落後」與「DWM 合成延遲」的差，實測這台是
~36ms，用 `--flip-lead 36`（預設值）補掉。這是**素材源自己的**殘差，不是錄影端的
—— 錄影端只負責忠實記錄，不該補這個。

量到的殘差（各跑四次）：CLI **−2.7 ~ +3.6ms**、GUI **−6.0 ~ +3.9ms**，平均都 ≈ 0，
離散約 ±6ms。剩下的離散來自 `mp4_check` 的起點偵測與素材源的輪詢粒度，不是錄影端。

一鍵驗證（會自己判 PASS/FAIL，並檢查 CLI 與 GUI 兩條路是否一致）：

```bash
bash out/verify_sync.sh
```

輸出的判讀方式（`mp4_check --sync` 會直接印成這三行）：


```
量到的原始偏移 D = -2.5ms（負 = 音訊偏早）
容器宣告的延遲 E = +60.0ms  (elst empty edit，不在媒體時間裡)
=> 呈現時間差 = +57.5ms  音訊偏晚
```

- **progressive**：`elst` 不在媒體時間軸上（`SourceReader` 不套用編輯清單），
  所以呈現差 = `D + E`。
- **fMP4**：`tfdt` 就在媒體時間軸上，量到的 `D` 已經含了 `E`，所以呈現差 = `D`。
  這個不對稱是真的：同一個延遲，progressive 的 `media_ts[0]` 是 0.0ms，
  fMP4 的 `media_ts[0]` 是 60.0ms。

實測（2026-10-17，3060 Ti，`sync_*.mp4`）：

| 組 | 參數 | `E` | `D` | 呈現差 |
|---|---|---|---|---|
| P0 | progressive，不補償 | 0 | +10.1 | +10.1 |
| P1 | progressive，`--av-offset 60` | +60 | −2.5 | **+57.5** |
| F0 | fMP4，不補償 | 0 | +10.9 | +10.9 |
| F1 | fMP4，`--av-offset 60` | +60 | +65.0 | **+65.0** |
| C2 | `--audio-delay 500 --av-offset 500` | +500 | +15.5 | **+15.5** |
| D2 | `--audio-delay 500`，不補償 | 0 | −96.7 | −96.7（混疊，候選含 −496.7） |

最終 binary 重跑兩組（`sync_V0/V1.mp4`）：不補償 −1.1ms、`--av-offset 60` 時 +70.8ms，
與上表一致。不補償的實測範圍是 **−10.4 ~ +10.9ms**（六次），所以「對齊」的門檻訂在 10ms。

三個結論：

1. **`elst` 與 `tfdt` 兩條路都真的生效**：宣告 60ms，呈現差就多 57~65ms。
2. **補償機制可以抵消真實延遲**：C2 人為把音訊起跑推後 500ms，宣告 500ms 之後
   殘差只剩 +15.5ms（跟 P0/F0 的基準 10ms 同級）。
3. **預設的推導值是錯的**：P0/F0 不補償就是 10ms 等級，補 60ms 反而變成 57~65ms。

兩個量測上的坑：

- **混疊**：哔聲與反轉同週期 P 時，「偏移 d」與「偏移 d + P」在檔案裡完全一樣，
  所以 D 只能決定到 mod P。單次量測**無法**分辨是哪一個（殘差照樣很小，因為整串
  哔聲在 d 上自洽），所以工具直接把等價類印出來：

  ```
  => 呈現時間差 = -96.7ms  音訊偏早
    注意：D 只能決定到 mod 400ms，候選 = -96.7 / -896.7 / -496.7 / +303.3 / +703.3
  ```

  D2 就是這樣：真實 −500ms，在 ±200ms 的窗裡顯示 −96.7ms，候選裡有 −496.7。
  要量超過 ±P/2 的偏移就把 `FLIP` 拉長（`FLIP=2000` 直接量到 −497.5ms）。
- **`-v` 不能亂放**：舊版把第 4 個參數當成門檻，`--sync file -v` 會讓
  `atoi("-v")` 變成 0，門檻變 0 之後整段音訊只算一個 onset，看起來像「完全對齊」。
  現在 `--sync` 用 `--freq/--period/-v` 三個具名參數，不會再誤吃。

### 6.6 那個 60ms 是假的：量到的事件對齊才是真的

§6.3 的 60ms 一直被當成「音訊比影像晚開始」。用 §6.5 的方法量下去，發現
**不補償的檔案本來就是對齊的**：

| | 宣告 | 量到 |
|---|---|---|
| 不補償 | 0 | −1.3 ~ +10.9 ms |
| 補 60ms | 60 | +57 ~ +65 ms |

也就是說：**檔案本來對齊，補了 60ms 之後音訊晚了 60ms。** 那 60ms 不是偏移，
是「WGC 給的幀時間戳」與「loopback packet 的 QPC」各自帶著的管線延遲 ——
兩個時間戳相減，得到的是「兩條管線的延遲差」，不是「內容的時間差」。

試過兩條路都一樣錯：

| 算法 | 這台量到 | 真實 |
|---|---|---|
| `Qpc100ns() − 幀 pts`（`raw_delay`） | 58.4 ms | ≈ 0 |
| 第一個 loopback packet 的 QPC − 幀 pts（`qpc0`） | 72.1 ms | ≈ 0 |
| `raw_delay − 第一次 Drain 的 frame 數`（舊的 pre-roll 猜法） | 48 ~ 50 ms | ≈ 0 |

第二條路本來是「正確」的方向：`IAudioCaptureClient::GetBuffer` 的
`pu64QPCPosition` 是「這個 packet 第一個 frame」的時間戳，而我們送出去的 pts 也
正好是從 0 開始累加 frame 數，兩者對的是同一個 frame。但實測它仍然偏高 72ms ——
所以「影像零點那一幀的 pts」本身也不是內容時間（它比畫面真的變了要晚）。

**結論：這個偏移在目前的資料下推導不出來，只能量。** 因此 `CapCli` 的預設行為
改成**不補償**，並提供三個明確的出口：

```
--av-offset <ms>    指定一個值（= 校準檔裡的那個數字）
--av-offset auto    讀校準檔 %LOCALAPPDATA%\CapWGC\av-offset.txt
--no-av-offset      等於 --av-offset 0
```

`SetAudioStartOffset` 的機制留著：§6.5 的 C2 已經證明它能把真實的 500ms 延遲
抵掉。缺的只是一個可信的輸入值 —— 那個值由 §6.7 的校準流程產生。

### 6.6.1 追記（2026-10）：上面那個結論是「兩個誤差互相抵掉」的產物

§6.6 說「不補償本來就是對齊的」，**這個結論是錯的**。錯在量測工具，不在資料。

`anim_window --sync` 當時用
`PlaySoundW(wav, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)` 播嗶聲。
PlaySound **呼叫**回來只要 1~3ms（所以當初判斷「啟動延遲 1~3ms」），但從呼叫到
嗶聲真的出現在 loopback 串流裡是 **85~90ms**（每次都要重開裝置）。

於是量測端看到的「音畫差」其實是

```
呈現差 = δ(素材源的 PlaySound 延遲) − gap
gap    = firstPacketQpc − videoOrigin   （= 音軌比影像零點晚起跑多少）
```

而 CLI 那條路的 `gap` 剛好是 86~89ms —— 和 δ 幾乎相等，於是 `呈現差 ≈ 0`。
**一個真的偏了 90ms 的檔案，被量成「對齊」。**

真正的 bug 在 `Recorder`／`CapCli` 兩邊都有：音軌第一筆樣本的內容時刻是
`videoOrigin + gap`，但 muxer 一律把它放在 `dts = 0`，所以整條音軌早了 `gap`。
CLI 的 `gap` 大（編碼器初始化慢），GUI 的 `gap` 小（`startDelay` 只有 1~6ms），
所以只有 GUI 露出來 —— 使用者看到的就是「嘴型對不上」。

| 路徑 | gap | 舊行為量到 | 真實 |
|---|---|---|---|
| CLI | 86.7 ms | +5.8 ms（看起來對齊） | 音訊早 87 ms |
| GUI | 17.1 ms | +67.5 ms | 音訊早 17 ms |

兩者的差（62ms）正好等於兩者 `gap` 的差（69.6ms）。這就是「同一個 bug、兩條路
量出不同答案」的原因。

**修法**（`recorder.cpp` 的 `applyAudioOffset`、`cli/main.cpp` 的 `applyOffset`）：
把 `gap` 直接寫進容器，`SetAudioStartOffset(track, +gap)`。`gap > 0` 走 empty
edit／tfdt 後推，正是「音訊本來偏早」那一格。

**驗證**（`out/verify_sync.sh`，素材源已改成樣本鎖定，見 §6.5.1）：

```
CLI  gap=+99.5ms  ->  -2.7 ~ +3.6ms（四次）  PASS
GUI  gap=+10.2ms  ->  -6.0 ~ +3.9ms（四次）  PASS
一致性（兩條路差）  ->   1.3 ~ 9.6ms          PASS
```

修正前同一個素材是 CLI +5.8ms、GUI +67.5ms —— 差 62ms，正好是兩者 `gap` 的差。

大 gap 的極端情況（`--audio-delay 250` 模擬慢機器，素材週期拉長到 1000ms 以免
mod 400 混疊）：

| | gap | 量到 |
|---|---|---|
| `--no-av-offset`（舊行為） | 342.3 ms | **−345.3 ms**（音訊早 345ms，肉眼可見） |
| 預設（自動補 gap） | 347.3 ms | **−5.4 ms**（對齊） |

所以 `--no-av-offset` 現在的意義是「純原始時間軸，A/B 對照用」，不再是預設。


### 6.7 負偏移：elst 推不了，要裁；以及校準檔

#### 符號約定

一條軌的位移 `N` 的定義是「把這條軌在**呈現時間軸**上移動 N」：

| N | 意思 | 什麼時候用 | progressive | fMP4 |
|---|---|---|---|---|
| `N > 0` | 往後推 | 音訊本來**偏早** | `elst` 一格 empty edit（`media_time = -1`） | 第一個 `tfdt` 從 N 起算 |
| `N < 0` | 往前拉 = 裁掉開頭 | 音訊本來**偏晚** | `elst` 一格 `media_time = |N|`，**資料不丟** | 開頭整塊 AU 丟掉 |
| `N = 0` | 不動 | 已對齊 | 不寫 `edts` | `tfdt` 從 0 起算 |

「要寫的 N」= 負的「量到的 D」。§6.5 的工具印 D（正 = 音訊偏晚），所以
校準檔寫的是 `−D`。

#### 為什麼負偏移不能也寫 elst

`elst` 的 empty edit 是一段「不對應任何 sample 的時間軸」—— 它插在媒體**前面**，
所以只能把媒體往後推。要把媒體往前拉，語意上等於「從媒體的第 x 秒開始播」，
那是 `media_time > 0`（不是 `-1`）。兩個單位不一樣，很容易寫錯：

- `segment_duration` 用 **movie** timescale（這裡是 1000）
- `media_time` 用 **media** timescale（音軌 = 取樣率，44.1k）

fMP4 沒有 `edts` 這個地方，所以負位移只能真的丟掉開頭的 AU。

#### 精度不對稱（這是選容器的實際代價）

| | 表達方式 | 精度 | 資料 |
|---|---|---|---|
| progressive，`N > 0` | `elst` empty edit | 1 ms（movie ts） | 完整 |
| progressive，`N < 0` | `elst` `media_time` | 1 tick = 1/44100 s | 完整 |
| fMP4，`N > 0` | 第一個 `tfdt` | 1 tick | 完整 |
| fMP4，`N < 0` | 丟 AU | **一個 AU = 1024 tick ≈ 23.2ms** | 開頭丟掉 |

fMP4 的負位移取整方向是 **floor**（只丟「整塊都落在裁切區內」的 AU），不是
ceil。用 ceil 會多丟一個 AU，而「多丟」的方向是危險的：真實偏移只有 9ms 時，
ceil 丟掉 23ms，補償後變成 −14ms —— 比不補償（+9ms）離對齊**更遠**。floor 在
同樣情況下丟 0 個，等於退回不補償，方向是安全的。

所以 fMP4 表達不了 |N| < 23.2ms 的負補償 —— 會量化成 0。`calibrate.sh` 對這種
情況會明講，不會讓校準檔看起來「有效」。

#### 實測（3060 Ti / tokyo）

`FLIP=2000`（把混疊模糊度放大到 ±1000ms，排除候選歧義）：

| 組 | 容器 | `--av-offset` | 量到的 D | 宣告的 E | 呈現差 |
|---|---|---|---|---|---|
| `sync_w0` | fMP4 | 0 | +5.0 | 0（tfdt） | **+5.0 對齊** |
| `sync_wN150` | fMP4 | −150 | −136.6 | 0（tfdt，已含在媒體時間） | **−136.6** |
| `sync_wF250` | fMP4 | +250 | +252.8 | +250.0（tfdt） | **+252.8** |
| `sync_wP0` | progressive | 0 | +8.9 | 0（無 `edts`） | **+8.9 對齊** |
| `sync_wPN150` | progressive | −150 | −1.0 | −150.0（`elst media_time`） | **−151.0** |
| `sync_wP250` | progressive | +250 | −2.1 | +250.0（`elst` empty edit） | **+247.9** |

四種組合（正／負 × progressive／fMP4）都對得上，而且正負兩種的**公式不同**這件
事在表裡看得很清楚：fMP4 的 `+250` 量到 `+252.8`（E 已含在 D 裡，不能加），
fMP4 的 `−150` 量到 `−136.6`（E 是 0，往前拉全在 D 裡）；progressive 兩個方向
都是 `D + E`（`−1.0 + (−150) = −151.0`、`−2.1 + 250 = +247.9`）。

`sync_wPN150` 的 `elst` 實際內容（直接 dump）：

```
trak  id=2   mdhd timescale=44100 dur=351232
  edts
    elst  ver=0 entries=1
      #0 segDur=7814  media_time=6615     ← 6615/44100 = 150.0ms
```

這六組把兩條路都驗完了：

- **fMP4，負位移**：丟 6 個 AU = 139.3ms，`5.0 − 139.3 = −134.3`，量到 −136.6
  （差 2.3ms，就是 AU 量化的殘差）。
- **fMP4，正位移**：`tfdt` 從 250ms 起算，量到 +252.8 —— 與不補償的 +5.0 相比
  多了 247.8ms，吻合指示值。
- **progressive，負位移**：`elst` 精確到 tick，`8.9 − 150 = −141.1`……但量到 −1.0。
  差的 150 正是 `E`，因為 progressive 的 `elst` **不在媒體時間軸上**，
  `SourceReader` 不套用它。所以呈現差 = D + E = −151.0。
- **progressive，正位移**：`D + E = −2.1 + 250 = +247.9`，同樣吻合。

這也解釋了為什麼量測端要分兩種公式：`fMP4` 的位移若為正，已經含在 `tfdt`
裡（不能再加一次）；但 fMP4 的**負**位移是「直接丟 AU」，丟完 `tfdt` 仍然是 0，
所以「已經往前拉了」完全反映在 D 裡 —— 再加一次負的 E 就是減兩次。

#### 校準檔：換硬體不會有問題的實際做法

「換不同等級的硬體，偏移會一樣嗎」—— 不會，而且這正是問題所在：

- 這個偏移是**每台機器的管線延遲差**，不是可移植常數。GPU（WGC 走的是
  DXGI 桌面複製的顯示管線）、音效驅動（WASAPI 引擎的緩衝深度）、Windows
  版本都會影響它。
- 時間戳**推導**不出來（§6.6 三條路都是偽影），所以不能靠「公式 + 硬體參數」
  在執行期算 —— 只能量。
- 但「要重新量」不等於「換硬體就會出問題」。把量到的值存起來，之後直接用；
  換硬體重跑一次校準（約 30 秒）即可。這就是 `out/calibrate.sh` 的用途。

```
./out/calibrate.sh            # 量兩輪 → 解混疊 → 寫校準檔 → 立刻驗證
./out/calibrate.sh --dry-run  # 只量不寫
```

流程刻意做成兩輪，因為單次量測有**混疊**（§6.5）：哔聲與畫面反轉同週期，所以
「偏移 d」與「偏移 d + P」在檔案裡完全一樣。

| 輪 | 週期 | 事件數 / 8 秒 | 能決定 | 不能決定 |
|---|---|---|---|---|
| 大範圍 | 2000ms | ~4 | 幾百 ms 的哪一檔 | 幾十 ms |
| 精修 | 400ms | ~20 | 幾 ms | mod 400ms |

拿大範圍去選精修的候選（`FINE + 400k` 中最接近 `WIDE` 的那個），兩輪就互補了。
兩輪差距 > 60ms 就拒絕寫檔並提示先殺素材源 —— 一個壞的校準檔比沒有更糟，因為
它會**靜默**歪掉之後每一份錄影。

校準腳本還有兩個「寧可不寫」的守門條件：

1. **|D| ≤ 10ms**：寫 0。這個幅度看影片分不出來，硬補只是把誤差搬到另一邊，
   而且不同播放器對 `elst` 的處理不一致。
2. **需要負補償且 |N| < 一個 AU**：寫檔但明講 fMP4 無效（見上面的量化）。
   progressive 仍然有效。

實跑（本機）：`D = +6.3ms` → 落在門檻內 → 寫 0。所以這台機器的正確設定就是
**不補償**，與 §6.6 的結論一致。

#### 副作用：把診斷值保留符號

`main.cpp` 裡那個「推導值」（`raw_delay` / `qpc0`）原本被夾成 `> 0 ? raw : 0`，
負值會被吞掉。那兩個值已知不是音畫差，但**音訊偏早的機器**會在那裡顯示負值 ——
夾成 0 等於把「這台需要負補償」的唯一線索蓋掉。現在保留原值。

---

## 7. 還沒做的

- **Opus**。MF 沒有通用的 Opus 編碼器，要走 libopus 就會碰到授權（跟「不能用
  FFmpeg」是同一條線）。
- **每條軌獨立碼率**。`--audio-bitrate` 目前對所有來源生效；`--mic-bitrate` 之類
  的細分沒有做。
- **裝置熱插拔**。錄到一半拔掉麥克風，那條軌就停在原地（不會崩，但也不會恢復）。
- **192k 取樣率的裝置**沒有上限檢查，照開。
- **音量正規化 / 混音成單軌**。現在是「分開存」，播放器要自己混。想輸出「已經混好
  的單軌」需要一條軟體混音路徑，那是另一個功能。
- **A/V 偏移的值只能量、不能推導**。目前用一個「3kHz 純音對整屏反轉」的外部素材源
  才量得出來（§6.5），再靠 `out/calibrate.sh` 存成校準檔（§6.7）。產品要自動校準
  的話，得在錄影開始時偷偷播一段聽不見的測試音 —— 這會干擾 loopback 錄到的內容，
  不能做。所以現階段就是「預設不補償 + 校準檔 + 可指定」。
  （負偏移已經做了，見 §6.7；剩下的純粹是「怎麼在產品裡自動量」的問題。）
- **fMP4 的小幅負位移**。丟 AU 的量化下限是 23.2ms，比這小的負補償表達不了。
  要解決得在 PCM 層裁 —— 但偏移要等第一個 AU 產生才算得出來（雞生蛋）。
  實務上這個幅度本來就在對齊門檻內，所以擱著。
- **AAC 編碼器自身的 priming**。`elst` 補的是「串流什麼時候開始」，不含 AAC 的
  編碼延遲（1024~2048 個取樣點，約 23~46ms）。要補得靠 `elst` 的 `media_time`
  去裁掉開頭（機制跟 §6.7 的負偏移是同一套，只差「量到的值要不要含 priming」），
  或寫 `sgpd/sbgp` 的 roll 資訊 —— 那是另一件事，先不動。

