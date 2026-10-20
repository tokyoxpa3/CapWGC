// ============================================================================
// i18n.cpp — 語系檔載入與查表
// ============================================================================
//
// 只做三件事：找檔案、解析（平的 JSON 物件）、查表。沒有格式化、沒有複數
// 規則、沒有 RTL —— 這支程式的介面是托盤選單與幾個小視窗，用不到那些。
// 需要參數的字串一律寫成完整句子放進語系檔（`"%d × %d 像素，已複製到剪貼簿"`），
// 不把句子切成片段再拼起來：片段的順序與格位每種語言都不一樣，拼起來的結果
// 只會是每種語言都怪。
// ============================================================================

#include "i18n.h"

#include "settings.h"   // Settings::IniPath

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <vector>

namespace app {
namespace i18n {
namespace {

// 順序就是 NetRedirector 的 SUPPORTED_LANGS / LANG_NAMES，不要重排：
// 使用者（與翻譯）已經習慣這個順序，而且兩支程式的選單長得一樣比較好找。
const Lang kLangs[] = {
    {L"zh_TW", L"繁體中文"},
    {L"zh_CN", L"简体中文"},
    {L"en_US", L"English"},
    {L"es_ES", L"Español"},
    {L"ko_KR", L"한국어"},
    {L"ja_JP", L"日本語"},
    {L"pt_BR", L"Português (BR)"},
    {L"fr_FR", L"Français"},
    {L"de_DE", L"Deutsch"},
    {L"ru_RU", L"Русский"},
    {L"it_IT", L"Italiano"},
    {L"vi_VN", L"Tiếng Việt"},
    {L"th_TH", L"ไทย"},
    {L"id_ID", L"Bahasa Indonesia"},
    {L"tr_TR", L"Türkçe"},
    {L"pl_PL", L"Polski"},
    {L"nl_NL", L"Nederlands"},
};
constexpr int kLangCount = (int)(sizeof(kLangs) / sizeof(kLangs[0]));

// std::less<> ＝ 透明比較子：find(L"…") 不必先建一個 std::wstring，
// 也就不會在每次重繪時配置記憶體（Toolbar::Paint 會逐格查表）。
std::map<std::wstring, std::wstring, std::less<>> g_table;
std::wstring g_code = L"zh_TW";
std::wstring g_note;
std::wstring g_dir;

// ---- UTF-8 → UTF-16 --------------------------------------------------------

void AppendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---- 最小 JSON 解析（只認 {"字串": "字串", …}）-----------------------------
//
// 刻意不寫通用 JSON：語系檔就是一個平的物件，多寫的型別（陣列、數字、巢狀）
// 只會變成沒人走過的分支。遇到不認得的東西就整份放棄、顯示原文 —— 壞掉的
// 翻譯檔不該讓程式起不來，也不該只套用一半（那會出現中英夾雜的介面）。
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && IsSpace(s[i])) ++i;
}

bool Hex4(const std::string& s, size_t& i, unsigned* out) {
    if (i + 4 > s.size()) return false;
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
        const char c = s[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return false;
    }
    i += 4;
    *out = v;
    return true;
}

bool ParseString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= s.size()) return false;
        const char e = s[i++];
        switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            unsigned cp = 0;
            if (!Hex4(s, i, &cp)) return false;
            // 代理對：\uD83D\uDE00 這種兩個一組的，要合成一個字元。
            // （翻譯檔不該出現 emoji，但手改的人會，而解析錯會整份壞掉。）
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' &&
                s[i + 1] == 'u') {
                size_t j = i + 2;
                unsigned lo = 0;
                if (Hex4(s, j, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i = j;
                }
            }
            AppendUtf8(out, cp);
            break;
        }
        default:
            return false;
        }
    }
    return false;   // 沒收尾的引號
}

bool ParseFlatObject(const std::string& s, std::map<std::wstring, std::wstring, std::less<>>* out) {
    size_t i = 0;
    // UTF-8 BOM：Windows 上的編輯器很愛加，加了之後第一個字元不是 '{'。
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        i = 3;
    }
    SkipWs(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    SkipWs(s, i);
    if (i < s.size() && s[i] == '}') return true;   // 空物件（＝全部顯示原文）

    for (;;) {
        SkipWs(s, i);
        std::string k, v;
        if (!ParseString(s, i, k)) return false;
        SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        SkipWs(s, i);
        if (!ParseString(s, i, v)) return false;
        (*out)[Utf8ToWide(k)] = Utf8ToWide(v);

        SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') return true;
        return false;
    }
}

bool ReadFile(const std::wstring& path, std::string* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, (size_t)n);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}

} // namespace

