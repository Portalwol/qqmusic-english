// Translates QQ Music's web pages (Home, Music Hall, Radio ...). Those are drawn by an
// embedded Chromium, qmbrowser.exe, that QQMusic.exe starts. We add a DevTools port
// to its command line, then attach to each page and inject web_dict.js + web.js,
// which translate the page text in place and keep doing so as the page changes.
//
// The port is random and bound to 127.0.0.1. Create %LOCALAPPDATA%\QQMusicEN\web.off
// to turn this part off. Untranslated web text: %LOCALAPPDATA%\QQMusicEN\missing_web.txt

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winhttp.lib")

void Status(const std::wstring& msg);  // qqmusic_en.cpp

namespace webbridge {
namespace {

std::wstring g_dir;
std::string g_scriptJson;  // web_dict.js + web.js as a JSON string literal (quotes included)
CRITICAL_SECTION g_lock;
std::vector<int> g_ports;            // one per qmbrowser.exe we started
std::unordered_set<std::wstring> g_attached;  // DevTools target ids

std::string ReadFileUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::string s(size, '\0');
    ReadFile(f, s.data(), size, &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    if (s.size() >= 3 && (BYTE)s[0] == 0xef && (BYTE)s[1] == 0xbb && (BYTE)s[2] == 0xbf) s.erase(0, 3);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// JSON string literal, ASCII only (non-ASCII as \uXXXX), so no encoding can go wrong on the way.
std::string JsonString(const std::wstring& s) {
    std::string out = "\"";
    char buf[8];
    for (wchar_t c : s) {
        if (c == L'"') out += "\\\"";
        else if (c == L'\\') out += "\\\\";
        else if (c == L'\n') out += "\\n";
        else if (c == L'\r') out += "\\r";
        else if (c == L'\t') out += "\\t";
        else if (c < 0x20 || c > 0x7e) { wsprintfA(buf, "\\u%04x", (unsigned)c); out += buf; }
        else out += (char)c;
    }
    return out + "\"";
}

// Decodes the JSON string literal that starts at s[i] (the opening quote).
std::wstring JsonUnstring(const std::string& s, size_t i) {
    std::string raw;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] != '\\') { raw += s[i]; continue; }
        char e = s[++i];
        if (e == 'n') raw += '\n';
        else if (e == 't') raw += '\t';
        else if (e == 'r') raw += '\r';
        else if (e == 'u' && i + 4 < s.size()) {
            unsigned cp = strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            wchar_t w = (wchar_t)cp;
            char u[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, &w, 1, u, sizeof u, nullptr, nullptr);
            raw.append(u, n);
        } else raw += e;
    }
    return Widen(raw);
}

// ---- qmbrowser.exe command line ----

// kernelbase!CreateProcessInternalW: every CreateProcess* call in the process ends up here.
using CreateProcessInternalWFn = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                               BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION,
                                               PHANDLE);
CreateProcessInternalWFn g_createProcess;

BOOL WINAPI HookCreateProcess(HANDLE token, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                              LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir,
                              LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi, PHANDLE newToken) {
    std::wstring line = cmd ? cmd : L"";
    std::wstring target = (app ? std::wstring(app) : L"") + L" " + line;
    for (auto& c : target) c = towlower(c);
    if (target.find(L"qmbrowser.exe") != std::wstring::npos && target.find(L"--type=") == std::wstring::npos &&
        target.find(L"--remote-debugging-port") == std::wstring::npos) {
        int port = 20000 + (int)((GetTickCount() ^ (GetCurrentProcessId() << 7) ^ (DWORD)(ULONG_PTR)&line) % 40000);
        EnterCriticalSection(&g_lock);
        g_ports.push_back(port);
        LeaveCriticalSection(&g_lock);
        line += L" --remote-debugging-port=" + std::to_wstring(port);
        return g_createProcess(token, app, line.data(), pa, ta, inherit, flags, env, dir, si, pi, newToken);
    }
    return g_createProcess(token, app, cmd, pa, ta, inherit, flags, env, dir, si, pi, newToken);
}

// Windows' hot-patch form: "mov edi,edi" at the entry with five int3/nop bytes before it.
// The 5 bytes become "jmp hook" and the entry a short jump back to them; the original
// continues at entry + 2.
bool Hotpatch(void* fn, void* hook, void** orig) {
    BYTE* p = (BYTE*)fn;
    if (!p || p[0] != 0x8b || p[1] != 0xff) return false;
    for (int i = 1; i <= 5; ++i)
        if (p[-i] != 0xcc && p[-i] != 0x90) return false;
    DWORD old;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = p + 2;
    p[-5] = 0xe9;
    *(DWORD*)(p - 4) = (DWORD)hook - (DWORD)p;
    *(volatile WORD*)p = 0xf9eb;  // jmp -7
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return true;
}

// ---- DevTools ----

std::string HttpGet(int port, const wchar_t* path) {
    std::string body;
    HINTERNET s = WinHttpOpen(L"QQMusicEN", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 1000, 1000, 2000, 2000);
    HINTERNET c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)port, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr, 0) : nullptr;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        char buf[8192];
        DWORD got;
        while (WinHttpReadData(r, buf, sizeof buf, &got) && got) body.append(buf, got);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return body;
}

