// ============================================================================
// mp4_muxer.cpp
// ============================================================================
//
// 這個檔案只做一件事：把「Annex-B 的 access unit + 真實時間戳」變成一個
// 播放器吃得下的 MP4。它不懂編碼，也不碰 D3D。
//
// 幾個刻意的選擇：
//
// * timescale = 90000，不是 10^7。
//   100ns 直接當 timescale 在數學上最乾淨，但 10^7 的 timescale 在部分老播放器
//   與硬解路徑上會被當成異常值。90000 的量化誤差是 11µs，遠小於一幀的抖動，
//   換來的是「任何播放器都認得」。
//
// * 每個 sample 一個 chunk（stsc 只有一筆）。
//   多樣本合併成一個 chunk 可以省 stco 的 4 bytes/sample，但會讓之後要插入音軌
//   變成「重排整個 mdat」。單樣本 chunk 的額外成本在 1080p60 下是 0.3%，
//   而它讓「之後加音軌」只是多一個 trak 的事。
//
// * keyframe 不信任呼叫端。
//   呼叫端給的 flag 與 NAL 掃描的結果取 OR：IDR 就是 sync sample，不管 MF 的
//   MFSampleExtension_CleanPoint 有沒有設。漏一個 sync sample 的代價是
//   seek 到那裡花屏，而多標一個的代價只是多一個可 seek 點。
// ============================================================================

#include "mp4_muxer.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace capwgc {

namespace {

// ---------------------------------------------------------------- byte helpers

using Bytes = std::vector<uint8_t>;

void U8(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v & 0xFF)); }
void U16(Bytes& b, uint32_t v) { U8(b, v >> 8); U8(b, v & 0xFF); }
void U24(Bytes& b, uint32_t v) { U8(b, v >> 16); U8(b, (v >> 8) & 0xFF); U8(b, v & 0xFF); }
void U32(Bytes& b, uint32_t v) { U16(b, v >> 16); U16(b, v & 0xFFFF); }
void U64(Bytes& b, uint64_t v) { U32(b, (uint32_t)(v >> 32)); U32(b, (uint32_t)(v & 0xFFFFFFFF)); }
void FourCC(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }
void Zeros(Bytes& b, size_t n) { b.insert(b.end(), n, 0); }
void Raw(Bytes& b, const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }

void Append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }

Bytes Box(const char* type, const Bytes& payload) {
    Bytes out;
    U32(out, (uint32_t)(8 + payload.size()));
    FourCC(out, type);
    Append(out, payload);
    return out;
}

Bytes FullBox(const char* type, uint8_t ver, uint32_t flags, const Bytes& payload) {
    Bytes p;
    U8(p, ver);
    U24(p, flags);
    Append(p, payload);
    return Box(type, p);
}

Bytes Container(const char* type, std::initializer_list<Bytes> children) {
    Bytes payload;
    for (const auto& c : children) Append(payload, c);
    return Box(type, payload);
}

// 同上的 vector 版：音軌數量是執行期決定的（1 + N 條），initializer_list 不夠用。
Bytes ContainerV(const char* type, const std::vector<Bytes>& children) {
    Bytes payload;
    for (const auto& c : children) Append(payload, c);
    return Box(type, payload);
}

// MPEG-4 descriptor（esds 用的那種）。長度一律寫成 4 bytes 的 expandable 形式：
// 合法、定長、不需要回填 —— 短形式在巢狀時很容易算錯內層長度。
void Desc(Bytes& b, uint8_t tag, const Bytes& payload) {
    b.push_back(tag);
    const size_t n = payload.size();
    b.push_back((uint8_t)(0x80 | ((n >> 21) & 0x7F)));
    b.push_back((uint8_t)(0x80 | ((n >> 14) & 0x7F)));
    b.push_back((uint8_t)(0x80 | ((n >> 7) & 0x7F)));
    b.push_back((uint8_t)(n & 0x7F));
    Raw(b, payload.data(), payload.size());
}

// 100ns → timescale 單位，四捨五入。
constexpr int64_t kTimescale = 90000;
int64_t ToTs(int64_t t100) {
    return (t100 * kTimescale + 5000000) / 10000000;
}

// ------------------------------------------------------------------ Annex-B

struct Nal {
    const uint8_t* p = nullptr;
    size_t n = 0;
    int type = 0;
};

// 掃 start code。回傳的 NAL 不含 start code，也不含結尾的 trailing zero。
bool SplitAnnexB(const uint8_t* d, size_t len, bool hevc, std::vector<Nal>& out) {
    out.clear();
    size_t i = 0;
    // 找第一個 start code
    auto findStart = [&](size_t from, size_t* scLen) -> size_t {
        for (size_t k = from; k + 2 < len; ++k) {
            if (d[k] == 0 && d[k + 1] == 0) {
                if (d[k + 2] == 1) { *scLen = 3; return k; }
                if (k + 3 < len && d[k + 2] == 0 && d[k + 3] == 1) { *scLen = 4; return k; }
            }
        }
        return SIZE_MAX;
    };

    size_t scLen = 0;
    i = findStart(0, &scLen);
    if (i == SIZE_MAX) return false;
    i += scLen;

    while (i < len) {
        size_t nextSc = 0;
        const size_t j = findStart(i, &nextSc);
        size_t end = (j == SIZE_MAX) ? len : j;
        // 去掉結尾的 0（trailing_zero_8bits）
        while (end > i && d[end - 1] == 0) --end;
        if (end > i) {
            Nal n;
            n.p = d + i;
            n.n = end - i;
            n.type = hevc ? ((n.p[0] >> 1) & 0x3F) : (n.p[0] & 0x1F);
            out.push_back(n);
        }
        if (j == SIZE_MAX) break;
        i = j + nextSc;
    }
    return !out.empty();
}

bool IsKeyNal(int type, bool hevc) {
    if (hevc) return type >= 16 && type <= 21;   // BLA / IDR / CRA
    return type == 5;                            // IDR
}

// Annex-B → AVCC（每個 NAL 前面換成 4-byte big-endian 長度）
void ToAvcc(const std::vector<Nal>& nals, Bytes& out) {
    out.clear();
    for (const auto& n : nals) {
        U32(out, (uint32_t)n.n);
        Raw(out, n.p, n.n);
    }
}

// ------------------------------------------------------- config record builders

// avcC：從 in-band 的 SPS/PPS 組。MF 有提供 blob 時不會走到這裡。
bool BuildAvcC(const std::vector<Nal>& nals, Bytes& out) {
    const Nal* sps = nullptr;
    const Nal* pps = nullptr;
    for (const auto& n : nals) {
        if (n.type == 7 && !sps) sps = &n;
        else if (n.type == 8 && !pps) pps = &n;
    }
    if (!sps || !pps || sps->n < 4) return false;

    out.clear();
    U8(out, 1);              // configurationVersion
    U8(out, sps->p[1]);      // AVCProfileIndication
    U8(out, sps->p[2]);      // profile_compatibility
    U8(out, sps->p[3]);      // AVCLevelIndication
    U8(out, 0xFF);           // reserved(6) + lengthSizeMinusOne(2)=3
    U8(out, 0xE1);           // reserved(3) + numOfSequenceParameterSets(5)=1
    U16(out, (uint32_t)sps->n);
    Raw(out, sps->p, sps->n);
    U8(out, 1);              // numOfPictureParameterSets
    U16(out, (uint32_t)pps->n);
    Raw(out, pps->p, pps->n);
    return true;
}

// hvcC：從 VPS/SPS/PPS 組。
//
// HEVC 的 NAL header 是 **2 bytes**（forbidden_zero_bit 1 + nal_unit_type 6 +
// nuh_layer_id 6 + nuh_temporal_id_plus1 3），H.264 只有 1 byte。所以 SPS 的
// RBSP 從 p[2] 開始，而 profile_tier_level 又在 RBSP 的第一個 byte 之後
// （vps_id 4 + max_sub_layers_minus1 3 + temporal_id_nesting 1 = 剛好一個 byte）
// —— 也就是 p[3]。從 p[1] 取會拿到 nuh_temporal_id_plus1 當 profile_idc、
// 拿 constraint flags 當 level，而且不會有任何錯誤訊息，只是一個解不開的檔。
//
// 硬體編碼器的 SPS 一定是 4:2:0 8-bit，這讓後面幾個欄位可以寫死。
bool BuildHvcC(const std::vector<Nal>& nals, Bytes& out) {
    const Nal* vps = nullptr;
    const Nal* sps = nullptr;
    const Nal* pps = nullptr;
    for (const auto& n : nals) {
        if (n.type == 32 && !vps) vps = &n;
        else if (n.type == 33 && !sps) sps = &n;
        else if (n.type == 34 && !pps) pps = &n;
    }
    if (!sps || !pps || sps->n < 15) return false;

    out.clear();
    U8(out, 1);
    Raw(out, sps->p + 3, 12);        // profile_space/tier/idc + compat + constraints + level
    U16(out, 0xF000);                // reserved(4) + min_spatial_segmentation_idc(12)
    U8(out, 0xFC);                   // reserved(6) + parallelismType(2)=0
    U8(out, 0xFD);                   // reserved(6) + chroma_format_idc(2)=1 (4:2:0)
    U8(out, 0xF8);                   // reserved(5) + bit_depth_luma_minus8(3)=0
    U8(out, 0xF8);                   // reserved(5) + bit_depth_chroma_minus8(3)=0
    U16(out, 0);                     // avgFrameRate
    U8(out, 0x0F);                   // constFrameRate(2)=0, numTemporalLayers(3)=1, nested=1, lenSize=3
    U8(out, (vps ? 3 : 2));          // numOfArrays
    auto array = [&](int type, const Nal* n) {
        if (!n) return;
        U8(out, 0x80 | (uint8_t)type);   // array_completeness=1 + reserved + NAL_unit_type
        U16(out, 1);
        U16(out, (uint32_t)n->n);
        Raw(out, n->p, n->n);
    };
    array(32, vps);
    array(33, sps);
    array(34, pps);
    return true;
}

