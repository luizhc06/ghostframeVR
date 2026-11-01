// Wrappers das interfaces OpenVR. Cada wrapper repassa 100% das chamadas para
// o objeto real do driver da Pico e só observa (log + estatísticas).
//
// Regra de ouro: se a versão de uma interface pedida pelo driver da Pico não
// for exatamente a que conhecemos, NÃO embrulhamos — devolvemos o objeto real
// e só registramos no log. Embrulhar uma vtable de versão diferente quebraria
// o SteamVR.
#pragma once

#include "framegen.h"

#include <openvr_driver.h>
#include <openvr_driver_legado.h>

#include <atomic>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
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
// Com o frame gen ligado (ghostframe.ini), as camadas do quadro ficam guardadas até o
// Present: é ali que se decide se o quadro é novo (vai para o histórico do motor) ou
// repetido (troca a camada da cena por um frame gerado). Com a trava do SteamVR
// (framesToThrottle = N), o PostPresent entrega N frames gerados, um por vsync.
// Desligado, tudo é repassado na hora, exatamente como antes.
class DirectModeWrapper final : public vr::IVRDriverDirectModeComponent {
public:
    explicit DirectModeWrapper(vr::IVRDriverDirectModeComponent *real) : m_real(real) {}
    ~DirectModeWrapper();

    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t *pThrottling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override;

private:
    using Camada = SubmitLayerPerEye_t[2];
    struct Conjunto {
        vr::SharedTextureHandle_t h[3];
        SwapTextureSetDesc_t desc;
        uint32_t flags;
        uint32_t pid;
    };
    static constexpr uint32_t kMaxCamadas = 16, kMaxConjuntos = 64;

    bool AcharConjunto(vr::SharedTextureHandle_t h, Conjunto *out);
    bool MesmoConjunto(vr::SharedTextureHandle_t a, vr::SharedTextureHandle_t b);
    bool GarantirSaida(const Camada &cena);
    void LiberarSaida();
    bool Motor();
    bool MotorOk() const { return m_motor.load() == 2; }
    void Esquecidos();
    void Reenviar(const Camada *camadas, uint32_t n, double atraso);
    bool GerarAdiante(uint32_t k);
    bool EntregarGerado(const Camada *camadas, uint32_t n);
    void Falhou(const char *motivo);
    void EsperarAte(double t);

    vr::IVRDriverDirectModeComponent *m_real;
    RateStats m_presentRate{"DirectMode::Present"};
    std::atomic<uint64_t> m_presents{0};
    std::atomic<uint64_t> m_layers{0};
    std::atomic<uint32_t> m_layersThisFrame{0};

    // Swap sets criados pelo driver (para achar formato/flags da textura da cena).
    std::mutex m_mutexConjuntos;
    Conjunto m_conjuntos[kMaxConjuntos] = {};
    uint32_t m_nConjuntos = 0;

    // Estado do frame gen: só a thread do compositor (SubmitLayer/Present/PostPresent) mexe.
    FrameGen m_fg;
    bool m_quadroComFG = false;  // decidido no 1º SubmitLayer do quadro
    Camada m_camadas[kMaxCamadas];
    uint32_t m_nCamadas = 0;
    uint32_t m_nUltimas = 0;     // camadas do último quadro real (reenviadas nos gerados)
    Camada m_ultimas[kMaxCamadas];
    vr::SharedTextureHandle_t m_sync = 0;
    bool m_ultimoFoiNovo = false;
    uint32_t m_repeticoes = 0;
    double m_tUltimoReal = 0, m_intervaloJogo = 0, m_tUltimoPresent = 0, m_intervaloPresent = 0;
    double m_tPresent = 0;       // último Present do compositor, ligado ou não (base da trava)
    uint32_t m_divisor = 2;      // frames por frame real esperados (trava + 1); alfa = k / divisor
    Conjunto m_saida[2] = {};
    bool m_temSaida = false, m_saidaUnica = false, m_saidaNt = false;
    uint32_t m_idxSaida = 0;
    bool m_pendente = false;     // há um frame gerado na GPU esperando ser entregue
    uint32_t m_idxPendente = 0, m_kPendente = 0, m_divisorPendente = 0;
    uint32_t m_falhasSeguidas = 0;  // erros (texturas, NovoFrame, Gerar); zera ao gerar
    uint32_t m_semEntrega = 0;      // gerado não ficou pronto a tempo; zera ao entregar
    bool m_desistiu = false;
    int32_t m_proximoIdx = -1;      // índice que o driver deu para o swap set de saída
    // O motor inicia numa thread à parte (cria device e compila shaders: centenas de ms
    // que não podem parar o Present). 0 nada, 1 iniciando, 2 pronto, 3 falhou.
    std::atomic<int> m_motor{0};
    std::thread m_inicio;
    // Destroy* pode vir de outra thread: os handles vão para esta fila e o Present
    // (única thread que mexe no motor) solta as texturas abertas por eles.
    static constexpr uint32_t kMaxEsquecer = 32;
    vr::SharedTextureHandle_t m_esquecer[kMaxEsquecer] = {};
    uint32_t m_nEsquecer = 0;
    bool m_esquecerTudo = false;
};

