// QQ Music English add-on, loaded as a version.dll proxy next to GF.dll.
//
// GF.dll draws all UI text with DirectWrite: every label, button and list cell
// becomes an IDWriteTextLayout made by IDWriteFactory::CreateTextLayout, and that
// one layout is used both to measure and to draw. We patch that method in
// DirectWrite's shared factory vtable and pass it the English text instead, so
// what gets measured is what gets drawn, and it stays English on every redraw.
//
// Translations: %LOCALAPPDATA%\QQMusicEN\strings.tsv  ("chinese<TAB>english" per line, \n escaped)
// Untranslated Chinese that reached the screen: %LOCALAPPDATA%\QQMusicEN\missing.txt
// Startup report: %LOCALAPPDATA%\QQMusicEN\status.txt

#include <windows.h>
#include <shlobj.h>
#include <dwrite.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>

// Forward every real version.dll export to the system copy.
#define FWD(n) __pragma(comment(linker, "/export:" #n "=C:\\Windows\\System32\\version." #n))
FWD(GetFileVersionInfoA) FWD(GetFileVersionInfoByHandle) FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW) FWD(GetFileVersionInfoSizeA) FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW) FWD(GetFileVersionInfoSizeW) FWD(GetFileVersionInfoW)
FWD(VerFindFileA) FWD(VerFindFileW) FWD(VerInstallFileA) FWD(VerInstallFileW)
FWD(VerLanguageNameA) FWD(VerLanguageNameW) FWD(VerQueryValueA) FWD(VerQueryValueW)

namespace {

// IDWriteFactory vtable slots (IUnknown 0-2, then the interface in declaration order).
constexpr int kCreateTextLayoutSlot = 18;
constexpr int kCreateGdiCompatibleTextLayoutSlot = 19;

using CreateTextLayoutFn = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*, const WCHAR*, UINT32, IDWriteTextFormat*,
                                                       FLOAT, FLOAT, IDWriteTextLayout**);
using CreateGdiLayoutFn = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*, const WCHAR*, UINT32, IDWriteTextFormat*,
                                                      FLOAT, FLOAT, FLOAT, const DWRITE_MATRIX*, BOOL,
                                                      IDWriteTextLayout**);
CreateTextLayoutFn g_createLayout;
CreateGdiLayoutFn g_createGdiLayout;

// A table entry with printf fields ("共%d首歌曲"), matched against whole strings.
struct Template {
    std::vector<std::wstring> lits;  // literal text around the fields: lits.size() == fields.size() + 1
    std::wstring fields;             // 'd' (number) or 's' (any text) per field
    std::wstring en;                 // English with the same number of fields
};

CRITICAL_SECTION g_lock;
std::unordered_map<std::wstring, std::wstring> g_table;
std::vector<Template> g_templates;
std::unordered_map<std::wstring, std::wstring> g_composed;  // cache of strings translated by parts
std::unordered_set<std::wstring> g_untranslated;            // negative cache, also dedupes the log
std::wstring g_dir;
volatile LONG g_seen, g_translated;
HANDLE g_ready;  // set once the table is loaded (or failed to load)

bool IsHan(wchar_t c) { return c >= 0x3400 && c <= 0x9fff; }
bool IsCjk(wchar_t c) { return (c >= 0x3000 && c <= 0x9fff) || (c >= 0xff00 && c <= 0xffef); }
// QQ Music's icon font puts its icons on ordinary hanzi code points.
bool IsIconGlyph(const std::wstring& s) { return s.size() == 1 && s[0] >= 0x4f00 && s[0] <= 0x52ff; }

void Append(const wchar_t* file, const std::wstring& line) {
    // The logs only help with adding translations; keep them from growing without end.
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW((g_dir + L"\\" + file).c_str(), GetFileExInfoStandard, &info) &&
        (info.nFileSizeHigh || info.nFileSizeLow > 2 * 1024 * 1024))
        return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW((g_dir + L"\\" + file).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    CloseHandle(f);
}

}  // namespace

void Status(const std::wstring& msg) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    wchar_t head[MAX_PATH + 32];
    wsprintfW(head, L"[%s %lu] ", name ? name + 1 : exe, GetCurrentProcessId());
    Append(L"status.txt", head + msg + L"\r\n");
}

