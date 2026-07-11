// gw2_textkey_hook.cpp
// Hook DLL untuk MENANGKAP password teks GW2 (textId -> key8) dari client retail
// (Gw2-64.exe, ASLR AKTIF). Alamat dicari via SIGNATURE SCAN relatif ke base modul,
// jadi tahan ASLR dan (sebagian besar) tahan patch. Tidak ada alamat hardcode.
//
// Meng-hook 2 fungsi sumber password (nama IDA):
//   sub_1410CBBB0(int textId /*RCX*/, uint64 key8 /*RDX*/)      -- insert tunggal
//   sub_1410D6A10(_, void* chunk /*RDX*/)                       -- bulk txtp (TextPackPasswords)
//        chunk: {u8 count@+2, u64 arr@+3}; record 12B {u32 id@0, u64 key@+4}
//
// Output: textkeys_hook.csv (di folder DLL). Format: textId,key8_hex  (dedup).
//
// Butuh MinHook (https://github.com/TsudaKageyu/minhook) untuk trampolin yang benar
// (prolog sub_1410CBBB0 mengandung jz rel32 -> harus di-relokasi; MinHook menangani ini).
//
// Build (MinGW, cocok dgn toolchain gw2mcp Anda):
//   g++ -shared -O2 -m64 -static -static-libgcc -static-libstdc++ \
//       -I minhook/include \
//       gw2_textkey_hook.cpp minhook/src/*.c minhook/src/hde/*.c \
//       -o gw2_textkey_hook.dll -lpsapi
//
// Inject SETELAH login (agar thread teks sudah ada) pakai injector DLL apa pun
// (mis. Process Hacker -> Miscellaneous -> Inject DLL, atau injector CreateRemoteThread).

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <set>
#include <mutex>
#include "MinHook.h"

static HMODULE   g_self   = nullptr;
static uintptr_t g_base   = 0;
static size_t    g_size   = 0;
static FILE*     g_fp     = nullptr;
static std::mutex g_mtx;
static std::set<std::pair<uint32_t,uint64_t>> g_seen;

// ---------------- util log ----------------
static void dbg(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
}

static void emit(uint32_t id, uint64_t key) {
    if (!key || id == 0xFFFFFFFF) return;            // sentinel / kosong
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_seen.insert({id, key}).second) return;    // dedup
    if (g_fp) { fprintf(g_fp, "%u,%016llx\n", id, (unsigned long long)key); fflush(g_fp); }
}

// ---------------- signature scan ----------------
// sig: "85 C9 0F 84 ?? ?? ?? ?? 48 ..."  ('??' = wildcard)
static uint8_t* find_sig(const char* sig) {
    std::vector<int> pat;
    for (const char* p = sig; *p; ) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { pat.push_back(-1); while (*p=='?') ++p; }
        else { pat.push_back((int)strtol(p, nullptr, 16)); p += 2; }
    }
    const size_t n = pat.size();
    if (!n || g_size < n) return nullptr;
    uint8_t* base = (uint8_t*)g_base;
    for (size_t i = 0; i + n <= g_size; ++i) {
        size_t j = 0;
        for (; j < n; ++j) if (pat[j] != -1 && base[i+j] != (uint8_t)pat[j]) break;
        if (j == n) return base + i;
    }
    return nullptr;
}

// ---------------- detours ----------------
typedef void*    (*t_insert)(int, uint64_t);
typedef uint64_t (*t_bulk)(uint64_t, uint64_t);
static t_insert o_insert = nullptr;
static t_bulk   o_bulk   = nullptr;

static void* hk_insert(int textId, uint64_t key8) {
    emit((uint32_t)textId, key8);
    return o_insert(textId, key8);
}

static uint64_t hk_bulk(uint64_t a1, uint64_t chunk) {
    __try {
        uint8_t  count = *(uint8_t*)(chunk + 2);
        uint64_t arr   = *(uint64_t*)(chunk + 3);      // unaligned read
        for (int i = 0; i < count; ++i) {
            uint32_t id  = *(uint32_t*)(arr + 12*i);
            uint64_t key = *(uint64_t*)(arr + 12*i + 4);
            emit(id, key);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* chunk tak valid -> abaikan */ }
    return o_bulk(a1, chunk);
}

// ---------------- init ----------------
static const char* SIG_INSERT =
    "85 C9 0F 84 ?? ?? ?? ?? 48 89 74 24 20 89 4C 24 08 57 48 83 EC 20 "
    "48 8B F2 48 89 5C 24 38 8B F9 E8 ?? ?? ?? ?? 4C 8D 44 24 40 48 8D 54 24 30 "
    "48 8B 58 50 48 81 C3 00 02 00 00";
static const char* SIG_BULK =
    "40 53 55 57 48 83 EC 20 48 8B EA E8 ?? ?? ?? ?? 0F B6 55 02 48 8B 48 50 "
    "03 91 04 02 00 00 48 8D 99 00 02 00 00 83 FA 08";

static DWORD WINAPI Init(LPVOID) {
    HMODULE hMod = GetModuleHandleW(nullptr);         // exe utama
    auto dos = (IMAGE_DOS_HEADER*)hMod;
    auto nt  = (IMAGE_NT_HEADERS*)((uint8_t*)hMod + dos->e_lfanew);
    g_base = (uintptr_t)hMod;
    g_size = nt->OptionalHeader.SizeOfImage;

    // buka CSV di folder DLL
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_self, path, MAX_PATH);
    if (wchar_t* s = wcsrchr(path, L'\\')) s[1] = 0;
    wcscat_s(path, L"textkeys_hook.csv");
    _wfopen_s(&g_fp, path, L"a");
    if (g_fp) fprintf(g_fp, "# session base=%p size=%zu\ntextId,key8_hex\n", (void*)g_base, g_size);

    uint8_t* pIns  = find_sig(SIG_INSERT);
    uint8_t* pBulk = find_sig(SIG_BULK);
    dbg("[gw2hook] base=%p size=%zu insert=%p bulk=%p\n", (void*)g_base, g_size, pIns, pBulk);

    if (MH_Initialize() != MH_OK) { dbg("[gw2hook] MH_Initialize gagal\n"); return 1; }
    if (pIns  && MH_CreateHook(pIns,  (void*)hk_insert, (void**)&o_insert) == MH_OK) MH_EnableHook(pIns);
    else dbg("[gw2hook] SIG_INSERT tidak ketemu / hook gagal\n");
    if (pBulk && MH_CreateHook(pBulk, (void*)hk_bulk,   (void**)&o_bulk)   == MH_OK) MH_EnableHook(pBulk);
    else dbg("[gw2hook] SIG_BULK tidak ketemu / hook gagal\n");

    if (g_fp) { fprintf(g_fp, "# hooks: insert=%s bulk=%s\n",
                        (pIns&&o_insert)?"ok":"MISS", (pBulk&&o_bulk)?"ok":"MISS"); fflush(g_fp); }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