struct Socket {
    HINTERNET session = nullptr, connect = nullptr, ws = nullptr;
    int next = 1;
    ~Socket() {
        if (ws) WinHttpCloseHandle(ws);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
    bool Open(int port, const std::wstring& path) {
        session = WinHttpOpen(L"QQMusicEN", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (!session) return false;
        connect = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)port, 0);
        HINTERNET r = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, nullptr, nullptr, 0) : nullptr;
        if (!r) return false;
        bool ok = WinHttpSetOption(r, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                  WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr);
        if (ok) ws = WinHttpWebSocketCompleteUpgrade(r, 0);
        WinHttpCloseHandle(r);
        return ws != nullptr;
    }
    // Sends one command and returns its response (events that arrive first are skipped).
    std::string Call(const std::string& method, const std::string& params) {
        int id = next++;
        std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\",\"params\":" + params + "}";
        if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, msg.data(), (DWORD)msg.size()) != NO_ERROR)
            return {};
        std::string want = "\"id\":" + std::to_string(id) + ",";
        for (;;) {
            std::string in;
            char buf[16384];
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            do {
                if (WinHttpWebSocketReceive(ws, buf, sizeof buf, &got, &type) != NO_ERROR) return {};
                in.append(buf, got);
            } while (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return {};
            if (in.find(want) != std::string::npos) return in.empty() ? std::string(" ") : in;
        }
    }
};

void AppendMissing(const std::wstring& lines) {
    if (lines.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW((g_dir + L"\\missing_web.txt").c_str(), GetFileExInfoStandard, &info) &&
        (info.nFileSizeHigh || info.nFileSizeLow > 2 * 1024 * 1024))
        return;
    std::wstring text = lines + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), u.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW((g_dir + L"\\missing_web.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

struct Target { int port; std::wstring id, path; };

// Stays attached for the life of the page: the on-load script only lasts as long as the session.
DWORD WINAPI AttachThread(void* p) {
    Target t = *(Target*)p;
    delete (Target*)p;
    Socket s;
    if (s.Open(t.port, t.path)) {
        s.Call("Page.enable", "{}");
        s.Call("Page.addScriptToEvaluateOnLoad", "{\"scriptSource\":" + g_scriptJson + "}");
        bool ok = !s.Call("Runtime.evaluate", "{\"expression\":" + g_scriptJson + "}").empty();
        while (ok) {
            Sleep(4000);
            std::string r = s.Call("Runtime.evaluate",
                                   "{\"expression\":\"window.__qqenTakeMissing?window.__qqenTakeMissing():''\",\"returnByValue\":true}");
            if (r.empty()) break;
            size_t v = r.find("\"value\":\"");
            if (v != std::string::npos) AppendMissing(JsonUnstring(r, v + 8));
        }
    }
    EnterCriticalSection(&g_lock);
    g_attached.erase(t.id);
    LeaveCriticalSection(&g_lock);
    return 0;
}

// Pulls  "key": "value"  out of one JSON object.
std::wstring Field(const std::string& obj, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t i = obj.find(k);
    if (i == std::string::npos) return {};
    i = obj.find('"', obj.find(':', i + k.size()));
    return i == std::string::npos ? std::wstring() : JsonUnstring(obj, i);
}

void PollTargets() {
    std::vector<int> ports;
    EnterCriticalSection(&g_lock);
    ports = g_ports;
    LeaveCriticalSection(&g_lock);
    for (int port : ports) {
        std::string list = HttpGet(port, L"/json");
        for (size_t a = list.find('{'); a != std::string::npos; a = list.find('{', a + 1)) {
            size_t b = list.find('}', a);
            if (b == std::string::npos) break;
            std::string obj = list.substr(a, b - a + 1);
            if (Field(obj, "type") != L"page") continue;
            std::wstring id = Field(obj, "id"), ws = Field(obj, "webSocketDebuggerUrl");
            size_t slash = ws.find(L'/', ws.find(L"://") + 3);
            if (id.empty() || slash == std::wstring::npos) continue;
            EnterCriticalSection(&g_lock);
            bool fresh = g_attached.insert(id).second;
            LeaveCriticalSection(&g_lock);
            if (!fresh) continue;
            if (HANDLE h = CreateThread(nullptr, 0, AttachThread, new Target{port, id, ws.substr(slash)}, 0, nullptr))
                CloseHandle(h);
        }
    }
}

DWORD WINAPI BridgeThread(void*) {
    for (;;) {
        PollTargets();
        Sleep(500);
    }
}

}  // namespace

// From DllMain, before QQMusic.exe gets to start its browser.
void EarlyHook(const std::wstring& dir) {
    g_dir = dir;
    if (GetFileAttributesW((dir + L"\\web.off").c_str()) != INVALID_FILE_ATTRIBUTES) return;
    InitializeCriticalSection(&g_lock);
    Hotpatch((void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateProcessInternalW"),
             (void*)&HookCreateProcess, (void**)&g_createProcess);
}

void Start() {
    if (!g_createProcess) return;
    std::string dict = ReadFileUtf8(g_dir + L"\\web_dict.js"), js = ReadFileUtf8(g_dir + L"\\web.js");
    if (dict.empty() || js.empty()) { Status(L"web pages: web_dict.js or web.js missing, skipped"); return; }
    g_scriptJson = JsonString(Widen(dict + "\n" + js));
    if (HANDLE t = CreateThread(nullptr, 0, BridgeThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace webbridge
