// Ponte cliente OpenVR do app (o "companion"): conecta num SteamVR que já está
// rodando, como um processo à parte. Nunca encosta no processo do jogo.
//
// Faz o mesmo que o openvr_api.dll (src/openvr_api_public.cpp do SDK): carrega o
// vrclient do runtime, pega o IVRClientCore_003 pela VRClientCoreFactory, Init e
// GetGenericInterface. Assim não distribuímos o openvr_api.dll. A API é C puro
// para o Rust (app/src/vr.rs); erros voltam como códigos do OpenVR, nunca exceção.
#define OPENVR_INTERFACE_INTERNAL // sem VR_Init/VR_Shutdown inline: não ligamos no openvr_api
#include <openvr.h>

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef wchar_t gf_char; // caminho em UTF-16: pasta do usuário pode ter acento
#else
#include <dlfcn.h>
typedef char gf_char;
#endif

namespace {

// Cópia de src/ivrclientcore.h do openvr v2.15.6 (não vem nos headers públicos).
// A ordem dos métodos é a vtable que o vrclient implementa: não mexer.
class IVRClientCore {
public:
    virtual vr::EVRInitError Init(vr::EVRApplicationType eApplicationType, const char *pStartupInfo) = 0;
    virtual void Cleanup() = 0;
    virtual vr::EVRInitError IsInterfaceVersionValid(const char *pchInterfaceVersion) = 0;
    virtual void *GetGenericInterface(const char *pchNameAndVersion, vr::EVRInitError *peError) = 0;
    virtual bool BIsHmdPresent() = 0;
    virtual const char *GetEnglishStringForHmdError(vr::EVRInitError eError) = 0;
    virtual const char *GetIDForVRInitError(vr::EVRInitError eError) = 0;
};
const char *const IVRClientCore_Version = "IVRClientCore_003";
typedef void *(*VRClientCoreFactoryFn)(const char *pInterfaceName, int *pReturnCode);

void *Carregar(const gf_char *caminho, char *msg, uint32_t tam) {
#ifdef _WIN32
    // Igual ao openvr_api: dependências do vrclient são procuradas na pasta dele.
    HMODULE h = LoadLibraryExW(caminho, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) snprintf(msg, tam, "não consegui carregar o vrclient (erro %lu)", GetLastError());
    return reinterpret_cast<void *>(h);
#else
    void *h = dlopen(caminho, RTLD_LOCAL | RTLD_NOW);
    if (!h) snprintf(msg, tam, "não consegui carregar o vrclient (%s)", dlerror());
    return h;
#endif
}

void *Simbolo(void *lib, const char *nome) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(lib), nome));
#else
    return dlsym(lib, nome);
#endif
}

void Descarregar(void *lib) {
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

} // namespace

struct GfVr {
    void *lib;
    IVRClientCore *core;
    vr::IVRSystem *sistema;
    vr::IVRSettings *config;
    vr::IVROverlay *overlay;
    vr::IVRCompositor *compositor;
    vr::IVRApplications *apps;
};

// Só os campos de Compositor_FrameTiming que o vr.rs usa, num layout fixo
// (o struct original muda de empacotamento entre Windows e Linux).
struct GfQuadro {
    double t;
    float intervalo, wgp_chamado, poses_prontas, frame_pronto, gpu_pre, gpu_pos;
    uint32_t apresentacoes, perdidos, flags;
};
static const uint32_t kMaxQuadros = 128; // o compositor guarda os últimos 128