// ---------------------------------------------------------------- AV1 (OBU)
//
// AV1 在容器層跟 H.264/HEVC 是兩件不同的事，不能塞進上面那條 Annex-B 管線：
//
//   - 沒有起始碼、沒有 NAL。一個 access unit 就是「一串 OBU」，每個 OBU 自帶
//     obu_size（leb128）。ISOBMFF 要求樣本裡的 OBU **必須**有 size 欄位，
//     所以 mdat 裡放的就是原樣的位元流，一個 byte 都不用改。
//   - 參數集不是 VPS/SPS/PPS，而是單一個 sequence header OBU，寫進 av1C 的
//     configOBUs。它的內容是位元流，要用 bit reader 解 profile/level/色度取樣。
//   - sample entry 是 'av01'，設定放在子 box 'av1C' 裡（不是像 hvcC 那樣直接
//     塞在 sample entry 的欄位中）。
//
// 這幾件事湊起來的結果是：AV1 的 muxer 路徑其實比 HEVC 短 —— 不需要拆 NAL、
// 不需要重寫長度前綴、不需要從 in-band 參數集組 config record。

struct ObuView {
    const uint8_t* p = nullptr;      // 整個 OBU，含 header 與 obu_size
    size_t n = 0;
    int type = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
};

// OBU 的 type 定義（AV1 spec 6.2.2）
constexpr int kObuSeqHeader   = 1;
constexpr int kObuFrameHdr    = 3;
constexpr int kObuFrame       = 6;

bool ReadLeb128(const uint8_t* p, size_t n, size_t& off, size_t& val) {
    val = 0;
    for (int i = 0; i < 8; ++i) {
        if (off >= n) return false;
        const uint8_t b = p[off++];
        val |= (size_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return true;
    }
    return false;
}

bool NextObu(const uint8_t* p, size_t n, size_t& off, ObuView& o) {
    if (off >= n) return false;
    const size_t start = off;
    const uint8_t h = p[off++];
    if (h & 0x80) return false;                  // forbidden bit 必須是 0
    o.type = (h >> 3) & 0x0F;
    const bool hasExt = (h & 0x04) != 0;
    const bool hasSize = (h & 0x02) != 0;
    if (hasExt) {
        if (off >= n) return false;
        ++off;                                   // temporal/spatial id + reserved
    }
    // 沒有 obu_size 的 OBU 在 ISOBMFF 樣本裡是非法的，也沒辦法逐個走過。
    if (!hasSize) return false;
    size_t sz = 0;
    if (!ReadLeb128(p, n, off, sz)) return false;
    if (off + sz > n) return false;
    o.p = p + start;
    o.payload = p + off;
    o.payloadLen = sz;
    o.n = (off + sz) - start;
    off += sz;
    return true;
}

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t bit = 0;
    bool ok = true;

    uint32_t Read(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; ++i) {
            const size_t byteIdx = bit >> 3;
            if (byteIdx >= n) { ok = false; return v; }
            v = (v << 1) | ((p[byteIdx] >> (7 - (bit & 7))) & 1);
            ++bit;
        }
        return v;
    }
    void Skip(size_t k) {
        for (size_t i = 0; i < k && ok; ++i) Read(1);
    }
    uint32_t Uvlc() {
        int leading = 0;
        while (ok && Read(1) == 0) ++leading;
        if (!ok) return 0;
        return (uint32_t)((1ull << leading) - 1 + (leading ? Read(leading) : 0));
    }
};

// av1C 需要的欄位，全部來自 sequence header。色度取樣那幾個欄位不是「額外
// 資訊」—— 標錯會讓播放器用錯的 chroma 位置去上採樣，畫面邊緣出現彩邊。
struct Av1SeqInfo {
    int profile = 0;
    int levelIdx = 0;
    int tier = 0;
    int highBitdepth = 0;
    int twelveBit = 0;
    int mono = 0;
    int subX = 1;
    int subY = 1;
    int chromaPos = 0;
    int width = 0;
    int height = 0;
};

bool ParseAv1SeqHeader(const uint8_t* p, size_t n, Av1SeqInfo& si) {
    BitReader br{p, n};
    si.profile = (int)br.Read(3);
    br.Skip(1);                                  // still_picture
    const bool reduced = br.Read(1) != 0;        // reduced_still_picture_header

    uint32_t level = 0, tier = 0;
    bool decoderModelInfo = false;
    bool initialDisplayDelay = false;
    size_t bufferDelayLen = 0;

    if (reduced) {
        br.Skip(5);                              // operating_points_cnt_minus_1 (==0)
        br.Skip(12);                             // operating_point_idc[0]
        level = br.Read(5);
        if (level > 7) tier = br.Read(1);
    } else {
        // 注意：decoder_model_info_present_flag 只在 timing_info_present_flag 為 1
        // 時才**存在**。無條件讀它會讓後面每一個欄位都偏一個 bit —— 症狀是
        // level_idx 解出一個不存在的等級（實測 18 / high tier），而且
        // frame_width_bits 之後全部錯位。整份檔案照樣能播（decoder 讀的是
        // sequence header 本身，不是 av1C 的這些欄位），所以這個 bug 不會自己
        // 浮出來，只會在嚴格解析器或跨工具時咬人。
        if (br.Read(1)) {                        // timing_info_present_flag
            br.Skip(32);                         // num_units_in_display_tick
            br.Skip(32);                         // time_scale
            if (br.Read(1)) br.Uvlc();           // num_ticks_per_picture_minus_1
            decoderModelInfo = br.Read(1) != 0;
            if (decoderModelInfo) {
                bufferDelayLen = br.Read(5) + 1;
                br.Skip(32);                     // num_units_in_decoding_tick
                br.Skip(5);                      // buffer_removal_time_length_minus_1
                br.Skip(5);                      // frame_presentation_time_length_minus_1
            }
        }
        initialDisplayDelay = br.Read(1) != 0;
        const uint32_t ops = br.Read(5);
        for (uint32_t i = 0; i <= ops; ++i) {
            br.Skip(12);                         // operating_point_idc
            const uint32_t lv = br.Read(5);
            uint32_t tr = 0;
            if (lv > 7) tr = br.Read(1);
            if (i == 0) { level = lv; tier = tr; }
            if (decoderModelInfo) {
                if (br.Read(1)) {                // decoder_model_present_for_this_op
                    br.Skip(bufferDelayLen);     // decoder_buffer_delay
                    br.Skip(bufferDelayLen);     // encoder_buffer_delay
                    br.Skip(1);                  // low_delay_mode_flag
                }
            }
            if (initialDisplayDelay) {
                if (br.Read(1)) br.Skip(4);      // initial_display_delay_minus_1
            }
        }
    }
    if (!br.ok) return false;

    const uint32_t wBits = br.Read(4) + 1;
    const uint32_t hBits = br.Read(4) + 1;
    const uint32_t maxW = br.Read((int)wBits) + 1;
    const uint32_t maxH = br.Read((int)hBits) + 1;

    if (!reduced) {
        if (br.Read(1)) br.Skip(7);              // frame_id_numbers_present_flag
    }
    br.Skip(1);                                  // use_128x128_superblock
    br.Skip(1);                                  // enable_filter_intra
    br.Skip(1);                                  // enable_intra_edge_filter
    if (!reduced) {
        br.Skip(1);                              // enable_interintra_compound
        br.Skip(1);                              // enable_masked_compound
        br.Skip(1);                              // enable_warped_motion
        br.Skip(1);                              // enable_dual_filter
        const bool orderHint = br.Read(1) != 0;  // enable_order_hint
        if (orderHint) { br.Skip(1); br.Skip(1); }   // jnt_comp, ref_frame_mvs
        const bool chooseScreen = br.Read(1) != 0;
        uint32_t forceScreen = 2;
        if (!chooseScreen) forceScreen = br.Read(1);
        if (forceScreen > 0) {
            const bool chooseIntMv = br.Read(1) != 0;
            if (!chooseIntMv) br.Read(1);
        }
        if (orderHint) br.Skip(3);               // order_hint_bits_minus_1
    }
    br.Skip(1);                                  // enable_superres
    br.Skip(1);                                  // enable_cdef
    br.Skip(1);                                  // enable_restoration

    // ---- color_config()
    si.highBitdepth = (int)br.Read(1);
    if (si.profile == 2 && si.highBitdepth) si.twelveBit = (int)br.Read(1);
    if (si.profile == 1) si.mono = 0;
    else si.mono = (int)br.Read(1);

    const bool colorDesc = br.Read(1) != 0;
    uint32_t primaries = 2, transfer = 2, matrix = 2;   // 預設 unspecified
    if (colorDesc) {
        primaries = br.Read(8);
        transfer = br.Read(8);
        matrix = br.Read(8);
    }
    if (si.mono) {
        br.Skip(1);                              // color_range
        si.subX = 1; si.subY = 1;
        si.chromaPos = 0;
    } else if (primaries == 1 && transfer == 13 && matrix == 0) {   // BT.709/sRGB/Identity
        si.subX = 0; si.subY = 0;
        si.chromaPos = 0;
    } else {
        br.Skip(1);                              // color_range
        const int bitDepth = si.twelveBit ? 12 : (si.highBitdepth ? 10 : 8);
        if (si.profile == 0)      { si.subX = 1; si.subY = 1; }
        else if (si.profile == 1) { si.subX = 0; si.subY = 0; }
        else if (bitDepth == 12)  { si.subX = 1; si.subY = 1; }
        else                      { si.subX = 1; si.subY = 0; }
        if (si.subX && si.subY) si.chromaPos = (int)br.Read(2);
    }
    if (!br.ok) return false;

    si.levelIdx = (int)level;
    si.tier = (int)tier;
    si.width = (int)maxW;
    si.height = (int)maxH;
    return true;
}

