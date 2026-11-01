// "vrserver" falso: carrega o proxy como o SteamVR faria e exercita o caminho
// completo (Init -> TrackedDeviceAdded -> Activate -> GetComponent -> Present).
#include <openvr_driver.h>
#include <openvr_driver_legado.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU: %s (linha %d)\n", #c, __LINE__); g_fail++; } else std::printf("ok: %s\n", #c); } while (0)

vr::ITrackedDeviceServerDriver *g_addedHmd = nullptr;

class Host : public vr::IVRServerDriverHost {
public:
    bool TrackedDeviceAdded(const char *, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver *d) override { g_addedHmd = d; return true; }
    void TrackedDevicePoseUpdated(uint32_t, const vr::DriverPose_t &, uint32_t) override {}
    void VsyncEvent(double) override {}
    void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t &, double) override {}
    bool IsExiting() override { return false; }
    bool PollNextEvent(vr::VREvent_t *, uint32_t) override { return false; }
    void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t *, uint32_t) override {}
    void RequestRestart(const char *, const char *, const char *, const char *) override {}
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming *, uint32_t) override { return 0; }
    void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t &, const vr::HmdMatrix34_t &) override {}
    void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t &, const vr::HmdRect2_t &) override {}
    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
} g_host;

// Propriedades: o teste confere se o proxy habilitou a trava por jogo no HMD.
int g_travaPorJogo = -1; // -1 = ninguém escreveu
class Props : public vr::IVRProperties {
public:
    vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t, vr::PropertyRead_t *b, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) b[i].eError = vr::TrackedProp_UnknownProperty;
        return vr::TrackedProp_Success;
    }
    vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t c, vr::PropertyWrite_t *b, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            b[i].eError = vr::TrackedProp_Success;
            if (c == 77 && b[i].prop == vr::Prop_Hmd_SupportsAppThrottling_Bool && b[i].writeType == vr::PropertyWrite_Set)
                g_travaPorJogo = *static_cast<bool *>(b[i].pvBuffer) ? 1 : 0;
        }
        return vr::TrackedProp_Success;
    }
    const char *GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return ""; }
    vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t d) override { return d == 0 ? 77 : 0; }
} g_props;

