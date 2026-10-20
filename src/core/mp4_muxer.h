// ============================================================================
// mp4_muxer.h — 自寫 MP4 muxer
// ============================================================================
//
// 為什麼不用 MF 的 SinkWriter 寫檔：它把 muxer 和 encoder 綁在一起，而我們要
// 的是「自己驅動 encoder」，所以容器也得自己寫。附帶好處是 stts/stss/stsz 這些
// 表完全由我們控制，VFR 的時間戳不會被 SinkWriter 的 CFR 假設吃掉。
//
// 支援：
//   - 單一視訊軌，H.264（avc1 + avcC）、HEVC（hvc1 + hvcC）、AV1（av01 + av1C）
//   - 0..N 條音軌（AAC，mp4a + esds），每條一個 trak；fMP4 時每個 moof 一個 traf
//   - H.264/HEVC：Annex-B 輸入 → AVCC（4-byte 長度前綴）寫入 mdat
//   - AV1：輸入本身就是「帶 obu_size 的 OBU 串」，原樣寫入；參數集是
//     sequence header OBU，解析後放進 av1C 的 configOBUs
//   - VFR：每個 sample 自帶 duration；pts 單調時 dts == pts，不寫 ctts
//   - pts 非單調（B 幀）時用 rank 法推 dts 並寫 ctts
//   - 兩種容器佈局：
//       progressive（預設）  ftyp + mdat + moov      ← moov 在檔尾
//       fragmented           ftyp + moov + (moof+mdat)*  ← 每個 GOP 一片
//
// 為什麼要有 fragmented 模式：progressive 的索引在檔尾，行程被 kill（工作管理
// 員、斷電、當機）時 moov 根本沒寫，整段錄影直接變成不可播的二進位垃圾 ——
// 這是錄屏軟體最常見的資料遺失情境，也是「錄了三小時結果全沒」的成因。
//
// fragmented 模式下每個 moof 都自帶完整的取樣表，所以「寫到哪裡就能播到哪裡」，
// 最壞情況只損失最後一個未寫完的分片（本專案的分片邊界對齊 GOP，預設 2 秒）。
//
// 代價：moof 每個分片多 ~100 bytes 的標頭，以及有些老播放器對 fMP4 支援較差。
// 對「錄壞掉」這個風險來說，這個代價明顯划算。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "encoder.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace capwgc {

class Mp4Muxer {
public:
    Mp4Muxer();
    ~Mp4Muxer();
    Mp4Muxer(const Mp4Muxer&) = delete;
    Mp4Muxer& operator=(const Mp4Muxer&) = delete;

    // configRecord：avcC / hvcC 的內容（不含 box header）。
    // 傳空的話會在遇到第一個關鍵幀時，從 in-band 的 VPS/SPS/PPS 自己組。
    //
    // fragmented = true 時走 fMP4 佈局（moov 在前、每個 GOP 一片 moof+mdat），
    // 被 kill 也還能播。代價是每個分片多一點標頭。
    //
    // audioTracks 非空時每條多開一個 trak（track_ID = 2 + index，timescale = 取樣率）。
    // 必須在寫入任何 sample 之前呼叫 —— 音軌是 moov 的一部分，檔案開好之後就加不了。
    bool Open(const std::wstring& path, Codec codec, int width, int height,
              const std::vector<uint8_t>& configRecord, std::string* err,
              bool fragmented = false,
              const std::vector<AudioTrackConfig>* audioTracks = nullptr);

    // annexb 是一個 access unit（decode order）。pts/dur 單位 100ns。
    // AV1 時傳的是原始 OBU 串（不是 Annex-B）。
    // keyframe 為 false 且 config 還沒建立時，這個 sample 會被丟棄並回報錯誤
    // —— 沒有 SPS/PPS 的 mdat 開頭在播放器裡等於壞檔。
    bool WriteSample(const uint8_t* annexb, size_t size, int64_t pts100ns,
                     int64_t dur100ns, bool keyframe, std::string* err);

    // 音訊 access unit（AAC raw，不含 ADTS 標頭）寫進第 track 條音軌（0 起算）。
    //
    // frames 是這一個 AU 包含的取樣點數（AAC-LC 固定 1024）。時間軸由 muxer 自己
    // 用取樣點累加 —— 音軌的 timescale 就是取樣率，所以 delta 恆等於 frames，
    // stts 只有一個 run。用 pts100ns 換算會讓 delta 在 1023/1025 之間跳，stts
    // 退化成「每個 sample 一個 run」。
    // pts100ns 只用來決定音軌的起點（第一筆的偏移），通常傳 0。
    //
    // 不同 track 各自累加自己的時間軸，互不干擾。
    bool WriteAudioSample(int track, const uint8_t* data, size_t size, uint32_t frames,
                          int64_t pts100ns, std::string* err);

    // 設定某條音軌的起始延遲（100ns）。必須在第一筆音訊 sample 之前呼叫。
    //
    // 為什麼要事後設定而不是 Open 時給：這個值只能在錄影真的開始之後才量得到
    // （音訊串流的 Start 發生在第一幀之後），而 Open 早在第一幀就發生了。
    // 容器還沒寫 moov，所以這時候改還来得及。
    bool SetAudioStartOffset(int track, int64_t offset100ns, std::string* err);

    // 寫 moov 並關檔。沒有 sample 時仍然會寫一個合法的空檔。
    bool Close(std::string* err);

    uint64_t bytes() const;        // 目前檔案大小（含已寫入的 mdat）
    uint64_t samples() const;
    uint64_t payloadBytes() const; // 送進 mdat 的位元組數
    uint64_t audioSamples() const; // 所有音軌的 AU 總數
    int audioTrackCount() const;
    bool hasAudio() const;
    bool hasConfig() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capwgc