// 從一段位元流裡找 sequence header OBU，組成 av1C 的內容。
// 找不到就回 false —— 那代表這個 access unit 不能當檔頭，不是「湊合著寫」的情況。
bool BuildAv1CFromObus(const uint8_t* p, size_t n, Bytes& out, Av1SeqInfo* info) {
    size_t off = 0;
    ObuView o;
    while (NextObu(p, n, off, o)) {
        if (o.type != kObuSeqHeader) continue;
        Av1SeqInfo si;
        if (!ParseAv1SeqHeader(o.payload, o.payloadLen, si)) return false;

        out.clear();
        U8(out, 0x81);   // marker(1)=1 + version(7)=1
        U8(out, ((uint32_t)si.profile << 5) | ((uint32_t)si.levelIdx & 0x1F));
        U8(out, ((uint32_t)si.tier << 7) | ((uint32_t)si.highBitdepth << 6) |
                ((uint32_t)si.twelveBit << 5) | ((uint32_t)si.mono << 4) |
                ((uint32_t)si.subX << 3) | ((uint32_t)si.subY << 2) |
                ((uint32_t)si.chromaPos & 0x3));
        U8(out, 0x00);   // reserved(3) + initial_presentation_delay_present(1)=0 + reserved(4)
        Raw(out, o.p, o.n);   // configOBUs：sequence header OBU 本體
        if (info) *info = si;
        return true;
    }
    return false;
}

// AV1 沒有「IDR」這種 NAL 型別，隨機存取點要看 frame header：
//   show_existing_frame=0 且 frame_type=KEY_FRAME(0)。
// 不看 show_frame —— KEY_FRAME + show_frame=0 是隱藏的 alt-ref 關鍵幀，它一樣
// 可以當 seek 點，漏掉只會少一個可 seek 位置。
bool Av1IsKeyFrame(const uint8_t* p, size_t n) {
    size_t off = 0;
    ObuView o;
    while (NextObu(p, n, off, o)) {
        if (o.type != kObuFrame && o.type != kObuFrameHdr) continue;
        BitReader br{o.payload, o.payloadLen};
        if (br.Read(1)) return false;    // show_existing_frame
        return br.Read(2) == 0;          // frame_type == KEY_FRAME
    }
    return false;
}

Bytes ColrBox() {
    // nclx, BT.709 primaries/transfer/matrix, limited range（跟 VideoProcessor 的設定一致）
    Bytes p;
    FourCC(p, "nclx");
    U16(p, 1);
    U16(p, 1);
    U16(p, 1);
    U8(p, 0);
    return Box("colr", p);
}

} // namespace

// ============================================================================

struct Mp4Muxer::Impl {
    FILE* f = nullptr;
    std::wstring path;
    Codec codec = Codec::H264;
    int width = 0;
    int height = 0;

    Bytes config;              // avcC / hvcC payload
    bool configFromMft = false;

    int64_t mdatStart = 0;
    uint64_t payloadBytes = 0;
    uint64_t totalSamples = 0;

    struct Sample {
        uint64_t off = 0;
        uint32_t size = 0;
        int64_t pts = 0;   // 100ns
        int64_t dur = 0;   // 100ns
        bool sync = false;
    };
    std::vector<Sample> samples;

    // ---- 音軌（可以有 N 條）
    //
    // 每條音軌的 Sample.pts / Sample.dur 單位是「取樣點」，不是 100ns —— 音軌的
    // timescale 直接等於取樣率，所以 AAC-LC 的每個 AU 都是整整 1024 個 tick，
    // stts 只有一個 run。影片軌才是 90000。
    //
    // 每條軌自己累加自己的時間軸（ts / nextDts），互不干擾：兩條音源的裝置時鐘
    // 不同，取樣率也可能不同（44.1k 的喇叭 + 48k 的麥克風），共用一個累加器
    // 一定錯。
    struct AudioTrack {
        int rate = 48000;
        int channels = 2;
        Bytes config;               // AudioSpecificConfig
        std::string name;           // hdlr 顯示名稱
        // 對應 tkhd 的 Track_enabled（flags bit 0）。false 時寫 6 而不是 7，
        // 播放器多半會跳過這條軌 —— 但這只是「請求」，MP4 沒有 MKV 那種
        // FlagDefault，實作不認就還是會照軌道順序挑第一條。
        bool enabled = true;

        std::vector<Sample> samples;    // progressive 用
        std::vector<Sample> frag;       // fMP4 待寫分片（pts/dur 單位是取樣點）
        Bytes fragData;

        int64_t ts = 0;             // 已寫入的取樣點數（也是下一筆的 pts）
        int64_t startTs = 0;
        bool started = false;
        int64_t nextDts = 0;        // 分片的 baseMediaDecodeTime 累加器
        int64_t startTicks = 0;     // 這條軌的起始延遲（tick = 取樣點）
        // 負偏移（音訊偏早）用：要從開頭裁掉多少 tick。
        // 為什麼需要它：elst 的 empty edit 只能把音訊往後推，不能往前拉。
        // 音訊偏早時正確的做法是「把開頭那一段丟掉」，progressive 用 elst 的
        // media_time > 0 表達（不真的丟資料），fMP4 沒有 elst，只能整包 AU 丟。
        int64_t trimTicks = 0;
        uint64_t total = 0;
    };
    std::vector<AudioTrack> aud;

    // ---- fragmented 模式的狀態
    //
    // 這裡刻意不共用 samples：progressive 需要「全部 sample 都在手上」才能算
    // stco/stsz，而 fragmented 要的是「每個 GOP 一寫完就落地」。兩者對記憶體的
    // 需求剛好相反（長錄影下 progressive 會累積幾十萬筆 Sample），所以分開存。
    bool frag = false;
    bool moovWritten = false;
    uint32_t seq = 0;              // mfhd 的 sequence_number
    int64_t moovStart = 0;         // 給 Close() 回填時長用
    int64_t moovSize = 0;
    int64_t nextDts = 0;           // baseMediaDecodeTime 累加器（timescale）
    int64_t firstPts100 = 0;       // 讓分片時間從 0 起算
    bool haveFirstPts = false;
    std::vector<Sample> fragSamples;   // 待寫出的分片（pts/dur/sync）
    Bytes fragData;                    // 待寫出的分片 payload（AVCC）

    std::string codecName() const {
        switch (codec) {
            case Codec::HEVC: return "HEVC";
            case Codec::AV1:  return "AV1";
            default:          return "H.264";
        }
    }

    bool fail(std::string* err, const std::string& m) {
        if (err) *err = m;
        return false;
    }

    // 時間軸。B 幀讓 pts 在 decode order 裡來回跳，dts 必須另外推：
    //   - pts 單調  → dts = pts，不寫 ctts（本專案預設，BPictureCount=0）
    //   - 否則      → dts 取「第 i 個最小 pts」（rank 法），再整體平移讓
    //                 ctts 全部非負。這是 mp4 表示 B 幀的標準做法。
    struct Timeline {
        std::vector<int64_t> dts;    // timescale 單位
        std::vector<int64_t> ctts;   // 空 = 不需要 ctts
        std::vector<int64_t> delta;
        int64_t duration = 0;
    };

    Bytes BuildStsd() const;
    Bytes BuildStsdAudio(const AudioTrack& a) const;
    Bytes BuildStbl() const;
    Bytes BuildStblFor(const std::vector<Sample>& v, bool convert100ns,
                       const AudioTrack* audio = nullptr) const;
    Bytes BuildMoov() const;
    Bytes BuildMoovFragmented(int64_t durTs) const;
    Bytes BuildStblEmpty(const AudioTrack* audio = nullptr) const;
    Timeline BuildTimeline(const std::vector<Sample>& v, bool convert100ns) const;
    Bytes BuildTraf(const std::vector<Sample>& v, uint32_t trackId, int64_t baseDts,
                    bool convert100ns, size_t* offField, int64_t* durTs) const;
    bool WriteFragment(std::string* err);
};

Mp4Muxer::Impl::Timeline Mp4Muxer::Impl::BuildTimeline(const std::vector<Sample>& v,
                                                       bool convert100ns) const {
    Timeline tl;
    const size_t n = v.size();
    if (n == 0) return tl;

    std::vector<int64_t> pts(n);
    for (size_t i = 0; i < n; ++i) pts[i] = convert100ns ? ToTs(v[i].pts) : v[i].pts;

    bool monotonic = true;
    for (size_t i = 1; i < n; ++i) {
        if (pts[i] < pts[i - 1]) { monotonic = false; break; }
    }

    tl.dts = pts;
    if (!monotonic) {
        std::vector<int64_t> sorted = pts;
        std::sort(sorted.begin(), sorted.end());
        tl.dts = sorted;

        int64_t minOff = 0;
        for (size_t i = 0; i < n; ++i) minOff = std::min(minOff, pts[i] - tl.dts[i]);
        int64_t shift = -minOff;
        if (tl.dts[0] + shift < 0) shift = -tl.dts[0];
        for (size_t i = 0; i < n; ++i) {
            pts[i] += shift;
            tl.dts[i] += shift;
        }
        tl.ctts.resize(n);
        for (size_t i = 0; i < n; ++i) tl.ctts[i] = pts[i] - tl.dts[i];
    }

    tl.delta.resize(n);
    for (size_t i = 0; i + 1 < n; ++i) tl.delta[i] = std::max<int64_t>(tl.dts[i + 1] - tl.dts[i], 1);
    tl.delta[n - 1] = std::max<int64_t>(convert100ns ? ToTs(v[n - 1].dur) : v[n - 1].dur, 1);

    tl.duration = tl.dts[n - 1] + tl.delta[n - 1];
    return tl;
}

