// "Driver da Pico" falso para testes: implementa um HMD com IVRVirtualDisplay
// e IVRDriverDirectModeComponent e conta quantas chamadas recebeu.
#include <openvr_driver.h>

#include <cstring>

extern "C" __attribute__((visibility("default"))) int g_fakeCounters[8];
int g_fakeCounters[8] = {};
enum { kPresentVD, kPresentDM, kSubmit, kActivate, kRunFrame, kInit, kCreateSwap, kTiming };

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

class FakeHmd : public vr::ITrackedDeviceServerDriver {
public:
    vr::EVRInitError Activate(uint32_t) override { g_fakeCounters[kActivate]++; return vr::VRInitError_None; }
    void Deactivate() override {}
    void EnterStandby() override {}
    void *GetComponent(const char *n) override {
        if (!std::strcmp(n, vr::IVRVirtualDisplay_Version)) return &vd;
        if (!std::strcmp(n, vr::IVRDriverDirectModeComponent_Version)) return &dm;
        return nullptr;
    }
    void DebugRequest(const char *, char *b, uint32_t s) override { if (s) b[0] = 0; }
    vr::DriverPose_t GetPose() override { return {}; }
    FakeVD vd;
    FakeDM dm;
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

} // namespace

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *rc) {
    if (!std::strcmp(name, vr::IServerTrackedDeviceProvider_Version)) { *rc = 0; return &g_provider; }
    *rc = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
