#include "wrappers.h"

#include "log.h"
#include "platform.h"

#include <cstring>

namespace gf {

// Quantas chamadas de cada tipo logamos em detalhe antes de passar só a resumir.
static constexpr uint64_t kVerboseCalls = 30;

static const char *DeviceClassName(vr::ETrackedDeviceClass c) {
    switch (c) {
    case vr::TrackedDeviceClass_HMD: return "HMD";
    case vr::TrackedDeviceClass_Controller: return "Controller";
    case vr::TrackedDeviceClass_GenericTracker: return "GenericTracker";
    case vr::TrackedDeviceClass_TrackingReference: return "TrackingReference";
    case vr::TrackedDeviceClass_DisplayRedirect: return "DisplayRedirect";
    default: return "Invalid/Unknown";
    }
}

// ===========================================================================
void RateStats::Tick() {
    std::lock_guard<std::mutex> lock(m_mutex);
    double now = NowSeconds();
    if (m_windowStart == 0.0) {
        m_windowStart = m_last = now;
        return;
    }
    double dt = now - m_last;
    m_last = now;
    ++m_count;
    m_sumDt += dt;
    if (dt < m_minDt) m_minDt = dt;
    if (dt > m_maxDt) m_maxDt = dt;

    double window = now - m_windowStart;
    if (window >= 2.0 && m_count > 0) {
        GF_LOG("STATS %-28s %6.1f/s  dt(ms) avg=%.2f min=%.2f max=%.2f",
                m_name, m_count / window, 1000.0 * m_sumDt / m_count,
                1000.0 * m_minDt, 1000.0 * m_maxDt);
        m_windowStart = now;
        m_count = 0;
        m_sumDt = 0.0;
        m_minDt = 1e9;
        m_maxDt = 0.0;
    }
}

// ===========================================================================
// IVRVirtualDisplay
void VirtualDisplayWrapper::Present(const vr::PresentInfo_t *info, uint32_t size) {
    uint64_t n = m_calls++;
    if (n < kVerboseCalls && info) {
        GF_LOG("VirtualDisplay::Present #%llu tex=0x%llx vsync=%d frameId=%llu vsyncTime=%.6f size=%u",
                (unsigned long long)n, (unsigned long long)info->backbufferTextureHandle,
                (int)info->vsync, (unsigned long long)info->nFrameId,
                info->flVSyncTimeInSeconds, size);
    }
    m_presentRate.Tick();
    // >>> Ponto de inserção do frame gen (caminho VirtualDisplay) <<<
    m_real->Present(info, size);
}

void VirtualDisplayWrapper::WaitForPresent() { m_real->WaitForPresent(); }

bool VirtualDisplayWrapper::GetTimeSinceLastVsync(float *pfSeconds, uint64_t *pulFrameCounter) {
    return m_real->GetTimeSinceLastVsync(pfSeconds, pulFrameCounter);
}

// ===========================================================================
// IVRDriverDirectModeComponent
void DirectModeWrapper::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *d, SwapTextureSet_t *out) {
    m_real->CreateSwapTextureSet(pid, d, out);
    if (d && out) {
        GF_LOG("DirectMode::CreateSwapTextureSet pid=%u %ux%u fmt=%u samples=%u -> [0x%llx 0x%llx 0x%llx] flags=0x%x",
                pid, d->nWidth, d->nHeight, d->nFormat, d->nSampleCount,
                (unsigned long long)out->rSharedTextureHandles[0],
                (unsigned long long)out->rSharedTextureHandles[1],
                (unsigned long long)out->rSharedTextureHandles[2], out->unTextureFlags);
    }
}

void DirectModeWrapper::DestroySwapTextureSet(vr::SharedTextureHandle_t h) {
    GF_LOG("DirectMode::DestroySwapTextureSet 0x%llx", (unsigned long long)h);
    m_real->DestroySwapTextureSet(h);
}

void DirectModeWrapper::DestroyAllSwapTextureSets(uint32_t pid) {
    GF_LOG("DirectMode::DestroyAllSwapTextureSets pid=%u", pid);
    m_real->DestroyAllSwapTextureSets(pid);
}

void DirectModeWrapper::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) {
    m_real->GetNextSwapTextureSetIndex(handles, pIndices);
}

void DirectModeWrapper::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    uint64_t n = m_layers++;
    m_layersThisFrame++;
    if (n < kVerboseCalls) {
        for (int eye = 0; eye < 2; ++eye) {
            const auto &e = perEye[eye];
            GF_LOG("DirectMode::SubmitLayer #%llu eye=%d tex=0x%llx depth=0x%llx bounds=[%.3f %.3f %.3f %.3f] "
                    "posePred=%.4fs pos=(%.3f %.3f %.3f)",
                    (unsigned long long)n, eye, (unsigned long long)e.hTexture,
                    (unsigned long long)e.hDepthTexture, e.bounds.uMin, e.bounds.vMin,
                    e.bounds.uMax, e.bounds.vMax, e.flHmdPosePredictionTimeInSecondsFromNow,
                    e.mHmdPose.m[0][3], e.mHmdPose.m[1][3], e.mHmdPose.m[2][3]);
        }
    }
    // >>> Ponto de captura: texturas de cor (+ profundidade, se houver) e pose de cada olho <<<
    m_real->SubmitLayer(perEye);
}

