#include "platform.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 // SDK antigo; Windows 10 1803+
#endif
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace gf {

static void AnchorSymbol() {}

std::string OwnModulePath() {
#ifdef _WIN32
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&AnchorSymbol), &self);
    char buf[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameA(self, buf, sizeof(buf));
    return std::string(buf, n);
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&AnchorSymbol), &info) && info.dli_fname)
        return info.dli_fname;
    return {};
#endif
}

std::string RealDriverPath() {
    std::string self = OwnModulePath();
    size_t dot = self.find_last_of('.');
    size_t slash = self.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return self + "_real";
    return self.substr(0, dot) + "_real" + self.substr(dot);
}

LibHandle LoadLib(const std::string &path, std::string *error) {
#ifdef _WIN32
    // LOAD_WITH_ALTERED_SEARCH_PATH: dependências da DLL real são procuradas
    // na pasta dela, como se o SteamVR a tivesse carregado diretamente.
    HMODULE h = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h && error) *error = "LoadLibraryEx falhou, GetLastError=" + std::to_string(GetLastError());
    return reinterpret_cast<LibHandle>(h);
#else
    void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h && error) *error = dlerror();
    return h;
#endif
}

void *GetSymbol(LibHandle lib, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

std::string TempDir() {
#ifdef _WIN32
    char buf[MAX_PATH + 1] = {};
    DWORD n = GetTempPathA(sizeof(buf), buf);
    return n ? std::string(buf, n) : std::string(".\\");
#else
    return "/tmp/";
#endif
}

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void DormirAte(double t) {
#ifdef _WIN32
    // Sleep comum arredonda para o tick do sistema (até ~15 ms): timer de alta resolução.
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    for (;;) {
        double falta = t - NowSeconds();
        if (falta <= 0) return;
        if (falta < 0.0005) {
            std::this_thread::yield();
            continue;
        }
#ifdef _WIN32
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((falta - 0.0003) * 1e7); // relativo, em 100 ns
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 50);
                continue;
            }
        }
#endif
        std::this_thread::sleep_for(std::chrono::duration<double>(falta - 0.0003));
    }
}

uint32_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint32_t>(getpid());
#endif
}

} // namespace gf