Bytes Mp4Muxer::Impl::BuildStsd() const {
    const char* entry = (codec == Codec::AV1) ? "av01"
                      : (codec == Codec::HEVC) ? "hvc1" : "avc1";
    const char* cfgBox = (codec == Codec::AV1) ? "av1C"
                       : (codec == Codec::HEVC) ? "hvcC" : "avcC";

    Bytes e;
    Zeros(e, 6);            // reserved
    U16(e, 1);              // data_reference_index
    U16(e, 0);              // pre_defined
    U16(e, 0);              // reserved
    Zeros(e, 12);           // pre_defined[3]
    U16(e, (uint32_t)width);
    U16(e, (uint32_t)height);
    U32(e, 0x00480000);     // 72 dpi
    U32(e, 0x00480000);
    U32(e, 0);              // reserved
    U16(e, 1);              // frame_count
    // compressorname：32 bytes，第一個 byte 是長度
    const char* name = "CapWGC";
    U8(e, (uint32_t)strlen(name));
    Raw(e, (const uint8_t*)name, strlen(name));
    Zeros(e, 31 - strlen(name));
    U16(e, 0x0018);         // depth
    U16(e, 0xFFFF);         // pre_defined = -1
    Append(e, Box(cfgBox, config));
    Append(e, ColrBox());

    Bytes p;
    U8(p, 0); U24(p, 0);
    U32(p, 1);              // entry_count
    Append(p, Box(entry, e));
    return Box("stsd", p);
}

// mp4a + esds。AAC 的 DecoderSpecificInfo 就是 AudioSpecificConfig（ASC），
// 由 MF AAC encoder 的輸出型別 MF_MT_USER_DATA 給，不是我們自己拼的 ——
// 自己拼的話取樣率索引與 channel config 很容易跟實際編碼不一致，
// 播放器會照 ASC 解，於是播出來是雜音或完全沒聲。
Bytes Mp4Muxer::Impl::BuildStsdAudio(const AudioTrack& a) const {
    Bytes dsi;                                  // DecoderSpecificInfo
    Desc(dsi, 0x05, a.config);

    Bytes dcdP;                                 // DecoderConfigDescriptor 的內容
    dcdP.push_back(0x40);                       // objectTypeIndication: MPEG-4 AAC
    dcdP.push_back(0x15);                       // streamType=5(audio)<<2 | upStream=0 | 1
    dcdP.push_back(0x00);                       // bufferSizeDB（24-bit）
    dcdP.push_back(0x00);
    dcdP.push_back(0x00);
    U32(dcdP, (uint32_t)(a.rate * a.channels * 16));   // maxBitrate（粗估上限）
    U32(dcdP, (uint32_t)(a.rate * a.channels * 4));    // avgBitrate（128 kbps @48k 立體）
    Append(dcdP, dsi);

    Bytes esP;                                  // ES_Descriptor 的內容
    U16(esP, 0);                                // ES_ID
    esP.push_back(0x00);                        // flags：無 dependency / URL / OCR
    Desc(esP, 0x04, dcdP);

    Bytes slP;
    slP.push_back(0x02);                        // predefined = MP4
    Desc(esP, 0x06, slP);

    // 最外層還有一個 ES_Descriptor（tag 0x03）。漏掉它的症狀極具誤導性：
    // 檔案結構、box 大小、entry_count 全部合法，MF 也照樣「開得起來」，
    // 但它只認得影像軌 —— 音軌整條消失，回的是 MF_E_INVALIDSTREAMNUMBER，
    // 看起來像「這個檔沒有音訊」而不是「esds 少了一層」。
    Bytes esAll;
    Desc(esAll, 0x03, esP);

    Bytes esds = FullBox("esds", 0, 0, esAll);

    Bytes e;
    Zeros(e, 6);                // reserved
    U16(e, 1);                  // data_reference_index
    Zeros(e, 8);                // version / revision / vendor
    U16(e, (uint32_t)a.channels);
    U16(e, 16);                 // samplesize
    U16(e, 0);                  // pre_defined
    U16(e, 0);                  // reserved
    U32(e, ((uint32_t)a.rate) << 16);   // 16.16 fixed
    Append(e, esds);

    Bytes p;
    U8(p, 0); U24(p, 0);
    U32(p, 1);
    Append(p, Box("mp4a", e));
    return Box("stsd", p);
}

Bytes Mp4Muxer::Impl::BuildStbl() const {
    return BuildStblFor(samples, true);
}

// convert100ns = false 時 v 的 pts/dur 已經是 timescale 單位（音軌）。
Bytes Mp4Muxer::Impl::BuildStblFor(const std::vector<Sample>& v, bool convert100ns,
                                  const AudioTrack* audio) const {
    const Timeline tl = BuildTimeline(v, convert100ns);
    const size_t n = v.size();

    // ---- stts（run-length）
    Bytes sttsP;
    U8(sttsP, 0); U24(sttsP, 0);
    struct Run { uint32_t count; int64_t delta; };
    std::vector<Run> runs;
    for (size_t i = 0; i < n; ++i) {
        const int64_t d = tl.delta[i];
        if (!runs.empty() && runs.back().delta == d) runs.back().count++;
        else runs.push_back({1, d});
    }
    U32(sttsP, (uint32_t)runs.size());
    for (const auto& r : runs) { U32(sttsP, r.count); U32(sttsP, (uint32_t)r.delta); }

    // ---- ctts（只有 B 幀才需要）
    Bytes cttsP;
    if (!tl.ctts.empty()) {
        U8(cttsP, 0); U24(cttsP, 0);
        std::vector<Run> cr;
        for (size_t i = 0; i < n; ++i) {
            const int64_t off = tl.ctts[i];
            if (!cr.empty() && cr.back().delta == off) cr.back().count++;
            else cr.push_back({1, off});
        }
        U32(cttsP, (uint32_t)cr.size());
        for (const auto& r : cr) { U32(cttsP, r.count); U32(cttsP, (uint32_t)r.delta); }
    }

    // ---- stss（全部都是 sync 時整張表沒有意義，省略）
    Bytes stssP;
    {
        std::vector<uint32_t> sync;
        for (size_t i = 0; i < n; ++i) if (v[i].sync) sync.push_back((uint32_t)(i + 1));
        if (sync.size() != n) {
            U8(stssP, 0); U24(stssP, 0);
            U32(stssP, (uint32_t)sync.size());
            for (uint32_t s : sync) U32(stssP, s);
        }
    }

    // ---- stsc：每個 sample 一個 chunk
    Bytes stscP;
    U8(stscP, 0); U24(stscP, 0);
    U32(stscP, 1);
    U32(stscP, 1);   // first_chunk
    U32(stscP, 1);   // samples_per_chunk
    U32(stscP, 1);   // sample_description_index

    // ---- stsz
    Bytes stszP;
    U8(stszP, 0); U24(stszP, 0);
    U32(stszP, 0);            // sample_size = 0 → 每個 sample 各自給
    U32(stszP, (uint32_t)n);
    for (const auto& s : v) U32(stszP, s.size);

    // ---- stco / co64：超過 4 GB 的檔案只能用 64 位偏移
    Bytes stcoP;
    bool need64 = false;
    for (const auto& s : v) if (s.off > 0xFFFFFFFFull) { need64 = true; break; }
    U8(stcoP, 0); U24(stcoP, 0);
    U32(stcoP, (uint32_t)n);
    if (need64) {
        for (const auto& s : v) U64(stcoP, s.off);
    } else {
        for (const auto& s : v) U32(stcoP, (uint32_t)s.off);
    }

    Bytes stbl;
    Append(stbl, audio ? BuildStsdAudio(*audio) : BuildStsd());
    Append(stbl, Box("stts", sttsP));
    if (!cttsP.empty()) Append(stbl, Box("ctts", cttsP));
    if (!stssP.empty()) Append(stbl, Box("stss", stssP));
    Append(stbl, Box("stsc", stscP));
    Append(stbl, Box("stsz", stszP));
    Append(stbl, Box(need64 ? "co64" : "stco", stcoP));
    return Box("stbl", stbl);
}