// ---------------------------------------------------------------------------
// DirectMode 008 (Pico Connect e outros drivers do SDK 1.23 a 2.2). A vtable é a mesma
// da 009; só o SubmitLayerPerEye_t é menor (não tinha a predição da pose no fim). Em vez
// de duplicar o frame gen, a 008 passa por dois tradutores:
//   SteamVR -> Frente008 (008->009) -> DirectModeWrapper -> Driver008 (009->008) -> driver
class Driver008 final : public vr::IVRDriverDirectModeComponent {
public:
    explicit Driver008(vr::legado::IVRDriverDirectModeComponent *real) : m_real(real) {}

    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override { m_real->DestroySwapTextureSet(h); }
    void DestroyAllSwapTextureSets(uint32_t unPid) override { m_real->DestroyAllSwapTextureSets(unPid); }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) override {
        m_real->GetNextSwapTextureSetIndex(handles, pIndices);
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override { m_real->Present(syncTexture); }
    void PostPresent(const Throttling_t *pThrottling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override { m_real->GetFrameTiming(pFrameTiming); }

private:
    vr::legado::IVRDriverDirectModeComponent *m_real;
};

class Frente008 final : public vr::legado::IVRDriverDirectModeComponent {
public:
    explicit Frente008(vr::IVRDriverDirectModeComponent *dm) : m_dm(dm) {}

    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override { m_dm->DestroySwapTextureSet(h); }
    void DestroyAllSwapTextureSets(uint32_t unPid) override { m_dm->DestroyAllSwapTextureSets(unPid); }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) override {
        m_dm->GetNextSwapTextureSetIndex(handles, pIndices);
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override { m_dm->Present(syncTexture); }
    void PostPresent(const Throttling_t *pThrottling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override { m_dm->GetFrameTiming(pFrameTiming); }

private:
    vr::IVRDriverDirectModeComponent *m_dm;
};

// ---------------------------------------------------------------------------
// Embrulha os componentes de vídeo (VirtualDisplay / DirectMode) que um driver devolve,
// venha o pedido do HMD, de um DisplayRedirect ou do plugin do compositor.
class Componentes {
public:
    void *Embrulhar(const char *quem, const char *nome, void *comp);

private:
    std::mutex m_mutex;
    std::unique_ptr<VirtualDisplayWrapper> m_virtualDisplay;
    std::unique_ptr<DirectModeWrapper> m_directMode;
    std::unique_ptr<Driver008> m_driver008;
    std::unique_ptr<DirectModeWrapper> m_directMode008;
    std::unique_ptr<Frente008> m_frente008;
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
    Componentes m_comp;
};

// ---------------------------------------------------------------------------
// Drivers que entregam o vídeo dentro do vrcompositor.exe (o SteamVR carrega a mesma
// DLL lá e pede IVRCompositorPluginProvider): o DirectMode deles vem por aqui.
class CompositorPluginWrapper final : public vr::IVRCompositorPluginProvider {
public:
    explicit CompositorPluginWrapper(vr::IVRCompositorPluginProvider *real) : m_real(real) {}

    vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override;
    void Cleanup() override;
    const char *const *GetInterfaceVersions() override;
    void *GetComponent(const char *pchComponentNameAndVersion) override;

private:
    vr::IVRCompositorPluginProvider *m_real;
    Componentes m_comp;
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
