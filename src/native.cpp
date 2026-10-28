// Translates QQ Music's few native Windows dialogs: message boxes (hooked before they
// are sized), window titles set in code, and classic dialogs, whose controls are
// translated when the dialog opens (a WinEvent hook limited to this process).

#include <windows.h>
#include <string>

const std::wstring* TranslateText(const wchar_t* text, size_t n);  // qqmusic_en.cpp
namespace iat { void Add(const char* dll, const char* fn, void* hook, void** orig); }

namespace native {
namespace {

decltype(&MessageBoxW) g_messageBox;
decltype(&MessageBoxExW) g_messageBoxEx;
decltype(&MessageBoxIndirectW) g_messageBoxIndirect;
decltype(&SetWindowTextW) g_setWindowText;
decltype(&SetDlgItemTextW) g_setDlgItemText;

const wchar_t* Tr(const wchar_t* s) {
    if (!s || IS_INTRESOURCE(s)) return s;
    const std::wstring* en = TranslateText(s, wcslen(s));
    return en ? en->c_str() : s;
}

// Text the user typed or can edit is theirs: never touch edit controls.
bool IsEditable(HWND w) {
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    return _wcsicmp(cls, L"Edit") == 0 || wcsstr(cls, L"RichEdit") || wcsstr(cls, L"RICHEDIT") ||
           _wcsicmp(cls, L"ComboBox") == 0;
}

int WINAPI HookMessageBoxW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type) {
    return g_messageBox(w, Tr(text), Tr(caption), type);
}
int WINAPI HookMessageBoxExW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang) {
    return g_messageBoxEx(w, Tr(text), Tr(caption), type, lang);
}
int WINAPI HookMessageBoxIndirectW(const MSGBOXPARAMSW* p) {
    if (!p) return g_messageBoxIndirect(p);
    MSGBOXPARAMSW q = *p;
    if (!q.hInstance) {  // with an hInstance the strings may be resource ids
        q.lpszText = Tr(q.lpszText);
        q.lpszCaption = Tr(q.lpszCaption);
    }
    return g_messageBoxIndirect(&q);
}
BOOL WINAPI HookSetWindowTextW(HWND w, LPCWSTR text) {
    return g_setWindowText(w, IsEditable(w) ? text : Tr(text));
}
BOOL WINAPI HookSetDlgItemTextW(HWND dlg, int id, LPCWSTR text) {
    HWND item = GetDlgItem(dlg, id);
    return g_setDlgItemText(dlg, id, item && IsEditable(item) ? text : Tr(text));
}

void TranslateWindow(HWND w) {
    if (IsEditable(w)) return;
    wchar_t buf[1024];
    int n = GetWindowTextW(w, buf, 1024);
    if (n <= 0) return;
    const std::wstring* en = TranslateText(buf, (size_t)n);
    if (en) g_setWindowText ? g_setWindowText(w, en->c_str()) : SetWindowTextW(w, en->c_str());
}

BOOL CALLBACK TranslateChild(HWND w, LPARAM) {
    TranslateWindow(w);
    return TRUE;
}

void CALLBACK OnDialog(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !w) return;
    TranslateWindow(w);
    EnumChildWindows(w, TranslateChild, 0);
}

DWORD WINAPI EventThread(void*) {
    // Out-of-context events are delivered to this thread's message loop.
    if (!SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART, nullptr, OnDialog,
                         GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD))
        return 0;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

}  // namespace

void Start() {
    iat::Add("user32.dll", "MessageBoxW", (void*)&HookMessageBoxW, (void**)&g_messageBox);
    iat::Add("user32.dll", "MessageBoxExW", (void*)&HookMessageBoxExW, (void**)&g_messageBoxEx);
    iat::Add("user32.dll", "MessageBoxIndirectW", (void*)&HookMessageBoxIndirectW, (void**)&g_messageBoxIndirect);
    iat::Add("user32.dll", "SetWindowTextW", (void*)&HookSetWindowTextW, (void**)&g_setWindowText);
    iat::Add("user32.dll", "SetDlgItemTextW", (void*)&HookSetDlgItemTextW, (void**)&g_setDlgItemText);
    if (HANDLE t = CreateThread(nullptr, 0, EventThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace native