void DirectModeWrapper::Present(vr::SharedTextureHandle_t syncTexture) {
    uint64_t n = m_presents++;
    uint32_t layers = m_layersThisFrame.exchange(0);
    if (n < kVerboseCalls)
        GF_LOG("DirectMode::Present #%llu sync=0x%llx layers=%u", (unsigned long long)n,
                (unsigned long long)syncTexture, layers);
    m_presentRate.Tick();
    // >>> Ponto de inserção do frame gen (caminho DirectMode) <<<
    m_real->Present(syncTexture);
}

void DirectModeWrapper::PostPresent(const Throttling_t *t) {
    m_real->PostPresent(t);
}

void DirectModeWrapper::GetFrameTiming(vr::DriverDirectMode_FrameTiming *ft) {
    uint32_t flagsIn = ft ? ft->m_nReprojectionFlags : 0;
    m_real->GetFrameTiming(ft);
    static std::atomic<uint64_t> calls{0};
    uint64_t n = calls++;
    if (ft && (n < kVerboseCalls || n % 600 == 0)) {
        GF_LOG("DirectMode::GetFrameTiming #%llu flagsIn=0x%x(motionSmoothing=%s) -> presents=%u misPresented=%u dropped=%u flagsOut=0x%x",
                (unsigned long long)n, flagsIn,
                (flagsIn & (vr::VRCompositor_ReprojectionMotion_Enabled | vr::VRCompositor_ReprojectionMotion_ForcedOn)) ? "on" : "off",
                ft->m_nNumFramePresents, ft->m_nNumMisPresented, ft->m_nNumDroppedFrames,
                ft->m_nReprojectionFlags);
    }
}

// ===========================================================================
// ITrackedDeviceServerDriver
DeviceWrapper::DeviceWrapper(vr::ITrackedDeviceServerDriver *real, const char *serial, vr::ETrackedDeviceClass cls)
    : m_real(real), m_class(cls) {
    GF_LOG("DeviceWrapper criado: serial=%s class=%s", serial ? serial : "(null)", DeviceClassName(cls));
}

vr::EVRInitError DeviceWrapper::Activate(uint32_t id) {
    vr::EVRInitError r = m_real->Activate(id);
    GF_LOG("Device(%s)::Activate id=%u -> %d", DeviceClassName(m_class), id, (int)r);
    return r;
}

void DeviceWrapper::Deactivate() { m_real->Deactivate(); }
void DeviceWrapper::EnterStandby() { m_real->EnterStandby(); }

void *DeviceWrapper::GetComponent(const char *name) {
    void *comp = m_real->GetComponent(name);
    GF_LOG("Device(%s)::GetComponent(\"%s\") -> %s", DeviceClassName(m_class),
            name ? name : "(null)", comp ? "IMPLEMENTADO" : "null");
    if (!comp || !name) return comp;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::strcmp(name, vr::IVRVirtualDisplay_Version) == 0) {
        if (!m_virtualDisplay)
            m_virtualDisplay = std::make_unique<VirtualDisplayWrapper>(static_cast<vr::IVRVirtualDisplay *>(comp));
        GF_LOG("  -> embrulhando IVRVirtualDisplay");
        return m_virtualDisplay.get();
    }
    if (std::strcmp(name, vr::IVRDriverDirectModeComponent_Version) == 0) {
        if (!m_directMode)
            m_directMode = std::make_unique<DirectModeWrapper>(static_cast<vr::IVRDriverDirectModeComponent *>(comp));
        GF_LOG("  -> embrulhando IVRDriverDirectModeComponent");
        return m_directMode.get();
    }
    if (std::strncmp(name, "IVRVirtualDisplay_", 18) == 0 ||
        std::strncmp(name, "IVRDriverDirectModeComponent_", 29) == 0) {
        GF_LOG("  !! versão diferente da que conhecemos (%s / %s) — repassando sem embrulhar",
                vr::IVRVirtualDisplay_Version, vr::IVRDriverDirectModeComponent_Version);
    }
    return comp;
}

void DeviceWrapper::DebugRequest(const char *req, char *buf, uint32_t size) {
    m_real->DebugRequest(req, buf, size);
}

vr::DriverPose_t DeviceWrapper::GetPose() { return m_real->GetPose(); }

// ===========================================================================
// IVRServerDriverHost
bool ServerDriverHostWrapper::TrackedDeviceAdded(const char *serial, vr::ETrackedDeviceClass cls,
                                                 vr::ITrackedDeviceServerDriver *pDriver) {
    GF_LOG("Host::TrackedDeviceAdded serial=%s class=%s", serial ? serial : "(null)", DeviceClassName(cls));
    vr::ITrackedDeviceServerDriver *toRegister = pDriver;
    // Só embrulhamos o HMD — é ele que tem os componentes de display.
    if (pDriver && cls == vr::TrackedDeviceClass_HMD) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_devices.push_back(std::make_unique<DeviceWrapper>(pDriver, serial, cls));
        toRegister = m_devices.back().get();
    }
    return m_real->TrackedDeviceAdded(serial, cls, toRegister);
}

