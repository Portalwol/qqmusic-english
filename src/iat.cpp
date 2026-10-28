// Import-table hooks across QQ Music's own modules (the DLLs in its folder).
// Modules keep loading (and some unload) for a while after start, so PatchAll()
// is called repeatedly; it pins each module while patching it.

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace iat {
namespace {

struct Hook { const char* fn; void* hook; void** orig; };
std::vector<Hook> g_hooks;
CRITICAL_SECTION g_lock;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

int g_patched;

void PatchModule(BYTE* base) {
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto slot = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* fn = ((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            for (const Hook& h : g_hooks) {
                if (strcmp(fn, h.fn) != 0 || (void*)slot->u1.Function == h.hook) continue;
                DWORD old;
                VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                slot->u1.Function = (ULONG_PTR)h.hook;
                ++g_patched;
                VirtualProtect(&slot->u1.Function, sizeof(void*), old, &old);
            }
        }
    }
}

void SafePatchModule(BYTE* base) {
    __try {
        PatchModule(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

// Hooks every import of fn (from any DLL) in QQ Music's modules. orig gets the real function.
void Add(const char* dll, const char* fn, void* hook, void** orig) {
    InitOnceExecuteOnce(&g_once, InitLock, nullptr, nullptr);
    *orig = (void*)GetProcAddress(LoadLibraryA(dll), fn);
    if (!*orig) return;
    EnterCriticalSection(&g_lock);
    g_hooks.push_back({fn, hook, orig});
    LeaveCriticalSection(&g_lock);
}

// Returns how many import slots are patched so far.
int PatchAll() {
    InitOnceExecuteOnce(&g_once, InitLock, nullptr, nullptr);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring appdir(exe, wcsrchr(exe, L'\\'));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return g_patched;
    EnterCriticalSection(&g_lock);
    MODULEENTRY32W me = {sizeof me};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsnicmp(me.szExePath, appdir.c_str(), appdir.size()) != 0) continue;
        // Modules can unload between the snapshot and now: pin it while we patch.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(0, me.szExePath, &pinned)) continue;
        if (pinned == me.hModule && pinned != (HMODULE)&__ImageBase) SafePatchModule((BYTE*)pinned);
        FreeLibrary(pinned);
    }
    LeaveCriticalSection(&g_lock);
    CloseHandle(snap);
    return g_patched;
}

}  // namespace iat