Bytes Mp4Muxer::Impl::BuildMoov() const {
    const Timeline vtl = BuildTimeline(samples, true);
    const int64_t trackDur = vtl.duration;
    const uint32_t videoDurMs = (uint32_t)((trackDur * 1000) / kTimescale);

    std::vector<int64_t> audioDurTs(aud.size(), 0);
    uint32_t movieDur = videoDurMs;
    for (size_t i = 0; i < aud.size(); ++i) {
        audioDurTs[i] = BuildTimeline(aud[i].samples, false).duration;
        // 這條軌在 movie 時間軸上佔的長度 = 起始延遲 + 實際呈現的媒體長度。
        // 有裁切（負偏移）時要把裁掉的那一段扣掉，否則 mvhd 會多算。
        const int64_t presented = audioDurTs[i] - aud[i].trimTicks;
        const uint32_t ms = aud[i].rate > 0
                                ? (uint32_t)(((presented + aud[i].startTicks) * 1000) /
                                             aud[i].rate)
                                : 0;
        if (ms > movieDur) movieDur = ms;
    }

    Bytes mvhdP;
    U32(mvhdP, 0);              // creation
    U32(mvhdP, 0);              // modification
    U32(mvhdP, 1000);           // timescale
    U32(mvhdP, movieDur);
    U32(mvhdP, 0x00010000);     // rate
    U16(mvhdP, 0x0100);         // volume
    Zeros(mvhdP, 2);
    Zeros(mvhdP, 8);
    const uint32_t unity[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : unity) U32(mvhdP, v);
    Zeros(mvhdP, 24);
    U32(mvhdP, (uint32_t)(2 + aud.size()));     // next_track_ID

    Bytes tkhdP;
    U32(tkhdP, 0);
    U32(tkhdP, 0);
    U32(tkhdP, 1);              // track_ID
    U32(tkhdP, 0);
    U32(tkhdP, videoDurMs);
    Zeros(tkhdP, 8);
    U16(tkhdP, 0);              // layer
    U16(tkhdP, 0);              // alternate_group
    U16(tkhdP, 0);              // volume (video = 0)
    U16(tkhdP, 0);
    for (uint32_t v : unity) U32(tkhdP, v);
    U32(tkhdP, (uint32_t)width << 16);
    U32(tkhdP, (uint32_t)height << 16);

    Bytes mdhdP;
    U32(mdhdP, 0);
    U32(mdhdP, 0);
    U32(mdhdP, (uint32_t)kTimescale);
    U32(mdhdP, (uint32_t)trackDur);
    U16(mdhdP, 0x55C4);         // 'und'
    U16(mdhdP, 0);

    Bytes hdlrP;
    U32(hdlrP, 0);              // pre_defined
    FourCC(hdlrP, "vide");
    Zeros(hdlrP, 12);
    const char* nm = "VideoHandler";
    Raw(hdlrP, (const uint8_t*)nm, strlen(nm) + 1);

    Bytes vmhdP;
    U16(vmhdP, 0);              // graphicsmode
    Zeros(vmhdP, 6);            // opcolor

    Bytes drefP;
    U32(drefP, 1);
    Append(drefP, FullBox("url ", 0, 1, Bytes()));

    Bytes minf = Container("minf",
        {FullBox("vmhd", 0, 1, vmhdP),
         Container("dinf", {FullBox("dref", 0, 0, drefP)}),
         BuildStbl()});

    Bytes mdia = Container("mdia",
        {FullBox("mdhd", 0, 0, mdhdP),
         FullBox("hdlr", 0, 0, hdlrP),
         minf});

    Bytes trak = Container("trak", {FullBox("tkhd", 0, 7, tkhdP), mdia});

    std::vector<Bytes> children;
    children.push_back(FullBox("mvhd", 0, 0, mvhdP));
    children.push_back(trak);

    // ---- 音軌（可以有 N 條）
    //
    // tkhd 的 volume 是 1.0（0x0100）而不是 0，寬高 0；mdhd 的 timescale 是取樣率，
    // 不是 90000。音軌用 90000 也能播，但每個 AAC frame 會變成 1919.99 個 tick，
    // stts 的 delta 必須在 1919/1920 之間跳，時間軸累積誤差會慢慢浮出來。
    //
    // alternate_group 一律留 0：設成同一個非零值等於告訴播放器「這幾條是互斥的
    // 替代版本，一次只播一條」。系統聲 + 麥克風要同時聽得到，所以不能設。
    for (size_t i = 0; i < aud.size(); ++i) {
        const AudioTrack& a = aud[i];
        const uint32_t delayMs =
            a.rate > 0 ? (uint32_t)((a.startTicks * 1000) / a.rate) : 0;
        const uint32_t mediaMs = a.rate > 0 ? (uint32_t)((audioDurTs[i] * 1000) / a.rate) : 0;
        // 負偏移（音訊偏早）走裁切：呈現長度 = 媒體長度 − 裁掉的那一段。
        const uint32_t trimMs =
            a.rate > 0 ? (uint32_t)((a.trimTicks * 1000) / a.rate) : 0;
        const uint32_t presentedMs = mediaMs > trimMs ? mediaMs - trimMs : 0;

        Bytes tkhdA;
        U32(tkhdA, 0);
        U32(tkhdA, 0);
        U32(tkhdA, (uint32_t)(2 + i));      // track_ID
        U32(tkhdA, 0);
        U32(tkhdA, delayMs + presentedMs);      // 含延遲的呈現長度
        Zeros(tkhdA, 8);
        U16(tkhdA, 0);              // layer
        U16(tkhdA, 0);              // alternate_group（見上）
        U16(tkhdA, 0x0100);         // volume
        U16(tkhdA, 0);
        for (uint32_t v : unity) U32(tkhdA, v);
        U32(tkhdA, 0);              // width
        U32(tkhdA, 0);              // height

        Bytes mdhdA;
        U32(mdhdA, 0);
        U32(mdhdA, 0);
        U32(mdhdA, (uint32_t)a.rate);
        U32(mdhdA, (uint32_t)audioDurTs[i]);    // 媒體本身的長度，不含延遲
        U16(mdhdA, 0x55C4);
        U16(mdhdA, 0);

        const std::string label = a.name.empty() ? std::string("SoundHandler") : a.name;

        Bytes hdlrA;
        U32(hdlrA, 0);
        FourCC(hdlrA, "soun");
        Zeros(hdlrA, 12);
        Raw(hdlrA, (const uint8_t*)label.c_str(), label.size() + 1);

        Bytes smhdP;
        U16(smhdP, 0);              // balance
        U16(smhdP, 0);              // reserved

        Bytes minfA = Container("minf",
            {FullBox("smhd", 0, 0, smhdP),
             Container("dinf", {FullBox("dref", 0, 0, drefP)}),
             BuildStblFor(a.samples, false, &a)});

        Bytes mdiaA = Container("mdia",
            {FullBox("mdhd", 0, 0, mdhdA),
             FullBox("hdlr", 0, 0, hdlrA),
             minfA});

        // ---- edts/elst：表達「這條軌的時間軸相對媒體位移了多少」
        //
        // stts 只存相鄰差，第一筆的 decode time 恆為 0，所以「晚 80ms 開始」只能
        // 靠 edit list 表達。三種情況：
        //
        //   音訊偏晚（delayMs > 0）：兩筆。第一筆 empty edit（media_time = -1，
        //     只佔時間軸、不對應任何 sample），長度就是延遲；第二筆才是真正的
        //     媒體（media_time = 0）。
        //   音訊偏早（trimMs > 0）：一筆，media_time = 裁掉的 tick 數 ——
        //     等於「從媒體的 trimMs 處開始播」，也就是把開頭那段丟掉。
        //     這一筆的 segment_duration 是 movie timescale，media_time 是
        //     **media timescale**，兩者單位不同，很容易寫錯。
        //   沒有偏移：不寫 edts。
        Bytes edts;
        if (delayMs > 0) {
            Bytes elstP;
            U32(elstP, 2);                  // entry_count
            U32(elstP, delayMs);            // empty edit 的長度（movie timescale）
            U32(elstP, 0xFFFFFFFF);         // media_time = -1（空段）
            U16(elstP, 1);                  // media_rate_integer
            U16(elstP, 0);                  // media_rate_fraction
            U32(elstP, presentedMs);        // 真正的媒體
            U32(elstP, 0);                  // media_time = 0
            U16(elstP, 1);
            U16(elstP, 0);
            edts = Container("edts", {FullBox("elst", 0, 0, elstP)});
        } else if (trimMs > 0 && a.trimTicks < 0x7FFFFFFF) {
            Bytes elstP;
            U32(elstP, 1);                              // entry_count
            U32(elstP, presentedMs);                    // 呈現長度（movie timescale）
            U32(elstP, (uint32_t)a.trimTicks);          // media_time（media timescale）
            U16(elstP, 1);
            U16(elstP, 0);
            edts = Container("edts", {FullBox("elst", 0, 0, elstP)});
        }

        std::vector<Bytes> trakChildren;
        // flags 7 = enabled|in_movie|in_preview；6 = 拿掉 enabled。
        // 分軌（剪輯用）那一條用 6：混音軌已經有它的內容，一般播放器不該
        // 再播一次，否則麥克風會疊兩聲。
        trakChildren.push_back(FullBox("tkhd", 0, a.enabled ? 7 : 6, tkhdA));
        if (!edts.empty()) trakChildren.push_back(edts);
        trakChildren.push_back(mdiaA);
        children.push_back(ContainerV("trak", trakChildren));
    }

    return ContainerV("moov", children);
}

// ------------------------------------------------------------------ fMP4
//
// fragmented 的 moov 和 progressive 幾乎一樣，差別只在兩處：
//   1. stbl 是空的（真正的取樣表在每個 moof 裡）
//   2. 多一個 mvex/trex，告訴播放器「這是分片檔，每片自帶時長」
//
// 空 stbl 不是「省略」，是「存在但 entry_count = 0」。少了 stsd 播放器會不知道
// 這是什麼編碼，少了那幾個空表有些解析器會直接判定檔案損壞。
Bytes Mp4Muxer::Impl::BuildStblEmpty(const AudioTrack* audio) const {
    // 注意：BuildStsd() 回傳的**已經是一個完整的 stsd box**，不要再包一層。
    // 包錯的症狀極具誤導性：檔案結構、每個 box 的大小、entry_count 全部合法，
    // 但 MF 把內層的 box header 當成 sample entry，於是把 'stsd' 這四個字讀成
    // 編碼型別，回一個 subtype = 73747364-... 的鬼東西。
    auto empty = [](const char* type) {
        Bytes p;
        U32(p, 0);          // entry_count = 0
        return FullBox(type, 0, 0, p);
    };

    // stsz 不能套用上面的「只寫 entry_count」：它的版面是
    //   version/flags, sample_size(4), sample_count(4)
    // 少寫 sample_size 會讓後面每一個 box 的邊界都錯開 4 bytes。
    Bytes stszP;
    U32(stszP, 0);          // sample_size
    U32(stszP, 0);          // sample_count

    return Container("stbl", {
        audio ? BuildStsdAudio(*audio) : BuildStsd(),
        empty("stts"),
        empty("stsc"),
        FullBox("stsz", 0, 0, stszP),
        empty("stco"),
    });
}