extern "C" {

GfVr *gf_vr_conectar(const gf_char *dll, char *msg, uint32_t tam) {
    void *lib = Carregar(dll, msg, tam);
    if (!lib) return nullptr;
    auto fabrica = reinterpret_cast<VRClientCoreFactoryFn>(Simbolo(lib, "VRClientCoreFactory"));
    int rc = 0;
    auto *core = fabrica ? static_cast<IVRClientCore *>(fabrica(IVRClientCore_Version, &rc)) : nullptr;
    if (!core) {
        snprintf(msg, tam, "o vrclient não tem %s (rc=%d)", IVRClientCore_Version, rc);
        Descarregar(lib);
        return nullptr;
    }
    // Background, não Overlay: um app Overlay ABRE o SteamVR se ele não estiver rodando
    // (e há corrida entre o Rust ver o vrserver e este Init). Background falha com
    // Init_NoServerForBackgroundApp, não segura o SteamVR aberto quando o resto fecha e
    // ainda pode criar overlays (o ReviveOverlay e o EasyOpenVR usam esse tipo).
    vr::EVRInitError e = core->Init(vr::VRApplication_Background, nullptr);
    if (e != vr::VRInitError_None) {
        snprintf(msg, tam, "%s (%d)", core->GetIDForVRInitError(e), static_cast<int>(e));
        Descarregar(lib); // como o openvr_api: Init falhou, sem Cleanup
        return nullptr;
    }
    auto pegar = [&](const char *versao) {
        vr::EVRInitError ei = vr::VRInitError_None;
        void *p = core->GetGenericInterface(versao, &ei);
        if (!p && e == vr::VRInitError_None) e = ei != vr::VRInitError_None ? ei : vr::VRInitError_Init_InterfaceNotFound;
        return p;
    };
    GfVr v{lib,
           core,
           static_cast<vr::IVRSystem *>(pegar(vr::IVRSystem_Version)),
           static_cast<vr::IVRSettings *>(pegar(vr::IVRSettings_Version)),
           static_cast<vr::IVROverlay *>(pegar(vr::IVROverlay_Version)),
           static_cast<vr::IVRCompositor *>(pegar(vr::IVRCompositor_Version)),
           static_cast<vr::IVRApplications *>(pegar(vr::IVRApplications_Version))};
    // VR_Init também avisa a versão do SDK; o runtime pode ajustar compatibilidade por ela.
    if (e == vr::VRInitError_None)
        e = v.sistema->SetSDKVersion(vr::k_nSteamVRVersionMajor, vr::k_nSteamVRVersionMinor, vr::k_nSteamVRVersionBuild);
    if (e != vr::VRInitError_None) {
        snprintf(msg, tam, "%s (%d)", core->GetIDForVRInitError(e), static_cast<int>(e));
        core->Cleanup();
        Descarregar(lib);
        return nullptr;
    }
    return new GfVr(v);
}

void gf_vr_desconectar(GfVr *v) {
    v->core->Cleanup();
    // Descarrega como o openvr_api: DLL presa no processo travaria a atualização do SteamVR.
    Descarregar(v->lib);
    delete v;
}

bool gf_vr_info_hmd(GfVr *v, float *hz, uint32_t *largura, uint32_t *altura, char *modelo, char *fabricante,
                    uint32_t tam) {
    auto *s = v->sistema;
    const auto hmd = vr::k_unTrackedDeviceIndex_Hmd;
    if (!s->IsTrackedDeviceConnected(hmd)) return false;
    *hz = s->GetFloatTrackedDeviceProperty(hmd, vr::Prop_DisplayFrequency_Float);
    s->GetRecommendedRenderTargetSize(largura, altura);
    s->GetStringTrackedDeviceProperty(hmd, vr::Prop_ModelNumber_String, modelo, tam);
    s->GetStringTrackedDeviceProperty(hmd, vr::Prop_ManufacturerName_String, fabricante, tam);
    return true;
}

// tipo: 0 bool, 1 int32, 2 float. O valor viaja como double (cabe os três sem perda).
int32_t gf_vr_ler(GfVr *v, const char *secao, const char *chave, int32_t tipo, double *valor) {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    switch (tipo) {
    case 0: *valor = v->config->GetBool(secao, chave, &e); break;
    case 1: *valor = v->config->GetInt32(secao, chave, &e); break;
    default: *valor = v->config->GetFloat(secao, chave, &e); break;
    }
    return e;
}

int32_t gf_vr_gravar(GfVr *v, const char *secao, const char *chave, int32_t tipo, double valor) {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    switch (tipo) {
    case 0: v->config->SetBool(secao, chave, valor != 0.0, &e); break;
    case 1: v->config->SetInt32(secao, chave, static_cast<int32_t>(valor), &e); break;
    default: v->config->SetFloat(secao, chave, static_cast<float>(valor), &e); break;
    }
    return e;
}

// qual: 's' settings, 'o' overlay, 'a' applications. O texto é do vrclient: copiar na hora.
const char *gf_vr_nome_erro(GfVr *v, char qual, int32_t e) {
    switch (qual) {
    case 's': return v->config->GetSettingsErrorNameFromEnum(static_cast<vr::EVRSettingsError>(e));
    case 'o': return v->overlay->GetOverlayErrorNameFromEnum(static_cast<vr::EVROverlayError>(e));
    default: return v->apps->GetApplicationsErrorNameFromEnum(static_cast<vr::EVRApplicationError>(e));
    }
}

uint32_t gf_vr_quadros(GfVr *v, GfQuadro *saida, uint32_t max) {
    // Sem jogo (app de cena) o histórico é só do compositor: não há o que medir.
    if (!v->apps->GetCurrentSceneProcessId()) return 0;
    vr::Compositor_FrameTiming t[kMaxQuadros];
    t[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
    uint32_t n = v->compositor->GetFrameTimings(t, max < kMaxQuadros ? max : kMaxQuadros);
    for (uint32_t i = 0; i < n; i++) {
        const auto &f = t[i];
        saida[i] = {f.m_flSystemTimeInSeconds, f.m_flClientFrameIntervalMs, f.m_flWaitGetPosesCalledMs,
                    f.m_flNewPosesReadyMs,     f.m_flNewFrameReadyMs,       f.m_flPreSubmitGpuMs,
                    f.m_flPostSubmitGpuMs,     f.m_nNumFramePresents,       f.m_nNumDroppedFrames,
                    f.m_nReprojectionFlags};
    }
    return n;
}

bool gf_vr_deve_sair(GfVr *v) {
    vr::VREvent_t ev;
    while (v->sistema->PollNextEvent(&ev, sizeof(ev))) {
        if (ev.eventType == vr::VREvent_Quit) {
            // O SteamVR mata quem não confirma; o Rust derruba a sessão logo depois.
            v->sistema->AcknowledgeQuit_Exiting();
            return true;
        }
    }
    return false;
}

int32_t gf_vr_autostart(GfVr *v, const char *manifesto, const char *chave, bool ligado) {
    vr::EVRApplicationError e = v->apps->AddApplicationManifest(manifesto, false);
    if (e != vr::VRApplicationError_None && e != vr::VRApplicationError_AppKeyAlreadyExists) return e;
    return v->apps->SetApplicationAutoLaunch(chave, ligado);
}

int32_t gf_overlay_criar(GfVr *v, const char *chave, const char *nome, uint64_t *h) {
    vr::EVROverlayError e = v->overlay->CreateOverlay(chave, nome, h);
    // Já criado por nós nesta sessão (ex.: HUD religado): reaproveita.
    if (e == vr::VROverlayError_KeyInUse) e = v->overlay->FindOverlay(chave, h);
    return e;
}

int32_t gf_overlay_imagem(GfVr *v, uint64_t h, const void *rgba, uint32_t largura, uint32_t altura) {
    return v->overlay->SetOverlayRaw(h, const_cast<void *>(rgba), largura, altura, 4);
}

// ancora: 0 pulso esquerdo, 1 pulso direito, 2 visão (HMD), 3 mundo (standing).
// m: HmdMatrix34_t por linhas, já calculada no Rust.
int32_t gf_overlay_posicionar(GfVr *v, uint64_t h, int32_t ancora, const float *m, float largura, float alpha) {
    auto *o = v->overlay;
    vr::HmdMatrix34_t mat;
    for (int i = 0; i < 12; i++) mat.m[i / 4][i % 4] = m[i];
    vr::EVROverlayError e = o->SetOverlayWidthInMeters(h, largura);
    if (e == vr::VROverlayError_None) e = o->SetOverlayAlpha(h, alpha);
    if (e != vr::VROverlayError_None) return e;
    if (ancora == 3) return o->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &mat);
    vr::TrackedDeviceIndex_t dev = vr::k_unTrackedDeviceIndex_Hmd;
    if (ancora < 2) {
        dev = v->sistema->GetTrackedDeviceIndexForControllerRole(
            ancora == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        if (dev == vr::k_unTrackedDeviceIndexInvalid) return vr::VROverlayError_InvalidTrackedDevice;
    }
    return o->SetOverlayTransformTrackedDeviceRelative(h, dev, &mat);
}

int32_t gf_overlay_mostrar(GfVr *v, uint64_t h, bool visivel) {
    return visivel ? v->overlay->ShowOverlay(h) : v->overlay->HideOverlay(h);
}

} // extern "C"