namespace webbridge {
void EarlyHook(const std::wstring& dir);  // webbridge.cpp
void Start();
}
namespace native {
void Start();  // native.cpp
}
namespace iat {
int PatchAll();  // iat.cpp
}

namespace {

void LogMissing(const std::wstring& s) {
    std::wstring line;
    for (wchar_t c : s) line += c == L'\n' ? std::wstring(L"\\n") : c == L'\t' ? std::wstring(L"\\t") : std::wstring(1, c);
    Append(L"missing.txt", line + L"\r\n");
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            out += n == L'n' ? L'\n' : n == L't' ? L'\t' : n == L'r' ? L'\r' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Splits printf text into literals and fields. Returns false for anything but %d/%u/%ld/%s-style fields.
bool ParseFormat(const std::wstring& s, std::vector<std::wstring>& lits, std::wstring& fields) {
    lits.assign(1, L"");
    fields.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'%') { lits.back() += s[i]; continue; }
        if (i + 1 < s.size() && s[i + 1] == L'%') { lits.back() += L'%'; ++i; continue; }
        size_t j = i + 1;
        while (j < s.size() && (iswdigit(s[j]) || s[j] == L'.' || s[j] == L'-' || s[j] == L'l' || s[j] == L'I'))
            ++j;
        if (j >= s.size()) return false;
        wchar_t t = s[j];
        if (t == L'd' || t == L'u' || t == L'i') fields += L'd';
        else if (t == L's' || t == L'S') fields += L's';
        else return false;
        lits.emplace_back();
        i = j;
    }
    return true;
}

void AddTemplate(const std::wstring& zh, const std::wstring& en) {
    Template t;
    if (!ParseFormat(zh, t.lits, t.fields) || t.fields.empty()) return;
    bool han = false;
    for (auto& l : t.lits)
        for (wchar_t c : l) han |= IsHan(c);
    if (!han) return;  // a bare "%s" would match anything
    std::vector<std::wstring> enLits;
    std::wstring enFields;
    if (!ParseFormat(en, enLits, enFields) || enFields.size() != t.fields.size()) return;
    t.en = en;
    g_templates.push_back(std::move(t));
}

const wchar_t* LoadTable() {
    HANDLE f = CreateFileW((g_dir + L"\\strings.tsv").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"cannot open strings.tsv";
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::string raw(size, '\0');
    ReadFile(f, raw.data(), size, &got, nullptr);
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)got, nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)got, text.data(), n);
    size_t pos = text.size() && text[0] == 0xfeff ? 1 : 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos) {
            std::wstring zh = Unescape(line.substr(0, tab)), en = Unescape(line.substr(tab + 1));
            if (zh.find(L'%') != std::wstring::npos) AddTemplate(zh, en);
            g_table[zh] = en;
        }
        pos = end + 1;
    }
    return g_table.empty() ? L"strings.tsv has no entries" : nullptr;
}

// Matches s[i..] against lits[k..] with fields; fills caps. Backtracks over field lengths.
bool MatchFrom(const Template& t, const std::wstring& s, size_t i, size_t k, std::vector<std::wstring>& caps) {
    const std::wstring& lit = t.lits[k];
    if (s.compare(i, lit.size(), lit) != 0) return false;
    i += lit.size();
    if (k == t.fields.size()) return i == s.size();
    wchar_t kind = t.fields[k];
    size_t maxLen = s.size() - i;
    for (size_t len = 1; len <= maxLen; ++len) {
        wchar_t c = s[i + len - 1];
        if (kind == L'd' && !(iswdigit(c) || (len == 1 && c == L'-'))) break;
        caps.push_back(s.substr(i, len));
        if (MatchFrom(t, s, i + len, k + 1, caps)) return true;
        caps.pop_back();
    }
    return false;
}

