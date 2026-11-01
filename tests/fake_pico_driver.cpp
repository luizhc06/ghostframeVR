// "Driver da Pico" falso para testes: implementa um HMD com IVRVirtualDisplay
// e IVRDriverDirectModeComponent e conta quantas chamadas recebeu. Como o Pico Connect
// de verdade, também responde DirectMode 008 e Display 002 (SDK antigo).
#include <openvr_driver.h>
#include <openvr_driver_legado.h>

#include <cstring>

extern "C" __attribute__((visibility("default"))) int g_fakeCounters[8];
int g_fakeCounters[8] = {};
enum { kPresentVD, kPresentDM, kSubmit, kActivate, kRunFrame, kInit, kCreateSwap, kTiming };

// DirectMode 008: guarda o que chegou para o teste comparar byte a byte.
using DM8 = vr::legado::IVRDriverDirectModeComponent;
extern "C" __attribute__((visibility("default"))) int g_fake8[8];
int g_fake8[8] = {};
enum { k8Create, k8Destroy, k8Submit, k8Present, k8PostPresent, k8Timing, k8NextIndex, k8DestroyAll };
extern "C" __attribute__((visibility("default"))) DM8::SubmitLayerPerEye_t g_ultimo8[2];
DM8::SubmitLayerPerEye_t g_ultimo8[2] = {};
extern "C" __attribute__((visibility("default"))) DM8::Throttling_t g_trava8;
DM8::Throttling_t g_trava8 = {};

namespace {

class FakeVD : public vr::IVRVirtualDisplay {
public:
    void Present(const vr::PresentInfo_t *, uint32_t) override { g_fakeCounters[kPresentVD]++; }
    void WaitForPresent() override {}
    bool GetTimeSinceLastVsync(float *s, uint64_t *c) override { *s = 0.001f; *c = 42; return true; }
};

class FakeDM : public vr::IVRDriverDirectModeComponent {
public:
    void CreateSwapTextureSet(uint32_t, const SwapTextureSetDesc_t *, SwapTextureSet_t *o) override {
        g_fakeCounters[kCreateSwap]++;
        o->rSharedTextureHandles[0] = 0x10; o->rSharedTextureHandles[1] = 0x11; o->rSharedTextureHandles[2] = 0x12;
        o->unTextureFlags = 0;
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&)[2]) override { g_fakeCounters[kSubmit]++; }
    void Present(vr::SharedTextureHandle_t) override { g_fakeCounters[kPresentDM]++; }
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *t) override { g_fakeCounters[kTiming]++; t->m_nNumFramePresents = 7; }
};

class FakeDM8 : public DM8 {
public:
    void CreateSwapTextureSet(uint32_t, const SwapTextureSetDesc_t *d, SwapTextureSet_t *o) override {
        g_fake8[k8Create]++;
        o->rSharedTextureHandles[0] = 0x80 + d->nWidth; o->rSharedTextureHandles[1] = 0x81; o->rSharedTextureHandles[2] = 0x82;
        o->unTextureFlags = vr::VRSwapTextureFlag_Shared_NTHandle;
    }
    void DestroySwapTextureSet(vr::SharedTextureHandle_t) override { g_fake8[k8Destroy]++; }
    void DestroyAllSwapTextureSets(uint32_t pid) override { if (pid == 4242) g_fake8[k8DestroyAll]++; }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t[2], uint32_t (*i)[2]) override {
        g_fake8[k8NextIndex]++; (*i)[0] = 1; (*i)[1] = 2;
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&e)[2]) override {
        g_fake8[k8Submit]++; std::memcpy(g_ultimo8, e, sizeof(g_ultimo8));
    }
    void Present(vr::SharedTextureHandle_t) override { g_fake8[k8Present]++; }
    void PostPresent(const Throttling_t *t) override { g_fake8[k8PostPresent]++; if (t) g_trava8 = *t; }
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *t) override { g_fake8[k8Timing]++; t->m_nNumFramePresents = 9; }
};