void ServerDriverHostWrapper::TrackedDevicePoseUpdated(uint32_t dev, const vr::DriverPose_t &pose, uint32_t size) {
    if (dev == vr::k_unTrackedDeviceIndex_Hmd) m_hmdPoseRate.Tick();
    m_real->TrackedDevicePoseUpdated(dev, pose, size);
}

void ServerDriverHostWrapper::VsyncEvent(double offset) {
    m_vsyncRate.Tick();
    m_real->VsyncEvent(offset);
}

void ServerDriverHostWrapper::VendorSpecificEvent(uint32_t dev, vr::EVREventType t, const vr::VREvent_Data_t &d, double off) {
    m_real->VendorSpecificEvent(dev, t, d, off);
}

bool ServerDriverHostWrapper::IsExiting() { return m_real->IsExiting(); }

bool ServerDriverHostWrapper::PollNextEvent(vr::VREvent_t *ev, uint32_t size) {
    return m_real->PollNextEvent(ev, size);
}

void ServerDriverHostWrapper::GetRawTrackedDevicePoses(float pred, vr::TrackedDevicePose_t *arr, uint32_t count) {
    m_real->GetRawTrackedDevicePoses(pred, arr, count);
}

void ServerDriverHostWrapper::RequestRestart(const char *a, const char *b, const char *c, const char *d) {
    m_real->RequestRestart(a, b, c, d);
}

uint32_t ServerDriverHostWrapper::GetFrameTimings(vr::Compositor_FrameTiming *t, uint32_t n) {
    return m_real->GetFrameTimings(t, n);
}

void ServerDriverHostWrapper::SetDisplayEyeToHead(uint32_t dev, const vr::HmdMatrix34_t &l, const vr::HmdMatrix34_t &r) {
    m_real->SetDisplayEyeToHead(dev, l, r);
}

void ServerDriverHostWrapper::SetDisplayProjectionRaw(uint32_t dev, const vr::HmdRect2_t &l, const vr::HmdRect2_t &r) {
    m_real->SetDisplayProjectionRaw(dev, l, r);
}

void ServerDriverHostWrapper::SetRecommendedRenderTargetSize(uint32_t dev, uint32_t w, uint32_t h) {
    GF_LOG("Host::SetRecommendedRenderTargetSize dev=%u %ux%u", dev, w, h);
    m_real->SetRecommendedRenderTargetSize(dev, w, h);
}

// ===========================================================================
// IVRDriverContext
void *DriverContextWrapper::GetGenericInterface(const char *version, vr::EVRInitError *peError) {
    void *iface = m_real->GetGenericInterface(version, peError);
    GF_LOG("Context::GetGenericInterface(\"%s\") -> %s", version ? version : "(null)", iface ? "ok" : "null");
    if (!iface || !version) return iface;

    if (std::strcmp(version, vr::IVRServerDriverHost_Version) == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_host)
            m_host = std::make_unique<ServerDriverHostWrapper>(static_cast<vr::IVRServerDriverHost *>(iface));
        return m_host.get();
    }
    if (std::strncmp(version, "IVRServerDriverHost_", 20) == 0) {
        GF_LOG("  !! driver pediu %s, conhecemos %s — sem interceptação de dispositivos",
                version, vr::IVRServerDriverHost_Version);
    }
    return iface;
}

vr::DriverHandle_t DriverContextWrapper::GetDriverHandle() { return m_real->GetDriverHandle(); }

// ===========================================================================
// IServerTrackedDeviceProvider
vr::EVRInitError ProviderWrapper::Init(vr::IVRDriverContext *ctx) {
    m_context = std::make_unique<DriverContextWrapper>(ctx);
    vr::EVRInitError r = m_real->Init(m_context.get());
    GF_LOG("Provider::Init -> %d", (int)r);
    return r;
}

void ProviderWrapper::Cleanup() {
    GF_LOG("Provider::Cleanup");
    m_real->Cleanup();
}

const char *const *ProviderWrapper::GetInterfaceVersions() {
    const char *const *v = m_real->GetInterfaceVersions();
    static bool logged = false;
    if (!logged && v) {
        logged = true;
        for (const char *const *p = v; *p; ++p) GF_LOG("Driver da Pico declara interface: %s", *p);
    }
    return v;
}

void ProviderWrapper::RunFrame() { m_real->RunFrame(); }
bool ProviderWrapper::ShouldBlockStandbyMode() { return m_real->ShouldBlockStandbyMode(); }
void ProviderWrapper::EnterStandby() { m_real->EnterStandby(); }
void ProviderWrapper::LeaveStandby() { m_real->LeaveStandby(); }

} // namespace gf