const Lang* Languages() { return kLangs; }
int LanguageCount() { return kLangCount; }

const Lang* Find(const wchar_t* code) {
    if (!code || !*code) return nullptr;
    for (const auto& l : kLangs) {
        if (!wcscmp(l.code, code)) return &l;
    }
    return nullptr;
}

int IndexOf(const wchar_t* code) {
    if (!code || !*code) return -1;
    for (int i = 0; i < kLangCount; ++i) {
        if (!wcscmp(kLangs[i].code, code)) return i;
    }
    return -1;
}

const wchar_t* NameOf(const wchar_t* code) {
    const Lang* l = Find(code);
    return l ? l->name : L"";
}

const wchar_t* Detect() {
    // 只看主要語言，不看地區：zh-HK 與 zh-TW 都用繁體，es-MX 用 es_ES 沒有
    // 問題 —— 我們沒有做地區化的用語差異。
    const LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_CHINESE:
        // 只有簡體（含新加坡）走 zh_CN，其餘（繁中、港、澳）走 zh_TW。
        return (SUBLANGID(id) == SUBLANG_CHINESE_SIMPLIFIED ||
                SUBLANGID(id) == SUBLANG_CHINESE_SINGAPORE)
                   ? L"zh_CN"
                   : L"zh_TW";
    case LANG_ENGLISH: return L"en_US";
    case LANG_SPANISH: return L"es_ES";
    case LANG_KOREAN: return L"ko_KR";
    case LANG_JAPANESE: return L"ja_JP";
    case LANG_PORTUGUESE: return L"pt_BR";
    case LANG_FRENCH: return L"fr_FR";
    case LANG_GERMAN: return L"de_DE";
    case LANG_RUSSIAN: return L"ru_RU";
    case LANG_ITALIAN: return L"it_IT";
    case LANG_VIETNAMESE: return L"vi_VN";
    case LANG_THAI: return L"th_TH";
    case LANG_INDONESIAN: return L"id_ID";
    case LANG_TURKISH: return L"tr_TR";
    case LANG_POLISH: return L"pl_PL";
    case LANG_DUTCH: return L"nl_NL";
    default: return L"en_US";
    }
}

const wchar_t* Current() { return g_code.c_str(); }

const std::wstring& LoadNote() { return g_note; }

const std::wstring& LocaleDir() {
    if (g_dir.empty()) g_dir = ExeDir() + L"\\locale";
    return g_dir;
}

bool Load(const wchar_t* code) {
    std::wstring want = (code && *code) ? code : Detect();
    if (IndexOf(want.c_str()) < 0) want = Detect();

    g_code = want;
    g_table.clear();
    g_note.clear();

    const std::wstring path = LocaleDir() + L"\\" + want + L".json";
    std::string raw;
    if (!ReadFile(path, &raw)) {
        // 檔案不在＝用原文。zh_TW 本來就是原文，所以這對 zh_TW 是正常狀態。
        g_note = path + L": not found (source strings)";
        return false;
    }

    std::map<std::wstring, std::wstring, std::less<>> table;
    if (!ParseFlatObject(raw, &table)) {
        g_note = path + L": parse failed (source strings)";
        return false;
    }
    g_table.swap(table);
    g_note = path + L": " + std::to_wstring(g_table.size()) + L" entries";
    return true;
}

void LoadFromIni() {
    wchar_t code[32] = {};
    const std::wstring ini = Settings::IniPath();
    if (!ini.empty()) {
        GetPrivateProfileStringW(L"ui", L"lang", L"", code, 32, ini.c_str());
    }
    Load(code);
}

const wchar_t* T(const wchar_t* src) {
    if (!src || !*src) return src ? src : L"";
    if (g_table.empty()) return src;
    const auto it = g_table.find(src);
    return it == g_table.end() ? src : it->second.c_str();
}

std::wstring Ts(const std::wstring& src) {
    if (g_table.empty()) return src;
    const auto it = g_table.find(src);
    return it == g_table.end() ? src : it->second;
}

std::wstring FmtV(const wchar_t* fmt, va_list ap) {
    if (!fmt) return std::wstring();
    // 先問長度再配置：翻譯字串的長度不是常數，這裡是唯一安全的地方。
    va_list probe;
    va_copy(probe, ap);
    const int n = _vscwprintf(fmt, probe);
    va_end(probe);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n + 1, L'\0');
    vswprintf_s(out.data(), out.size(), fmt, ap);
    out.resize((size_t)n);
    return out;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = FmtV(fmt, ap);
    va_end(ap);
    return s;
}

} // namespace i18n
} // namespace app
