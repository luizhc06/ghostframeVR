#include "platform.h"

#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
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

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace gf