Bytes Mp4Muxer::Impl::BuildMoovFragmented(int64_t durTs) const {
    const uint32_t videoDurMs = (uint32_t)((durTs * 1000) / kTimescale);
    uint32_t movieDur = videoDurMs;
    for (size_t i = 0; i < aud.size(); ++i) {
        const uint32_t ms = aud[i].rate > 0
                                ? (uint32_t)((aud[i].nextDts * 1000) / aud[i].rate)
                                : 0;
        if (ms > movieDur) movieDur = ms;
    }

    Bytes mvhdP;
    U32(mvhdP, 0);
    U32(mvhdP, 0);
    U32(mvhdP, 1000);
    U32(mvhdP, movieDur);
    U32(mvhdP, 0x00010000);
    U16(mvhdP, 0x0100);
    Zeros(mvhdP, 2);
    Zeros(mvhdP, 8);
    const uint32_t unity[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : unity) U32(mvhdP, v);
    Zeros(mvhdP, 24);
    U32(mvhdP, (uint32_t)(2 + aud.size()));

    Bytes drefP;
    U32(drefP, 1);
    Append(drefP, FullBox("url ", 0, 1, Bytes()));

    // 每一條軌的 tkhd/mdhd/hdlr 只差在幾個欄位，用一個 lambda 收掉重複的部分。
    // audio 為 nullptr 就是影像軌。
    auto buildTrak = [&](uint32_t trackId, const AudioTrack* a, int64_t dur,
                         uint32_t durMs) {
        Bytes tkhdP;
        U32(tkhdP, 0);
        U32(tkhdP, 0);
        U32(tkhdP, trackId);
        U32(tkhdP, 0);
        U32(tkhdP, durMs);
        Zeros(tkhdP, 8);
        U16(tkhdP, 0);
        U16(tkhdP, 0);
        U16(tkhdP, a ? 0x0100 : 0);     // volume
        U16(tkhdP, 0);
        for (uint32_t v : unity) U32(tkhdP, v);
        U32(tkhdP, a ? 0u : ((uint32_t)width << 16));
        U32(tkhdP, a ? 0u : ((uint32_t)height << 16));

        Bytes mdhdP;
        U32(mdhdP, 0);
        U32(mdhdP, 0);
        U32(mdhdP, (uint32_t)(a ? a->rate : kTimescale));
        // fMP4 沒有 elst：起始延遲是靠第一個 tfdt 的起點表達的，所以 mdhd 這裡
        // 只寫「媒體本身」的長度（nextDts 已含延遲，要扣掉）。
        U32(mdhdP, (uint32_t)(a ? (a->nextDts - a->startTicks) : dur));
        U16(mdhdP, 0x55C4);
        U16(mdhdP, 0);

        const std::string label = a ? (a->name.empty() ? std::string("SoundHandler") : a->name)
                                    : std::string("VideoHandler");

        Bytes hdlrP;
        U32(hdlrP, 0);
        FourCC(hdlrP, a ? "soun" : "vide");
        Zeros(hdlrP, 12);
        Raw(hdlrP, (const uint8_t*)label.c_str(), label.size() + 1);

        Bytes mediaHeader;
        if (a) {
            Bytes smhdP;
            U16(smhdP, 0);
            U16(smhdP, 0);
            mediaHeader = FullBox("smhd", 0, 0, smhdP);
        } else {
            Bytes vmhdP;
            U16(vmhdP, 0);
            Zeros(vmhdP, 6);
            mediaHeader = FullBox("vmhd", 0, 1, vmhdP);
        }

        Bytes minf = Container("minf",
            {mediaHeader,
             Container("dinf", {FullBox("dref", 0, 0, drefP)}),
             BuildStblEmpty(a)});

        Bytes mdia = Container("mdia",
            {FullBox("mdhd", 0, 0, mdhdP),
             FullBox("hdlr", 0, 0, hdlrP),
             minf});

        // 分軌（給剪輯用）寫成 Track_enabled=0：flags 6 而不是 7。
        return Container("trak", {FullBox("tkhd", 0, (a && !a->enabled) ? 6 : 7, tkhdP), mdia});
    };

    // trex：分片的預設值。全 0 代表「每一片自己帶完整資訊」，這是合法且
    // 最不容易出錯的選擇（預設值一旦和實際不符，播放器會靜默播錯速度）。
    auto buildTrex = [](uint32_t trackId) {
        Bytes p;
        U32(p, trackId);
        U32(p, 1);      // default_sample_description_index
        U32(p, 0);      // default_sample_duration
        U32(p, 0);      // default_sample_size
        U32(p, 0);      // default_sample_flags
        return FullBox("trex", 0, 0, p);
    };

    std::vector<Bytes> traks;
    std::vector<Bytes> trexs;
    traks.push_back(buildTrak(1, nullptr, durTs, videoDurMs));
    trexs.push_back(buildTrex(1));
    for (size_t i = 0; i < aud.size(); ++i) {
        // tkhd 的長度 = 延遲 + 媒體（呈現時間軸上的總長），mdhd 由 buildTrak 自己算。
        const uint32_t ms = aud[i].rate > 0
                                ? (uint32_t)((aud[i].nextDts * 1000) / aud[i].rate)
                                : 0;
        traks.push_back(buildTrak((uint32_t)(2 + i), &aud[i], aud[i].nextDts, ms));
        trexs.push_back(buildTrex((uint32_t)(2 + i)));
    }

    std::vector<Bytes> children;
    children.push_back(FullBox("mvhd", 0, 0, mvhdP));
    for (auto& t : traks) children.push_back(std::move(t));
    children.push_back(ContainerV("mvex", trexs));
    return ContainerV("moov", children);
}

// 一個 traf（單軌的一片）。offField 回傳「data_offset 欄位在這個 traf 內的位移」，
// 因為 trun 的 data_offset 要等整個 moof 組好才知道。
Bytes Mp4Muxer::Impl::BuildTraf(const std::vector<Sample>& v, uint32_t trackId,
                                int64_t baseDts, bool convert100ns,
                                size_t* offField, int64_t* durTs) const {
    const Timeline tl = BuildTimeline(v, convert100ns);
    const size_t n = v.size();

    Bytes tfhdP;
    U32(tfhdP, trackId);
    Bytes tfhd = FullBox("tfhd", 0, 0x020000, tfhdP);   // default-base-is-moof

    Bytes tfdtP;
    U64(tfdtP, (uint64_t)baseDts);       // 版本 1 才有 64 位
    Bytes tfdt = FullBox("tfdt", 1, 0, tfdtP);

    Bytes trunP;
    U32(trunP, (uint32_t)n);             // sample_count
    U32(trunP, 0);                       // data_offset（下面回填）
    for (size_t i = 0; i < n; ++i) {
        U32(trunP, (uint32_t)tl.delta[i]);
        U32(trunP, v[i].size);
        // sample_flags：sample_depends_on(2) + sample_is_non_sync_sample(1)
        //   I 幀：depends_on = 2（不依賴別人）、non_sync = 0
        //   其他：depends_on = 1、non_sync = 1
        // 音訊沒有「關鍵幀」的概念，全部標成 sync。
        U32(trunP, v[i].sync ? 0x02000000u : 0x01010000u);
        if (!tl.ctts.empty()) U32(trunP, (uint32_t)tl.ctts[i]);
    }
    uint32_t trunFlags = 0x000001 | 0x000100 | 0x000200 | 0x000400;
    if (!tl.ctts.empty()) trunFlags |= 0x000800;
    Bytes trun = FullBox("trun", 0, trunFlags, trunP);

    Bytes traf = Container("traf", {tfhd, tfdt, trun});

    // 偏移要數清楚，少算一個 4 就會把 sample_count 蓋掉，而且症狀極難看出來：
    // 檔案結構看起來完全正常、每個 box 大小都對，只有 MF 回一個
    // MF_E_UNSUPPORTED_BYTESTREAM_TYPE（0xC00D36C4），像是「格式不支援」。
    //
    //   traf header(8) + tfhd + tfdt
    //   + trun header(8) + ver/flags(4) + sample_count(4)  ← data_offset 在這裡
    *offField = 8 + tfhd.size() + tfdt.size() + 8 + 4 + 4;

    // 這一片的長度是「最後一個 dts + 它的時長 - 第一個 dts」。
    //
    // 不能直接用 tl.duration：BuildTimeline 回傳的是「時間軸上的結束位置」，
    // 而分片的 pts 是相對於整段錄影的起點。拿結束位置去累加 baseMediaDecodeTime
    // 會讓時間軸每片膨脹一次 —— 實測 5 秒的錄影播出來是 7 秒，而且越長越誇張。
    *durTs = tl.dts.back() + tl.delta.back() - tl.dts.front();
    return traf;
}

