// Ponto de entrada do driver proxy.
//
// O SteamVR carrega a nossa DLL no lugar da DLL do Pico Connect e chama
// HmdDriverFactory. Nós carregamos a DLL real ("<nome>_real.dll"), pedimos a
// ela o mesmo objeto e devolvemos um wrapper que repassa tudo.
#include "log.h"
#include "platform.h"
#include "wrappers.h"

#include <openvr_driver.h>

#include <cstring>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#define GF_EXPORT extern "C" __declspec(dllexport)
#else
#define GF_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using FactoryFn = void *(*)(const char *, int *);

std::once_flag g_loadOnce;
FactoryFn g_realFactory = nullptr;
std::unique_ptr<gf::ProviderWrapper> g_provider;

void LoadReal() {
    gf::LogInit();
    std::string path = gf::RealDriverPath();
    // "recursos:" diz ao app o que esta DLL sabe fazer (proxy antigo = sem frame gen).
    GF_LOG("ghostframe proxy iniciado (recursos: framegen). DLL própria: %s", gf::OwnModulePath().c_str());
    GF_LOG("Carregando driver real: %s", path.c_str());
    std::string err;
    gf::LibHandle lib = gf::LoadLib(path, &err);
    if (!lib) {
        GF_LOG("ERRO: não foi possível carregar o driver real: %s", err.c_str());
        return;
    }
    g_realFactory = reinterpret_cast<FactoryFn>(gf::GetSymbol(lib, "HmdDriverFactory"));
    if (!g_realFactory) GF_LOG("ERRO: driver real não exporta HmdDriverFactory");
}

} // namespace

GF_EXPORT void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode) {
    std::call_once(g_loadOnce, LoadReal);
    if (!g_realFactory) {
        if (pReturnCode) *pReturnCode = vr::VRInitError_Init_FileNotFound;
        return nullptr;
    }

    void *real = g_realFactory(pInterfaceName, pReturnCode);
    GF_LOG("HmdDriverFactory(\"%s\") -> %s", pInterfaceName ? pInterfaceName : "(null)", real ? "ok" : "null");
    if (!real || !pInterfaceName) return real;

    if (std::strcmp(pInterfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
        if (!g_provider)
            g_provider = std::make_unique<gf::ProviderWrapper>(static_cast<vr::IServerTrackedDeviceProvider *>(real));
        return g_provider.get();
    }
    if (std::strncmp(pInterfaceName, "IServerTrackedDeviceProvider_", 29) == 0) {
        GF_LOG("!! SteamVR pediu %s, conhecemos %s — modo passthrough total",
                pInterfaceName, vr::IServerTrackedDeviceProvider_Version);
    }
    // Watchdog e outros: repassamos sem tocar.
    return real;
}
