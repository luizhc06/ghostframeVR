// "vrserver" falso: carrega o proxy como o SteamVR faria e exercita o caminho
// completo (Init -> TrackedDeviceAdded -> Activate -> GetComponent -> Present).
#include <openvr_driver.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstring>

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

class Ctx : public vr::IVRDriverContext {
public:
    void *GetGenericInterface(const char *v, vr::EVRInitError *e) override {
        if (e) *e = vr::VRInitError_None;
        if (!std::strcmp(v, vr::IVRServerDriverHost_Version)) return &g_host;
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

    // Confere os contadores do driver falso: tudo chegou do outro lado?
    int *counters = nullptr;
    // O driver real foi carregado pelo proxy com RTLD_LOCAL; pegamos pelo nome.
    void *realLib = dlopen(argv[2], RTLD_NOW | RTLD_NOLOAD);
    if (realLib) counters = (int *)dlsym(realLib, "g_fakeCounters");
    CHECK(counters != nullptr);
    if (counters) {
        CHECK(counters[0] == 100); // Present VD
        CHECK(counters[1] == 50);  // Present DM
        CHECK(counters[2] == 50);  // SubmitLayer
        CHECK(counters[3] == 1);   // Activate
        CHECK(counters[4] == 1);   // RunFrame
        CHECK(counters[5] == 1);   // Init
        CHECK(counters[6] == 1);   // CreateSwapTextureSet
        CHECK(counters[7] == 1);   // GetFrameTiming
    }
    prov->Cleanup();

    std::printf(g_fail ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", g_fail);
    return g_fail ? 1 : 0;
}