class FakeDisplay2 : public vr::legado::IVRDisplayComponent {
public:
    void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override { *x = *y = 0; *w = *h = 1; }
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return true; }
    void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override { *w = *h = 1; }
    void GetEyeOutputViewport(vr::EVREye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override { *x = *y = 0; *w = *h = 1; }
    void GetProjectionRaw(vr::EVREye, float *l, float *r, float *t, float *b) override { *l = *t = -1.28f; *r = *b = 1.28f; }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float, float) override { return {}; }
};

// Uma DirectMode de versão que o proxy não conhece: tem de voltar sem wrapper.
class FakeDM7 : public DM8 {};

class FakeHmd : public vr::ITrackedDeviceServerDriver {
public:
    vr::EVRInitError Activate(uint32_t) override { g_fakeCounters[kActivate]++; return vr::VRInitError_None; }
    void Deactivate() override {}
    void EnterStandby() override {}
    void *GetComponent(const char *n) override {
        if (!std::strcmp(n, vr::IVRVirtualDisplay_Version)) return &vd;
        if (!std::strcmp(n, vr::IVRDriverDirectModeComponent_Version)) return &dm;
        if (!std::strcmp(n, vr::legado::IVRDriverDirectModeComponent_Version)) return &dm8;
        if (!std::strcmp(n, vr::legado::IVRDisplayComponent_Version)) return &disp2;
        if (!std::strcmp(n, "IVRDriverDirectModeComponent_007")) return &dm7;
        return nullptr;
    }
    void DebugRequest(const char *, char *b, uint32_t s) override { if (s) b[0] = 0; }
    vr::DriverPose_t GetPose() override { return {}; }
    FakeVD vd;
    FakeDM dm;
    FakeDM8 dm8;
    FakeDisplay2 disp2;
    FakeDM7 dm7;
};

class FakeProvider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *ctx) override {
        g_fakeCounters[kInit]++;
        // Como um driver real: pede o host ao contexto recebido (que será o wrapper).
        auto *host = (vr::IVRServerDriverHost *)ctx->GetGenericInterface(vr::IVRServerDriverHost_Version, nullptr);
        if (!host) return vr::VRInitError_Init_InterfaceNotFound;
        host->TrackedDeviceAdded("PICO-FAKE-001", vr::TrackedDeviceClass_HMD, &hmd);
        return vr::VRInitError_None;
    }
    void Cleanup() override {}
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override { g_fakeCounters[kRunFrame]++; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
    FakeHmd hmd;
};

FakeProvider g_provider;

// Como drivers que entregam o vídeo dentro do vrcompositor.exe.
class FakePlugin : public vr::IVRCompositorPluginProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *) override { return vr::VRInitError_None; }
    void Cleanup() override {}
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void *GetComponent(const char *n) override {
        return std::strcmp(n, vr::IVRDriverDirectModeComponent_Version) ? nullptr : &dm;
    }
    FakeDM dm;
};

FakePlugin g_plugin;

} // namespace

// O teste confere que o proxy devolveu um wrapper, não este objeto.
extern "C" __attribute__((visibility("default"))) void *g_fakePluginDM;
void *g_fakePluginDM = &g_plugin.dm;
extern "C" __attribute__((visibility("default"))) void *g_fakeDM8;
void *g_fakeDM8 = &g_provider.hmd.dm8;
extern "C" __attribute__((visibility("default"))) void *g_fakeDM7;
void *g_fakeDM7 = &g_provider.hmd.dm7;

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *rc) {
    if (!std::strcmp(name, vr::IServerTrackedDeviceProvider_Version)) { *rc = 0; return &g_provider; }
    if (!std::strcmp(name, vr::IVRCompositorPluginProvider_Version)) { *rc = 0; return &g_plugin; }
    *rc = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