// 把目前累積的分片寫成 moof + mdat。
//
// 有音軌時一個 moof 裡有 1 + N 個 traf、共用一個 mdat：影像在前、各條音軌依序
// 在後，每個 trun 的 data_offset 各自指向自己的第一筆。分片仍然是自包含的
// （default-base-is-moof），所以被 kill 之後每一片都能單獨解。
bool Mp4Muxer::Impl::WriteFragment(std::string* err) {
    // moov 還沒寫就落分片 = 生出一個沒有 moov 的檔（播放器直接判定損壞）。
    // 正常路徑不會走到這裡（moov 在寫第一個影像 sample 時就落地了），但音訊
    // 是背景執行緒送進來的，防呆比追查「為什麼檔案打不開」便宜。
    if (!moovWritten) return true;

    bool any = !fragSamples.empty();
    for (const auto& a : aud) any = any || !a.frag.empty();
    if (!any) return true;

    Bytes mfhdP;
    U32(mfhdP, seq);
    Bytes mfhd = FullBox("mfhd", 0, 0, mfhdP);

    // 這一片的成員，順序固定：影像 → 音軌 0 → 音軌 1 → ……
    // moof 的資料順序與 mdat 的 payload 順序必須完全一致，data_offset 就是靠
    // 「前面所有成員的 traf 大小 / payload 大小」累加出來的。
    struct Part {
        Bytes traf;
        size_t offField = 0;
        const Bytes* data = nullptr;
        int64_t dur = 0;
        int audioTrack = -1;      // -1 = 影像
    };
    std::vector<Part> parts;

    if (!fragSamples.empty()) {
        Part p;
        p.traf = BuildTraf(fragSamples, 1, nextDts, true, &p.offField, &p.dur);
        p.data = &fragData;
        parts.push_back(std::move(p));
    }
    for (size_t i = 0; i < aud.size(); ++i) {
        if (aud[i].frag.empty()) continue;
        Part p;
        p.traf = BuildTraf(aud[i].frag, (uint32_t)(2 + i), aud[i].nextDts, false,
                           &p.offField, &p.dur);
        p.data = &aud[i].fragData;
        p.audioTrack = (int)i;
        parts.push_back(std::move(p));
    }

    std::vector<Bytes> moofChildren;
    moofChildren.push_back(mfhd);
    for (auto& p : parts) moofChildren.push_back(p.traf);
    Bytes moof = ContainerV("moof", moofChildren);

    // 每個成員的 data_offset = moof 大小 + mdat header(8) + 前面所有成員的 payload
    size_t base = 8 + mfhd.size();
    uint64_t payload = 0;
    for (auto& p : parts) payload += p.data->size();

    auto patch = [&moof](size_t at, uint32_t val) {
        if (at + 4 > moof.size()) return false;
        moof[at + 0] = (uint8_t)(val >> 24);
        moof[at + 1] = (uint8_t)(val >> 16);
        moof[at + 2] = (uint8_t)(val >> 8);
        moof[at + 3] = (uint8_t)val;
        return true;
    };

    uint64_t dataOff = (uint64_t)moof.size() + 8;
    for (auto& p : parts) {
        if (!patch(base + p.offField, (uint32_t)dataOff)) {
            return fail(err, p.audioTrack < 0 ? "internal: video trun offset overflow"
                                              : "internal: audio trun offset overflow");
        }
        base += p.traf.size();
        dataOff += p.data->size();
    }

    Bytes mdatHdr;
    U32(mdatHdr, (uint32_t)(payload + 8));
    FourCC(mdatHdr, "mdat");

    if (fwrite(moof.data(), 1, moof.size(), f) != moof.size()) return fail(err, "write moof failed");
    if (fwrite(mdatHdr.data(), 1, mdatHdr.size(), f) != mdatHdr.size()) return fail(err, "write mdat header failed");
    for (auto& p : parts) {
        if (!p.data->empty() && fwrite(p.data->data(), 1, p.data->size(), f) != p.data->size()) {
            return fail(err, p.audioTrack < 0 ? "write mdat payload failed"
                                              : "write audio payload failed");
        }
    }
    // 立刻推出去。少這一步，行程被 kill 時最後一片會停在 CRT 的緩衝區裡 ——
    // 「每片都落地」是這個模式唯一的價值，不能在這裡省。
    fflush(f);

    for (auto& p : parts) {
        if (p.audioTrack < 0) nextDts += p.dur;
        else aud[(size_t)p.audioTrack].nextDts += p.dur;
    }
    fragSamples.clear();
    fragData.clear();
    for (auto& a : aud) { a.frag.clear(); a.fragData.clear(); }
    ++seq;
    return true;
}

// ============================================================================

Mp4Muxer::Mp4Muxer() : impl_(std::make_unique<Impl>()) {}
Mp4Muxer::~Mp4Muxer() { Close(nullptr); }

bool Mp4Muxer::Open(const std::wstring& path, Codec codec, int width, int height,
                    const std::vector<uint8_t>& configRecord, std::string* err,
                    bool fragmented, const std::vector<AudioTrackConfig>* audioTracks) {
    auto& I = *impl_;
    I.path = path;
    I.codec = codec;
    I.width = width;
    I.height = height;
    I.config.clear();
    if (codec == Codec::AV1) {
        // AV1 的 configRecord 是原始 OBU（通常 TD + sequence header），不是 avcC
        // 那種「版本號開頭」的結構。組不起來就留空，等第一個關鍵幀從 in-band 補。
        if (!configRecord.empty()) {
            Av1SeqInfo si;
            if (!BuildAv1CFromObus(configRecord.data(), configRecord.size(), I.config, &si)) {
                I.config.clear();
            }
        }
    } else {
        I.config = configRecord;
    }
    I.configFromMft = !I.config.empty();
    I.samples.clear();
    I.payloadBytes = 0;
    I.totalSamples = 0;

    I.frag = fragmented;
    I.moovWritten = false;
    I.seq = 0;
    I.moovStart = 0;
    I.moovSize = 0;
    I.nextDts = 0;
    I.firstPts100 = 0;
    I.haveFirstPts = false;
    I.fragSamples.clear();
    I.fragData.clear();

    // ---- 音軌
    I.aud.clear();
    if (audioTracks) {
        for (const auto& a : *audioTracks) {
            if (a.sampleRate <= 0 || a.channels <= 0 || a.decoderConfig.empty()) continue;
            Impl::AudioTrack t;
            t.rate = a.sampleRate;
            t.channels = a.channels;
            t.config = a.decoderConfig;
            t.name = a.name;
            t.enabled = a.enabled;
            // 起始延遲換算成這條軌自己的 tick（= 取樣點），四捨五入。
            //   正值（音訊偏晚）→ startTicks，progressive 用 elst 的 empty edit 表達
            //   負值（音訊偏早）→ trimTicks，progressive 用 elst 的 media_time 表達、
            //     fMP4 真的丟掉開頭幾個 AU。走 SetAudioStartOffset() 同一套邏輯，
            //     所以這裡不再把負值吃掉。
            const int64_t off = a.startOffset100ns;
            const int64_t ticks =
                (off * (int64_t)t.rate + (off >= 0 ? 5000000 : -5000000)) / 10000000;
            if (ticks >= 0) {
                t.startTicks = ticks;
                t.trimTicks = 0;
            } else {
                t.startTicks = 0;
                t.trimTicks = -ticks;
            }
            t.nextDts = t.startTicks;
            I.aud.push_back(std::move(t));
        }
    }

    I.f = _wfopen(path.c_str(), L"wb");
    if (!I.f) return I.fail(err, "cannot open output file");

    // ftyp
    Bytes ftypP;
    FourCC(ftypP, "isom");
    U32(ftypP, 0x200);
    FourCC(ftypP, "isom");
    FourCC(ftypP, "iso2");
    FourCC(ftypP, (codec == Codec::AV1) ? "av01" : (codec == Codec::HEVC) ? "hvc1" : "avc1");
    FourCC(ftypP, "mp41");
    Bytes ftyp = Box("ftyp", ftypP);
    if (fwrite(ftyp.data(), 1, ftyp.size(), I.f) != ftyp.size()) {
        return I.fail(err, "write ftyp failed");
    }

    // fMP4 的 moov 要在第一個 sample 之後才寫得出來（那時才有 SPS/PPS），
    // 所以這裡只落 ftyp。mdat 也不預留 header —— 每片自己帶。
    if (I.frag) {
        fflush(I.f);
        return true;
    }

    // mdat：先寫 8-byte header，Close() 再回來補 size
    I.mdatStart = _ftelli64(I.f);
    const uint8_t hdr[8] = {0, 0, 0, 0, 'm', 'd', 'a', 't'};
    if (fwrite(hdr, 1, 8, I.f) != 8) return I.fail(err, "write mdat header failed");
    return true;
}

bool Mp4Muxer::WriteSample(const uint8_t* annexb, size_t size, int64_t pts100ns,
                           int64_t dur100ns, bool keyframe, std::string* err) {
    auto& I = *impl_;
    if (!I.f) return I.fail(err, "muxer not open");
    if (!annexb || size == 0) return I.fail(err, "empty access unit");

    const bool av1 = I.codec == Codec::AV1;
    const bool hevc = I.codec == Codec::HEVC;

    Bytes payload;          // 要進 mdat 的位元組（H.264/HEVC 是 AVCC，AV1 是原樣 OBU）
    bool key = keyframe;

    if (av1) {
        key = key || Av1IsKeyFrame(annexb, size);
        if (I.config.empty()) {
            if (!key) {
                return I.fail(err, "first AV1 sample is not a keyframe and no av1C was "
                                   "supplied -- dropping it would produce a file that "
                                   "cannot start decoding");
            }
            Av1SeqInfo si;
            if (!BuildAv1CFromObus(annexb, size, I.config, &si)) {
                return I.fail(err, "no sequence header OBU in the first keyframe -- "
                                   "cannot build av1C");
            }
            // sequence header 裡有 max_frame_width/height。拿它跟實際設定對一次，
            // 是唯一能免費抓到「bit 讀錯位」的方法 —— av1C 的欄位錯位不會讓
            // decoder 報錯（它自己會重讀 sequence header），只會讓檔案在別的
            // 工具裡看起來很奇怪。
            if (si.width > 0 && si.height > 0 &&
                (si.width != I.width || si.height != I.height)) {
                fprintf(stderr, "[mux] WARNING: av1C says %dx%d but the encoder was "
                                "configured for %dx%d -- sequence header parse is "
                                "probably misaligned\n",
                        si.width, si.height, I.width, I.height);
            }
        }
        payload.assign(annexb, annexb + size);
    } else {
        std::vector<Nal> nals;
        if (!SplitAnnexB(annexb, size, hevc, nals)) {
            return I.fail(err, "access unit has no Annex-B start code");
        }

        for (const auto& n : nals) if (IsKeyNal(n.type, hevc)) key = true;

        if (I.config.empty()) {
            if (!key) {
                return I.fail(err, "first sample is not a keyframe and no config record "
                                   "was supplied -- dropping it would produce a file "
                                   "that cannot start decoding");
            }
            const bool ok = hevc ? BuildHvcC(nals, I.config) : BuildAvcC(nals, I.config);
            if (!ok) return I.fail(err, "cannot build " + std::string(hevc ? "hvcC" : "avcC") +
                                            " from in-band parameter sets");
        }
        ToAvcc(nals, payload);
    }

    if (I.frag) {
        // 第一個 sample 進來時才寫 moov（此時 config 一定已經有了）。
        // 時長先寫 0：分片檔的時長本來就是「由分片累加而得」，寫 0 是合法值，
        // 而且 Close() 會回填成真實值（大小不變，可以原地覆寫）。
        if (!I.moovWritten) {
            I.moovStart = _ftelli64(I.f);
            Bytes moov = I.BuildMoovFragmented(0);
            if (fwrite(moov.data(), 1, moov.size(), I.f) != moov.size()) {
                return I.fail(err, "write moov failed");
            }
            I.moovSize = (int64_t)moov.size();
            I.moovWritten = true;
        }

        // 分片邊界對齊 GOP：遇到關鍵幀就把上一片收掉。這樣每片都能獨立解碼，
        // 被 kill 時損失的上限就是「一個 GOP」，而不是「整段錄影」。
        if (key && !I.fragSamples.empty()) {
            if (!I.WriteFragment(err)) return false;
        }

        if (!I.haveFirstPts) { I.firstPts100 = pts100ns; I.haveFirstPts = true; }

        Impl::Sample s;
        s.off = 0;                       // fragmented 不用絕對偏移
        s.size = (uint32_t)payload.size();
        s.pts = pts100ns - I.firstPts100;
        s.dur = dur100ns;
        s.sync = key;
        I.fragSamples.push_back(s);
        Append(I.fragData, payload);
        I.payloadBytes += payload.size();
        ++I.totalSamples;
        return true;
    }

    Impl::Sample s;
    s.off = (uint64_t)_ftelli64(I.f);
    s.size = (uint32_t)payload.size();
    s.pts = pts100ns;
    s.dur = dur100ns;
    s.sync = key;

    if (fwrite(payload.data(), 1, payload.size(), I.f) != payload.size()) {
        return I.fail(err, "write mdat payload failed");
    }
    I.payloadBytes += payload.size();
    I.samples.push_back(s);
    ++I.totalSamples;
    return true;
}