class Ctx : public vr::IVRDriverContext {
public:
    void *GetGenericInterface(const char *v, vr::EVRInitError *e) override {
        if (e) *e = vr::VRInitError_None;
        if (!std::strcmp(v, vr::IVRServerDriverHost_Version)) return &g_host;
        if (!std::strcmp(v, vr::IVRProperties_Version)) return &g_props;
        if (e) *e = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override { return 1; }
} g_ctx;

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("uso: %s <proxy.so>\n", argv[0]); return 2; }
    void *proxy = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(proxy != nullptr);
    if (!proxy) { std::printf("%s\n", dlerror()); return 1; }
    auto factory = (void *(*)(const char *, int *))dlsym(proxy, "HmdDriverFactory");
    CHECK(factory != nullptr);

    int rc = -1;
    auto *prov = (vr::IServerTrackedDeviceProvider *)factory(vr::IServerTrackedDeviceProvider_Version, &rc);
    CHECK(prov != nullptr && rc == 0);
    CHECK(prov->Init(&g_ctx) == vr::VRInitError_None);
    prov->RunFrame();
    CHECK(prov->GetInterfaceVersions() != nullptr);

    // O HMD registrado deve ser o wrapper, não o objeto real.
    CHECK(g_addedHmd != nullptr);
    if (!g_addedHmd) return 1;
    CHECK(g_addedHmd->Activate(0) == vr::VRInitError_None);
    // Frame gen ligado: o HMD passa a aceitar a trava por jogo; desligado, nada muda.
    CHECK(g_travaPorJogo == (std::getenv("GF_INI") ? 1 : -1));
    if (const char *ini = std::getenv("GF_INI")) {
        // Desligar no app (o ini é relido a cada 1 s) devolve o valor do driver; religar, liga.
        auto gravar = [ini](const char *v) { FILE *f = std::fopen(ini, "wb"); if (f) { std::fputs(v, f); std::fclose(f); } };
        gravar("[framegen]\nativo = 0\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(g_travaPorJogo == 0);
        gravar("; teste\n[framegen]\nativo = 1\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(g_travaPorJogo == 1);
    }

    auto *vd = (vr::IVRVirtualDisplay *)g_addedHmd->GetComponent(vr::IVRVirtualDisplay_Version);
    auto *dm = (vr::IVRDriverDirectModeComponent *)g_addedHmd->GetComponent(vr::IVRDriverDirectModeComponent_Version);
    CHECK(vd && dm);
    if (!vd || !dm) return 1;
    CHECK(g_addedHmd->GetComponent("IVRCameraComponent_003") == nullptr);
    // Versão desconhecida: deve repassar (null aqui, pois o fake não implementa).
    CHECK(g_addedHmd->GetComponent("IVRVirtualDisplay_999") == nullptr);

    vr::PresentInfo_t pi{0x1234, vr::VSync_None, 1, 0.0};
    for (int i = 0; i < 100; ++i) vd->Present(&pi, sizeof(pi));
    float s = 0; uint64_t c = 0;
    CHECK(vd->GetTimeSinceLastVsync(&s, &c) && c == 42);

    vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t desc{2064, 2208, 29, 1};
    vr::IVRDriverDirectModeComponent::SwapTextureSet_t set{};
    dm->CreateSwapTextureSet(1, &desc, &set);
    CHECK(set.rSharedTextureHandles[2] == 0x12);
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
    for (int i = 0; i < 50; ++i) { dm->SubmitLayer(eyes); dm->Present(0); }
    vr::DriverDirectMode_FrameTiming ft{sizeof(ft), 0, 0, 0, vr::VRCompositor_ReprojectionMotion_Enabled};
    dm->GetFrameTiming(&ft);
    CHECK(ft.m_nNumFramePresents == 7);

    // Como o Pico Connect de verdade: Display 002 e DirectMode 008 (SDK antigo).
    void *realLib = dlopen(argv[2], RTLD_NOW | RTLD_NOLOAD);
    CHECK(realLib != nullptr);
    if (!realLib) return 1;
    using DM8 = vr::legado::IVRDriverDirectModeComponent;
    CHECK(g_addedHmd->GetComponent(vr::legado::IVRDisplayComponent_Version) != nullptr);
    auto *dm8 = (DM8 *)g_addedHmd->GetComponent(vr::legado::IVRDriverDirectModeComponent_Version);
    void **real8 = (void **)dlsym(realLib, "g_fakeDM8"), **real7 = (void **)dlsym(realLib, "g_fakeDM7");
    int *c8 = (int *)dlsym(realLib, "g_fake8");
    auto *ultimo = (const DM8::SubmitLayerPerEye_t *)dlsym(realLib, "g_ultimo8");
    auto *trava = (const DM8::Throttling_t *)dlsym(realLib, "g_trava8");
    CHECK(real8 && real7 && c8 && ultimo && trava);
    if (!dm8 || !real8 || !real7 || !c8 || !ultimo || !trava) return 1;
    CHECK(dm8 != *real8); // embrulhou a 008
    // Versão que o proxy não conhece: volta o objeto do driver, sem wrapper.
    CHECK(g_addedHmd->GetComponent("IVRDriverDirectModeComponent_007") == *real7);

    DM8::SwapTextureSetDesc_t d8{1832, 1920, 29, 1};
    DM8::SwapTextureSet_t s8{};
    dm8->CreateSwapTextureSet(4242, &d8, &s8);
    CHECK(s8.rSharedTextureHandles[0] == 0x80 + 1832 && s8.rSharedTextureHandles[2] == 0x82);
    CHECK(s8.unTextureFlags == vr::VRSwapTextureFlag_Shared_NTHandle);

    // Jogo a meia taxa: cada pose vem duas vezes (o compositor repete o quadro). O que
    // chega no driver tem de ser byte a byte o que o SteamVR mandou, nos dois olhos.
    bool iguais = true;
    double segurou = 0; // segundos dentro do PostPresent
    for (int i = 0; i < 40; ++i) {
        DM8::SubmitLayerPerEye_t e[2];
        std::memset(e, 0, sizeof(e));
        for (int olho = 0; olho < 2; ++olho) {
            e[olho].hTexture = 0x1000 * (olho + 1) + i;
            e[olho].hDepthTexture = 0x5000 + olho;
            e[olho].bounds = {0.0f, 0.0f, 1.0f, 0.5f + 0.25f * olho};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) e[olho].mProjection.m[r][c] = float(olho * 100 + r * 4 + c);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) e[olho].mHmdPose.m[r][c] = float(i / 2) + 0.01f * float(r * 4 + c);
        }
        dm8->SubmitLayer(e);
        // Frame gen ligado: o quadro fica guardado até o Present (prova que passa pelo wrapper).
        if (i == 0) CHECK(c8[2] == (std::getenv("GF_INI") ? 0 : 1));
        dm8->Present(0x77);
        iguais = iguais && std::memcmp(ultimo, e, sizeof(e)) == 0;
        const DM8::Throttling_t t{1, 2};
        const auto t0 = std::chrono::steady_clock::now();
        dm8->PostPresent(&t);
        segurou += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    CHECK(iguais);
    CHECK(c8[2] == 40 && c8[3] == 40 && c8[4] == 40); // SubmitLayer, Present, PostPresent
    // Trava declarada pelo proxy (frame gen ligado): o driver nunca recebe trava > 0, quem segura
    // os vsyncs é o proxy. Desligado, a trava vai como veio.
    const bool fgLigado = std::getenv("GF_INI") != nullptr;
    CHECK(trava->nFramesToThrottle == (fgLigado ? 0u : 1u) && trava->nAdditionalFramesToPredict == 2);
    CHECK(fgLigado ? segurou >= 40 * 0.015 : segurou < 0.2); // 40 PostPresents de ~2 vsyncs (90 Hz)
    vr::SharedTextureHandle_t hs[2] = {s8.rSharedTextureHandles[0], s8.rSharedTextureHandles[0]};
    uint32_t idx[2] = {9, 9};
    dm8->GetNextSwapTextureSetIndex(hs, &idx);
    CHECK(idx[0] == 1 && idx[1] == 2);
    vr::DriverDirectMode_FrameTiming ft8{sizeof(ft8), 0, 0, 0, vr::VRCompositor_ReprojectionMotion_Enabled};
    dm8->GetFrameTiming(&ft8);
    CHECK(ft8.m_nNumFramePresents == 9 && ft8.m_nReprojectionFlags == vr::VRCompositor_ReprojectionMotion_Enabled);
    dm8->DestroySwapTextureSet(s8.rSharedTextureHandles[0]);
    dm8->DestroyAllSwapTextureSets(4242);
    CHECK(c8[0] == 1 && c8[1] == 1 && c8[5] == 1 && c8[6] == 1 && c8[7] == 1); // Create, Destroy, Timing, NextIndex, DestroyAll

    // Plugin do compositor (vídeo dentro do vrcompositor.exe): o DirectMode dele também é embrulhado.
    auto *plugin = (vr::IVRCompositorPluginProvider *)factory(vr::IVRCompositorPluginProvider_Version, &rc);
    CHECK(plugin != nullptr && rc == 0);
    if (!plugin) return 1;
    CHECK(plugin->Init(&g_ctx) == vr::VRInitError_None);
    auto *dmPlugin = (vr::IVRDriverDirectModeComponent *)plugin->GetComponent(vr::IVRDriverDirectModeComponent_Version);
    CHECK(dmPlugin != nullptr);
    if (!dmPlugin) return 1;
    for (int i = 0; i < 10; ++i) { dmPlugin->SubmitLayer(eyes); dmPlugin->Present(0); }
    plugin->Cleanup();

    // Confere os contadores do driver falso: tudo chegou do outro lado?
    int *counters = nullptr;
    // O driver real foi carregado pelo proxy com RTLD_LOCAL; pegamos pelo nome.
    counters = (int *)dlsym(realLib, "g_fakeCounters");
    CHECK(counters != nullptr);
    if (counters) {
        CHECK(counters[0] == 100); // Present VD
        CHECK(counters[1] == 60);  // Present DM (50 do HMD + 10 do plugin)
        CHECK(counters[2] == 60);  // SubmitLayer
        CHECK(counters[3] == 1);   // Activate
        CHECK(counters[4] == 1);   // RunFrame
        CHECK(counters[5] == 1);   // Init
        CHECK(counters[6] == 1);   // CreateSwapTextureSet
        CHECK(counters[7] == 1);   // GetFrameTiming
    }
    void **realPlugin = (void **)dlsym(realLib, "g_fakePluginDM");
    CHECK(realPlugin && *realPlugin != (void *)dmPlugin); // devolveu o wrapper
    prov->Cleanup();

    std::printf(g_fail ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", g_fail);
    return g_fail ? 1 : 0;
}