std::wstring Fill(const std::wstring& en, const std::vector<std::wstring>& caps) {
    std::wstring out;
    size_t f = 0;
    for (size_t i = 0; i < en.size(); ++i) {
        if (en[i] != L'%') { out += en[i]; continue; }
        if (i + 1 < en.size() && en[i + 1] == L'%') { out += L'%'; ++i; continue; }
        size_t j = i + 1;
        while (j < en.size() && !iswalpha(en[j]) ) ++j;
        while (j < en.size() && (en[j] == L'l' || en[j] == L'I')) ++j;
        if (f < caps.size()) {
            auto it = g_table.find(caps[f]);  // a Chinese name inside a sentence, if we know it
            out += it != g_table.end() ? it->second : caps[f];
        }
        ++f;
        i = j;
    }
    return out;
}

bool MatchTemplates(const std::wstring& s, std::wstring& out) {
    for (const Template& t : g_templates) {
        std::vector<std::wstring> caps;
        if (MatchFrom(t, s, 0, 0, caps)) { out = Fill(t.en, caps); return true; }
    }
    return false;
}

// Chinese counts use 万 (10^4) and 亿 (10^8): "2494.0万" -> "24.9M", "1.5亿" -> "150M".
std::wstring Numbers(const std::wstring& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        while (j < s.size() && (iswdigit(s[j]) || (s[j] == L'.' && j > i))) ++j;
        if (j > i && iswdigit(s[j - 1]) && j < s.size() && (s[j] == L'万' || s[j] == L'亿')) {
            double v = _wtof(s.substr(i, j - i).c_str()) * (s[j] == L'万' ? 1e4 : 1e8);
            const wchar_t* unit = v >= 1e9 ? L"B" : v >= 1e6 ? L"M" : L"K";
            double scaled = v / (v >= 1e9 ? 1e9 : v >= 1e6 ? 1e6 : 1e3);
            wchar_t buf[32];
            swprintf(buf, 32, scaled >= 100 || scaled == (int)scaled ? L"%.0f%s" : L"%.1f%s", scaled, unit);
            out += buf;
            i = j + 1;
        } else if (j > i) {
            out += s.substr(i, j - i);
            i = j;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// Translates each run of Chinese separately ("歌曲12" -> "Songs 12"), as long as every run has a translation.
bool Compose(const std::wstring& input, std::wstring& out) {
    std::wstring s = Numbers(input);
    out.clear();
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        bool cjk = IsCjk(s[i]);
        while (j < s.size() && IsCjk(s[j]) == cjk) ++j;
        std::wstring run = s.substr(i, j - i);
        if (cjk) {
            // Leading/trailing full-width punctuation belongs to the run but not to the table key.
            size_t a = 0, b = run.size();
            while (a < b && !IsHan(run[a])) ++a;
            while (b > a && !IsHan(run[b - 1])) --b;
            auto it = a < b ? g_table.find(run.substr(a, b - a)) : g_table.end();
            if (a < b && it == g_table.end()) return false;
            out += run.substr(0, a);
            if (a < b) {
                if (!out.empty() && iswalnum(out.back())) out += L' ';
                out += it->second;
                if (b == run.size() && j < s.size() && iswalnum(s[j])) out += L' ';
            }
            out += run.substr(b);
        } else {
            out += run;
        }
        i = j;
    }
    return true;
}

// Returns the English for text, or nullptr. The pointer stays valid (map nodes are stable).
const std::wstring* Translate(const wchar_t* text, size_t n) {
    // Single characters are icon-font glyphs or karaoke lyrics, which are drawn one character at a time.
    if (!text || n < 2) return nullptr;
    // The first layouts can arrive while the table is still loading; they are worth a short wait.
    if (WaitForSingleObject(g_ready, 3000) != WAIT_OBJECT_0 || g_table.empty()) return nullptr;
    bool any = false;
    for (size_t i = 0; i < n; ++i)
        if (IsHan(text[i])) { any = true; break; }
    if (!any) return nullptr;
    std::wstring s(text, n);
    const std::wstring* result = nullptr;
    EnterCriticalSection(&g_lock);
    if (auto it = g_table.find(s); it != g_table.end()) {
        result = &it->second;
    } else if (auto c = g_composed.find(s); c != g_composed.end()) {
        result = &c->second;
    } else if (!g_untranslated.count(s)) {
        std::wstring out;
        if (MatchTemplates(s, out) || Compose(s, out)) {
            result = &g_composed.emplace(s, out).first->second;
        } else {
            g_untranslated.insert(s);
            if (!IsIconGlyph(s)) LogMissing(s);
        }
    }
    LeaveCriticalSection(&g_lock);
    return result;
}

// English is often wider than the Chinese a label was sized for. If the translated text
// needs more room than the Chinese did (the box GF gave it, or the Chinese's own size when
// that already spilled over the box), shrink its font just enough, down to kMinScale; what
// still does not fit is logged to overflow.txt so the translation can be shortened.
constexpr float kMinScale = 0.72f;
constexpr float kMaxBox = 4000;  // bigger boxes are measuring passes, not real boxes

template <class MakeZh>
void Fit(IDWriteTextLayout* layout, const WCHAR* zh, UINT32 zhLen, const std::wstring& en, FLOAT w, FLOAT h,
         MakeZh makeZh) {
    if (!layout || w <= 0 || w > kMaxBox) return;
    DWRITE_TEXT_METRICS m;
    if (FAILED(layout->GetMetrics(&m))) return;
    bool wraps = layout->GetWordWrapping() != DWRITE_WORD_WRAPPING_NO_WRAP;
    float need;  // how much the text must shrink: < 1 means it overflows
    bool line = !wraps || m.lineCount <= 1 && m.widthIncludingTrailingWhitespace > w + 0.5f;
    if (line ? m.widthIncludingTrailingWhitespace <= w + 0.5f : !(h > 0 && h <= kMaxBox && m.height > h + 0.5f))
        return;
    // It overflows the box. Did the Chinese fit? If not, GF lets text spill over here.
    DWRITE_TEXT_METRICS zm = {};
    IDWriteTextLayout* zl = makeZh();
    if (!zl) return;
    HRESULT hr = zl->GetMetrics(&zm);
    zl->Release();
    if (FAILED(hr)) return;
    if (line) {
        float room = zm.widthIncludingTrailingWhitespace > w ? zm.widthIncludingTrailingWhitespace : w;
        need = room / m.widthIncludingTrailingWhitespace;
    } else {
        float room = zm.height > h ? zm.height : h;
        need = sqrtf(room / m.height);  // wrapped text shrinks in both directions
    }
    if (need >= 1) return;
    float size = layout->GetFontSize();
    float scale = need < kMinScale ? kMinScale : need * 0.98f;
    DWRITE_TEXT_RANGE all = {0, (UINT32)en.size()};
    layout->SetFontSize(size * scale, all);
    if (need < kMinScale) {
        wchar_t buf[64];
        swprintf(buf, 64, L"\t%.0f%%\t", need * 100);
        Append(L"overflow.txt", std::wstring(zh, zhLen) + buf + en + L"\r\n");
    }
}

HRESULT STDMETHODCALLTYPE HookCreateTextLayout(IDWriteFactory* f, const WCHAR* s, UINT32 n, IDWriteTextFormat* fmt,
                                               FLOAT w, FLOAT h, IDWriteTextLayout** out) {
    InterlockedIncrement(&g_seen);
    if (const std::wstring* en = Translate(s, n)) {
        InterlockedIncrement(&g_translated);
        HRESULT hr = g_createLayout(f, en->c_str(), (UINT32)en->size(), fmt, w, h, out);
        if (SUCCEEDED(hr) && out) Fit(*out, s, n, *en, w, h, [&] {
            IDWriteTextLayout* zl = nullptr;
            return SUCCEEDED(g_createLayout(f, s, n, fmt, w, h, &zl)) ? zl : nullptr;
        });
        return hr;
    }
    return g_createLayout(f, s, n, fmt, w, h, out);
}

HRESULT STDMETHODCALLTYPE HookCreateGdiLayout(IDWriteFactory* f, const WCHAR* s, UINT32 n, IDWriteTextFormat* fmt,
                                              FLOAT w, FLOAT h, FLOAT ppd, const DWRITE_MATRIX* m, BOOL natural,
                                              IDWriteTextLayout** out) {
    InterlockedIncrement(&g_seen);
    if (const std::wstring* en = Translate(s, n)) {
        InterlockedIncrement(&g_translated);
        HRESULT hr = g_createGdiLayout(f, en->c_str(), (UINT32)en->size(), fmt, w, h, ppd, m, natural, out);
        if (SUCCEEDED(hr) && out) Fit(*out, s, n, *en, w, h, [&] {
            IDWriteTextLayout* zl = nullptr;
            return SUCCEEDED(g_createGdiLayout(f, s, n, fmt, w, h, ppd, m, natural, &zl)) ? zl : nullptr;
        });
        return hr;
    }
    return g_createGdiLayout(f, s, n, fmt, w, h, ppd, m, natural, out);
}

bool PatchSlot(void* obj, int slot, void* hook, void** orig) {
    void** vt = *(void***)obj;
    if (vt[slot] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[slot];
    InterlockedExchangePointer(&vt[slot], hook);
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

// Every DirectWrite factory in the process shares this vtable, so patching it through
// a factory of our own also covers the ones GF.dll creates.
bool HookDirectWrite() {
    auto create = (decltype(&DWriteCreateFactory))GetProcAddress(LoadLibraryW(L"dwrite.dll"), "DWriteCreateFactory");
    IDWriteFactory* f = nullptr;
    if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&f))) return false;
    bool a = PatchSlot(f, kCreateTextLayoutSlot, (void*)&HookCreateTextLayout, (void**)&g_createLayout);
    bool b = PatchSlot(f, kCreateGdiCompatibleTextLayoutSlot, (void*)&HookCreateGdiLayout, (void**)&g_createGdiLayout);
    // Isolated factories are a separate object; make sure they share the vtable (they do on Windows 10/11).
    IDWriteFactory* iso = nullptr;
    if (SUCCEEDED(create(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), (IUnknown**)&iso))) {
        if (*(void**)iso != *(void**)f) {
            PatchSlot(iso, kCreateTextLayoutSlot, (void*)&HookCreateTextLayout, (void**)&g_createLayout);
            PatchSlot(iso, kCreateGdiCompatibleTextLayoutSlot, (void*)&HookCreateGdiLayout, (void**)&g_createGdiLayout);
        }
        iso->Release();
    }
    return a && b;  // the shared factory stays alive for the life of the process
}

