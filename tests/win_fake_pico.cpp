// Pico Connect falso do teste ponta a ponta no Windows (win_fake_vrserver.cpp). Como o
// de verdade: HMD com Display 002 e DirectMode 008, nada de VirtualDisplay. As texturas
// dos swap sets são D3D11 de verdade, compartilhadas por handle legado, num device só
// deste "driver". O proxy carrega esta DLL como "<nome>_real.dll".
//
// O Present lê de volta a textura da cena (olho 0) e guarda o que chegou: handle, se é de
// um swap set pedido pelo próprio processo (o proxy roda no mesmo processo do teste) e o
// x médio do brilho. O teste pega isso pelas funções FakePico_*.
#include <openvr_driver.h>
#include <openvr_driver_legado.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstring>

#define FAKE_EXPORT extern "C" __declspec(dllexport)

using Microsoft::WRL::ComPtr;
using DM8 = vr::legado::IVRDriverDirectModeComponent;

namespace {

struct Registro {
    uint64_t handle;
    int doProxy;
    float centroX; // pixels a partir da borda esquerda da região do olho 0; -1 = não leu
};
constexpr int kMaxRegistros = 8192;
Registro g_reg[kMaxRegistros];
int g_nReg = 0, g_postPresents = 0, g_conjuntosDoProxy = 0, g_syncFalhou = 0;

struct Conjunto {
    ComPtr<ID3D11Texture2D> tex[3];
    vr::SharedTextureHandle_t h[3] = {};
    uint32_t pid = 0;
    uint32_t ultimo = 2; // índice apresentado por último; o próximo é o seguinte
};
Conjunto g_conj[16];

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
vr::SharedTextureHandle_t g_hSync = 0;
ComPtr<IDXGIKeyedMutex> g_sync;

bool Device() {
    return g_ctx || SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, g_dev.GetAddressOf(), nullptr,
                                                g_ctx.GetAddressOf()));
}

Conjunto *Achar(vr::SharedTextureHandle_t h, uint32_t *idx) {
    for (Conjunto &c : g_conj)
        for (uint32_t i = 0; i < 3; ++i)
            if (h && c.h[i] == h) {
                if (idx) *idx = i;
                return &c;
            }
    return nullptr;
}

// x médio do brilho (r+g+b) na região `b`, lendo a textura por uma cópia em staging.
float CentroX(ID3D11Texture2D *t, const vr::VRTextureBounds_t &b) {
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, st.GetAddressOf()))) return -1;
    g_ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return -1;
    const UINT x0 = UINT(b.uMin * float(d.Width) + 0.5f), x1 = UINT(b.uMax * float(d.Width) + 0.5f);
    const UINT y0 = UINT(b.vMin * float(d.Height) + 0.5f), y1 = UINT(b.vMax * float(d.Height) + 0.5f);
    double soma = 0, somaX = 0;
    for (UINT y = y0; y < y1 && y < d.Height; ++y) {
        const uint8_t *linha = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
        for (UINT x = x0; x < x1 && x < d.Width; ++x) {
            const uint8_t *p = linha + size_t(x) * 4;
            const double i = double(p[0]) + p[1] + p[2];
            soma += i;
            somaX += i * (double(x - x0) + 0.5);
        }
    }
    g_ctx->Unmap(st.Get(), 0);
    return soma > 0 ? float(somaX / soma) : -1.0f;
}

