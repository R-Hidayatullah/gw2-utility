// inject.exe -- injektor DLL sederhana (CreateRemoteThread + LoadLibraryW).
// Pakai: inject.exe  C:\full\path\gw2_textkey_hook.dll  [Gw2-64.exe]
// Jalankan SETELAH game login (agar thread teks sudah ada).
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>

static DWORD pidByName(const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{}; e.dwSize = sizeof e; DWORD pid = 0;
    if (Process32FirstW(s, &e)) do {
        if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; }
    } while (Process32NextW(s, &e));
    CloseHandle(s); return pid;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: inject <full\\path\\to.dll> [exeName=Gw2-64.exe]\n"); return 1; }
    const wchar_t* exe = (argc >= 3) ? argv[2] : L"Gw2-64.exe";
    DWORD pid = pidByName(exe);
    if (!pid) { wprintf(L"proses %s tidak ditemukan\n", exe); return 1; }

    HANDLE p = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!p) { wprintf(L"OpenProcess gagal %lu\n", GetLastError()); return 1; }

    size_t n = (wcslen(argv[1]) + 1) * sizeof(wchar_t);
    void* rem = VirtualAllocEx(p, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(p, rem, argv[1], n, nullptr);
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(p, nullptr, 0, ll, rem, 0, nullptr);
    if (!t) { wprintf(L"CreateRemoteThread gagal %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(t, INFINITE);
    wprintf(L"injected -> pid %lu\n", pid);
    return 0;
}