// Runs once the loader is done (threads started in DllMain wait for it).
DWORD WINAPI InstallThread(void*) {
    // Hook first: GF.dll starts laying out the main window right away.
    bool ok = HookDirectWrite();
    webbridge::Start();
    native::Start();
    int patched = iat::PatchAll();
    const wchar_t* why = LoadTable();
    SetEvent(g_ready);
    wchar_t msg[160];
    if (why) {
        wsprintfW(msg, L"%s (error %lu)", why, GetLastError());
    } else {
        wsprintfW(msg, L"%u strings, %u templates; DirectWrite hook %s", (unsigned)g_table.size(),
                  (unsigned)g_templates.size(), ok ? L"on" : L"FAILED");
    }
    Status(msg);
    wsprintfW(msg, L"%d imports hooked", patched);
    Status(msg);
    if (why || !ok) return 0;
    // Modules keep loading after start (some only when a feature is first used).
    for (int i = 0; i < 600; ++i) {
        Sleep(500);
        int now = iat::PatchAll();
        if (now != patched) {
            wsprintfW(msg, L"%d imports hooked", now);
            Status(msg);
            patched = now;
        }
        if (i == 119) {
            wsprintfW(msg, L"first minute: %ld text layouts, %ld translated", g_seen, g_translated);
            Status(msg);
        }
    }
    wsprintfW(msg, L"first five minutes: %ld text layouts, %ld translated", g_seen, g_translated);
    Status(msg);
    return 0;
}

void Install() {
    wchar_t* local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return;
    g_dir = std::wstring(local) + L"\\QQMusicEN";
    CoTaskMemFree(local);
    CreateDirectoryW(g_dir.c_str(), nullptr);
    InitializeCriticalSection(&g_lock);
    // The web pages' browser is started by QQMusic.exe early on; hook process creation now.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (name && _wcsicmp(name + 1, L"QQMusic.exe") == 0) webbridge::EarlyHook(g_dir);
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace

const std::wstring* TranslateText(const wchar_t* text, size_t n) { return Translate(text, n); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Install();
    }
    return TRUE;
}