bool Mp4Muxer::SetAudioStartOffset(int track, int64_t offset100ns, std::string* err) {
    auto& I = *impl_;
    if (track < 0 || (size_t)track >= I.aud.size()) return I.fail(err, "no such audio track");

    Impl::AudioTrack& a = I.aud[(size_t)track];
    const int64_t ticks = (offset100ns * (int64_t)a.rate + (offset100ns >= 0 ? 5000000 : -5000000)) /
                          10000000;

    // fMP4 的延遲是靠第一個 tfdt 表達的，所以累加器的起點要跟著改。
    // 累加器一旦往前跑過，tfdt 就已經寫死在檔案裡，再改會跟後面的分片接不上
    // （症狀是這條軌後面每一片的 baseMediaDecodeTime 都少一段，聲音越播越早）。
    // progressive 的 moov 是 Close 才寫的，在那之前改都還來得及。
    if (a.nextDts != a.startTicks) {
        return I.fail(err, "audio samples already written; start offset is fixed");
    }

    // 正負兩種情況走不同的表達方式（見 AudioTrack::trimTicks 的說明）：
    //   ticks > 0（音訊偏晚）→ startTicks = ticks，empty edit 往後推
    //   ticks < 0（音訊偏早）→ trimTicks = −ticks，裁掉開頭；tfdt 仍然是 0
    if (ticks >= 0) {
        a.startTicks = ticks;
        a.trimTicks = 0;
    } else {
        a.startTicks = 0;
        a.trimTicks = -ticks;
    }
    a.nextDts = a.startTicks;
    return true;
}

bool Mp4Muxer::WriteAudioSample(int track, const uint8_t* data, size_t size, uint32_t frames,
                                int64_t pts100ns, std::string* err) {
    auto& I = *impl_;
    if (!I.f) return I.fail(err, "muxer not open");
    if (track < 0 || (size_t)track >= I.aud.size()) return I.fail(err, "no such audio track");
    if (!data || size == 0 || frames == 0) return I.fail(err, "empty audio access unit");

    Impl::AudioTrack& a = I.aud[(size_t)track];

    // 時間軸用取樣點累加，不用 pts100ns：音軌 timescale = 取樣率，所以 AAC-LC
    // 每個 AU 剛好 1024 個 tick，stts 只有一個 run。pts100ns 只決定起點。
    if (!a.started) {
        int64_t start = (pts100ns * (int64_t)a.rate + 5000000) / 10000000;
        a.startTs = start > 0 ? start : 0;
        a.ts = a.startTs;
        a.started = true;
    }

    // 負偏移的裁切。progressive 交給 elst 的 media_time（資料不丟，精確到 tick），
    // fMP4 沒有 elst，只能真的丟掉開頭的 AU。
    //
    // 取整方向是 floor，不是 ceil：只丟「整塊都落在裁切區內」的 AU。
    //   用 ceil 會多丟一個 AU，而多丟的方向是危險的 —— 真實偏移只有 9ms 時，
    //   ceil 丟掉 23ms，補償後變成 −14ms，比不補償（+9ms）離對齊更遠。
    //   floor 在同樣情況下丟 0 個，等於退回「不補償」，方向是安全的。
    //
    // 精度：AU 是 1024 個取樣點（44.1k 下 23.2ms、48k 下 21.3ms），所以 fMP4 的
    // 負偏移只能表達到大於一個 AU 的量，殘差最多一個 AU。要更細就得在 PCM 層
    // 裁 —— 那時偏移還沒量到（要等第一個 AU 產生才算得出來），雞生蛋。
    if (I.frag && a.trimTicks > 0) {
        const int64_t rel = a.ts - a.startTs;
        if (rel + (int64_t)frames <= a.trimTicks) {
            a.ts += (int64_t)frames;      // 時間軸照樣前進，但不寫進容器
            return true;
        }
    }

    Impl::Sample s;
    s.off = 0;
    s.size = (uint32_t)size;
    s.pts = a.ts;
    s.dur = (int64_t)frames;
    s.sync = true;
    a.ts += (int64_t)frames;

    Bytes payload(data, data + size);

    if (I.frag) {
        // moov 一定要先寫（那是分片檔的樣板）。實務上第一筆一定是影像，
        // 這裡只是防呆：真的先來音訊就先攢著，等影像把 moov 寫出來。
        a.frag.push_back(s);
        Append(a.fragData, payload);
        I.payloadBytes += payload.size();
        ++a.total;

        // 影像停住（畫面完全不動）時不會有分片邊界，音訊不能跟著無限累積 ——
        // 超過 3 秒就自己落一片。這片的 traf 只有音軌，tfdt 照樣接得下去。
        //
        // 多條音軌時任一條滿了就落一片：各條的 AU 數量不會差太多（同一批時間），
        // 但取樣率不同時 3 秒對應的 AU 數也不同，所以逐條判斷。
        if (I.moovWritten && I.fragSamples.empty() &&
            a.frag.size() >= (size_t)((int64_t)a.rate * 3 / 1024)) {
            if (!I.WriteFragment(err)) return false;
        }
        return true;
    }

    s.off = (uint64_t)_ftelli64(I.f);
    if (fwrite(payload.data(), 1, payload.size(), I.f) != payload.size()) {
        return I.fail(err, "write audio payload failed");
    }
    I.payloadBytes += payload.size();
    a.samples.push_back(s);
    ++a.total;
    return true;
}

bool Mp4Muxer::Close(std::string* err) {
    auto& I = *impl_;
    if (!I.f) return true;

    if (I.frag) {
        if (!I.WriteFragment(err)) {
            fclose(I.f);
            I.f = nullptr;
            return false;
        }
        // 回填時長。moov 的欄位全是定長 u32，所以重寫後大小不變，可以原地覆寫
        // —— 這樣「正常關檔」的檔案有正確時長，「被 kill」的檔案則停在 0
        // （播放器會用分片累加，仍然能播）。
        if (I.moovWritten && I.moovSize > 0) {
            Bytes moov = I.BuildMoovFragmented(I.nextDts);
            const int64_t end = _ftelli64(I.f);
            if (moov.size() == (size_t)I.moovSize &&
                _fseeki64(I.f, I.moovStart, SEEK_SET) == 0) {
                fwrite(moov.data(), 1, moov.size(), I.f);
                _fseeki64(I.f, end, SEEK_SET);
            }
        }
        fflush(I.f);
        fclose(I.f);
        I.f = nullptr;
        return true;
    }

    // 補 mdat size
    const int64_t end = _ftelli64(I.f);
    const int64_t mdatSize = end - I.mdatStart;
    if (mdatSize > 0xFFFFFFFFLL) {
        fclose(I.f);
        I.f = nullptr;
        return I.fail(err, "mdat exceeds the 4 GB box limit -- lower the bitrate or "
                           "split the recording");
    }
    uint8_t hdr[8];
    hdr[0] = (uint8_t)(mdatSize >> 24); hdr[1] = (uint8_t)(mdatSize >> 16);
    hdr[2] = (uint8_t)(mdatSize >> 8);  hdr[3] = (uint8_t)mdatSize;
    memcpy(hdr + 4, "mdat", 4);
    if (_fseeki64(I.f, I.mdatStart, SEEK_SET) != 0 ||
        fwrite(hdr, 1, 8, I.f) != 8 ||
        _fseeki64(I.f, end, SEEK_SET) != 0) {
        fclose(I.f);
        I.f = nullptr;
        return I.fail(err, "cannot patch mdat size");
    }

    Bytes moov = I.BuildMoov();
    if (fwrite(moov.data(), 1, moov.size(), I.f) != moov.size()) {
        fclose(I.f);
        I.f = nullptr;
        return I.fail(err, "write moov failed");
    }
    fflush(I.f);
    fclose(I.f);
    I.f = nullptr;
    return true;
}

uint64_t Mp4Muxer::bytes() const {
    if (!impl_->f) return 0;
    const int64_t p = _ftelli64(impl_->f);
    return p > 0 ? (uint64_t)p : 0;
}

uint64_t Mp4Muxer::samples() const { return impl_->totalSamples; }
uint64_t Mp4Muxer::audioSamples() const {
    uint64_t n = 0;
    for (const auto& a : impl_->aud) n += a.total;
    return n;
}
int Mp4Muxer::audioTrackCount() const { return (int)impl_->aud.size(); }
bool Mp4Muxer::hasAudio() const { return !impl_->aud.empty(); }
uint64_t Mp4Muxer::payloadBytes() const { return impl_->payloadBytes; }
bool Mp4Muxer::hasConfig() const { return !impl_->config.empty(); }

} // namespace capwgc
