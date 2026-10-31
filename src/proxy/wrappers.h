// Wrappers das interfaces OpenVR. Cada wrapper repassa 100% das chamadas para
// o objeto real do driver da Pico e só observa (log + estatísticas).
//
// Regra de ouro: se a versão de uma interface pedida pelo driver da Pico não
// for exatamente a que conhecemos, NÃO embrulhamos — devolvemos o objeto real
// e só registramos no log. Embrulhar uma vtable de versão diferente quebraria
// o SteamVR.
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace gf {

// Contador simples de taxa por janela de tempo (ex.: Present/s).
class RateStats {
public:
    explicit RateStats(const char *name) : m_name(name) {}
    // Registra um evento; a cada ~2 s escreve um resumo no log.
    void Tick();

private:
    const char *m_name;
    std::mutex m_mutex;
    double m_windowStart = 0.0;
    double m_last = 0.0;
    uint32_t m_count = 0;
    double m_minDt = 1e9, m_maxDt = 0.0, m_sumDt = 0.0;
};

// ---------------------------------------------------------------------------
class VirtualDisplayWrapper final : public vr::IVRVirtualDisplay {
public:
    explicit VirtualDisplayWrapper(vr::IVRVirtualDisplay *real) : m_real(real) {}

    void Present(const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize) override;
    void WaitForPresent() override;
    bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override;

private:
    vr::IVRVirtualDisplay *m_real;
    RateStats m_presentRate{"VirtualDisplay::Present"};
    std::atomic<uint64_t> m_calls{0};
};

// ---------------------------------------------------------------------------
class DirectModeWrapper final : public vr::IVRDriverDirectModeComponent {
public:
    explicit DirectModeWrapper(vr::IVRDriverDirectModeComponent *real) : m_real(real) {}

    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t *pThrottling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override;

private:
    vr::IVRDriverDirectModeComponent *m_real;
    RateStats m_presentRate{"DirectMode::Present"};
    std::atomic<uint64_t> m_presents{0};
    std::atomic<uint64_t> m_layers{0};
    std::atomic<uint32_t> m_layersThisFrame{0};
};

// ---------------------------------------------------------------------------
class DeviceWrapper final : public vr::ITrackedDeviceServerDriver {
public:
    DeviceWrapper(vr::ITrackedDeviceServerDriver *real, const char *serial, vr::ETrackedDeviceClass cls);

    vr::EVRInitError Activate(uint32_t unObjectId) override;
    void Deactivate() override;
    void EnterStandby() override;
    void *GetComponent(const char *pchComponentNameAndVersion) override;
    void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
    vr::DriverPose_t GetPose() override;

private:
    vr::ITrackedDeviceServerDriver *m_real;
    vr::ETrackedDeviceClass m_class;
    std::mutex m_mutex;
    std::unique_ptr<VirtualDisplayWrapper> m_virtualDisplay;
    std::unique_ptr<DirectModeWrapper> m_directMode;
};

// ---------------------------------------------------------------------------
class ServerDriverHostWrapper final : public vr::IVRServerDriverHost {
public:
    explicit ServerDriverHostWrapper(vr::IVRServerDriverHost *real) : m_real(real) {}

    bool TrackedDeviceAdded(const char *serial, vr::ETrackedDeviceClass cls, vr::ITrackedDeviceServerDriver *pDriver) override;
    void TrackedDevicePoseUpdated(uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize) override;
    void VsyncEvent(double vsyncTimeOffsetSeconds) override;
    void VendorSpecificEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t &eventData, double eventTimeOffset) override;
    bool IsExiting() override;
    bool PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent) override;
    void GetRawTrackedDevicePoses(float fPredictedSecondsFromNow, vr::TrackedDevicePose_t *pArray, uint32_t unCount) override;
    void RequestRestart(const char *pchLocalizedReason, const char *pchExecutableToStart, const char *pchArguments, const char *pchWorkingDirectory) override;
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming *pTiming, uint32_t nFrames) override;
    void SetDisplayEyeToHead(uint32_t unWhichDevice, const vr::HmdMatrix34_t &eyeToHeadLeft, const vr::HmdMatrix34_t &eyeToHeadRight) override;
    void SetDisplayProjectionRaw(uint32_t unWhichDevice, const vr::HmdRect2_t &eyeLeft, const vr::HmdRect2_t &eyeRight) override;
    void SetRecommendedRenderTargetSize(uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight) override;

private:
    vr::IVRServerDriverHost *m_real;
    std::mutex m_mutex;
    std::vector<std::unique_ptr<DeviceWrapper>> m_devices;
    RateStats m_hmdPoseRate{"HMD TrackedDevicePoseUpdated"};
    RateStats m_vsyncRate{"VsyncEvent"};
};

// ---------------------------------------------------------------------------
class DriverContextWrapper final : public vr::IVRDriverContext {
public:
    explicit DriverContextWrapper(vr::IVRDriverContext *real) : m_real(real) {}

    void *GetGenericInterface(const char *pchInterfaceVersion, vr::EVRInitError *peError) override;
    vr::DriverHandle_t GetDriverHandle() override;

private:
    vr::IVRDriverContext *m_real;
    std::mutex m_mutex;
    std::unique_ptr<ServerDriverHostWrapper> m_host;
};

// ---------------------------------------------------------------------------
class ProviderWrapper final : public vr::IServerTrackedDeviceProvider {
public:
    explicit ProviderWrapper(vr::IServerTrackedDeviceProvider *real) : m_real(real) {}

    vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override;
    void Cleanup() override;
    const char *const *GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

private:
    vr::IServerTrackedDeviceProvider *m_real;
    std::unique_ptr<DriverContextWrapper> m_context;
};

} // namespace gf