class FakeDM8 : public DM8 {
public:
    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *d, SwapTextureSet_t *o) override {
        *o = {};
        Conjunto *c = nullptr;
        for (Conjunto &x : g_conj)
            if (!x.h[0]) {
                c = &x;
                break;
            }
        if (!c || !d || !Device()) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = d->nWidth;
        td.Height = d->nHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = static_cast<DXGI_FORMAT>(d->nFormat);
        td.SampleDesc.Count = d->nSampleCount ? d->nSampleCount : 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        for (int i = 0; i < 3; ++i) {
            ComPtr<IDXGIResource> r;
            HANDLE h = nullptr;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, c->tex[i].ReleaseAndGetAddressOf())) ||
                FAILED(c->tex[i].As(&r)) || FAILED(r->GetSharedHandle(&h)) || !h) {
                *c = Conjunto();
                *o = {};
                return;
            }
            c->h[i] = o->rSharedTextureHandles[i] = vr::SharedTextureHandle_t(reinterpret_cast<uintptr_t>(h));
        }
        c->pid = pid;
        c->ultimo = 2;
        if (pid == GetCurrentProcessId()) g_conjuntosDoProxy++;
    }
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override {
        if (Conjunto *c = Achar(h, nullptr)) *c = Conjunto();
    }
    void DestroyAllSwapTextureSets(uint32_t pid) override {
        for (Conjunto &c : g_conj)
            if (c.h[0] && c.pid == pid) c = Conjunto();
    }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t h[2], uint32_t (*idx)[2]) override {
        for (int e = 0; e < 2; ++e) {
            const Conjunto *c = Achar(h[e], nullptr);
            (*idx)[e] = c ? (c->ultimo + 1) % 3 : 0;
        }
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&e)[2]) override {
        if (m_camadas++ == 0) std::memcpy(m_primeira, e, sizeof(m_primeira));
    }
    void Present(vr::SharedTextureHandle_t sync) override {
        // Como um driver de verdade: espera o compositor soltar o sync (keyed mutex, chave 0).
        if (sync && sync != g_hSync && Device()) {
            ComPtr<ID3D11Texture2D> t;
            g_sync.Reset();
            g_hSync = 0;
            HANDLE h = reinterpret_cast<HANDLE>(uintptr_t(sync));
            if (SUCCEEDED(g_dev->OpenSharedResource(h, IID_PPV_ARGS(t.GetAddressOf()))) && SUCCEEDED(t.As(&g_sync)))
                g_hSync = sync;
        }
        if (sync && (!g_sync || g_sync->AcquireSync(0, 100) != S_OK)) g_syncFalhou++;
        else if (sync) g_sync->ReleaseSync(0);

        Registro r{m_camadas ? m_primeira[0].hTexture : 0, 0, -1.0f};
        uint32_t idx = 0;
        if (Conjunto *c = Achar(r.handle, &idx)) {
            r.doProxy = c->pid == GetCurrentProcessId();
            c->ultimo = idx;
            r.centroX = CentroX(c->tex[idx].Get(), m_primeira[0].bounds);
        }
        m_camadas = 0;
        if (g_nReg < kMaxRegistros) g_reg[g_nReg++] = r;
    }
    void PostPresent(const Throttling_t *) override { g_postPresents++; }
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *) override {}

private:
    uint32_t m_camadas = 0;
    SubmitLayerPerEye_t m_primeira[2] = {};
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

class FakeHmd : public vr::ITrackedDeviceServerDriver {
public:
    vr::EVRInitError Activate(uint32_t) override { return vr::VRInitError_None; }
    void Deactivate() override {}
    void EnterStandby() override {}
    void *GetComponent(const char *n) override {
        if (!std::strcmp(n, vr::legado::IVRDriverDirectModeComponent_Version)) return &dm;
        if (!std::strcmp(n, vr::legado::IVRDisplayComponent_Version)) return &disp;
        return nullptr; // IVRVirtualDisplay_002 inclusive, como o Pico de verdade
    }
    void DebugRequest(const char *, char *b, uint32_t s) override { if (s) b[0] = 0; }
    vr::DriverPose_t GetPose() override { return {}; }
    FakeDM8 dm;
    FakeDisplay2 disp;
};

class FakeProvider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *ctx) override {
        auto *host = static_cast<vr::IVRServerDriverHost *>(ctx->GetGenericInterface(vr::IVRServerDriverHost_Version, nullptr));
        if (!host) return vr::VRInitError_Init_InterfaceNotFound;
        host->TrackedDeviceAdded("PICO-FAKE-WIN", vr::TrackedDeviceClass_HMD, &hmd);
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        for (Conjunto &c : g_conj) c = Conjunto();
        g_sync.Reset();
        g_hSync = 0;
        g_ctx.Reset();
        g_dev.Reset();
    }
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {}
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
    FakeHmd hmd;
};

FakeProvider g_provider;

} // namespace

FAKE_EXPORT void *HmdDriverFactory(const char *name, int *rc) {
    if (!std::strcmp(name, vr::IServerTrackedDeviceProvider_Version)) {
        *rc = 0;
        return &g_provider;
    }
    *rc = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

// Para o teste conferir que o proxy devolveu um wrapper, não este objeto.
FAKE_EXPORT void *FakePico_DM8() { return &g_provider.hmd.dm; }
FAKE_EXPORT int FakePico_Presents() { return g_nReg; }
FAKE_EXPORT int FakePico_PostPresents() { return g_postPresents; }
FAKE_EXPORT int FakePico_ConjuntosDoProxy() { return g_conjuntosDoProxy; }
FAKE_EXPORT int FakePico_SyncFalhou() { return g_syncFalhou; }
FAKE_EXPORT int FakePico_Registro(int i, uint64_t *handle, int *doProxy, float *centroX) {
    if (i < 0 || i >= g_nReg) return 0;
    *handle = g_reg[i].handle;
    *doProxy = g_reg[i].doProxy;
    *centroX = g_reg[i].centroX;
    return 1;
}
