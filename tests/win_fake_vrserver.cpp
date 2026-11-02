// "vrserver" e compositor falsos no Windows, ponta a ponta com GPU: carrega o proxy de
// verdade, que carrega o Pico falso (win_fake_pico.cpp, "<nome>_real.dll"), e faz o papel
// do compositor do SteamVR com D3D11 de verdade: swap sets criados pelo driver, sync com
// keyed mutex, jogo a meia taxa. Confere o que chega ao driver: o frame real intacto e,
// no lugar do repetido, o frame gerado pelo motor.
// Máquina sem device de hardware que compartilhe texturas entre devices: SKIP (código 77).
#include "cena_sintetica.h"
#include "framegen_ref.h"
#include "platform.h"

#include <openvr_driver.h>
#include <openvr_driver_legado.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using DM8 = vr::legado::IVRDriverDirectModeComponent;

namespace {

int g_fail = 0;
void Check(bool ok, const char *expr, int linha) {
    std::printf("%s: %s (linha %d)\n", ok ? "ok" : "FALHOU", expr, linha);
    if (!ok) g_fail++;
}
#define CHECK(c) Check((c), #c, __LINE__)

// Os dois olhos lado a lado numa textura. Quadrado claro e texturizado sobre preto, andando
// 8 px por frame do jogo (2 pixels de luma: o fluxo por blocos acha o vetor exato); depois
// da última posição volta ao começo.
constexpr int kW = 1024, kH = 256, kOlho = 512;
constexpr int kLado = 96, kY0 = 80, kX0 = 32, kPasso = 8;
constexpr int kPosicoes = (kOlho - 2 * kX0 - kLado) / kPasso + 1;
constexpr DXGI_FORMAT kFormato = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // o do compositor do SteamVR
constexpr uint32_t kPidCompositor = 4242;                       // qualquer um diferente do nosso
constexpr double kVsync = 0.030; // folgado: no CI a "GPU" pode ser o rasterizador em software
const vr::VRTextureBounds_t kBounds[2] = {{0.0f, 0.0f, 0.5f, 1.0f}, {0.5f, 0.0f, 1.0f, 1.0f}};

int X(int jogo) { return kX0 + kPasso * (jogo % kPosicoes); }

void Desenha(int jogo, std::vector<uint8_t> &img) {
    img.assign(size_t(kW) * kH * 4, 0);
    for (int olho = 0; olho < 2; ++olho)
        for (int y = 0; y < kLado; ++y)
            for (int x = 0; x < kLado; ++x) {
                // Liso, o fluxo não veria o movimento dentro do quadrado (tudo empata no zero).
                const float n = cena::Ruido(x / 8.0, y / 8.0, 7);
                uint8_t *p = &img[(size_t(kY0 + y) * kW + size_t(olho) * kOlho + size_t(X(jogo) + x)) * 4];
                p[0] = p[1] = p[2] = uint8_t((0.5f + 0.5f * n) * 255 + 0.5f);
                p[3] = 255;
            }
}

// x médio do brilho (r+g+b) nos w×h primeiros pixels: a mesma conta do driver falso.
double CentroX(const uint8_t *img, size_t stride, int w, int h) {
    double soma = 0, somaX = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t *p = img + size_t(y) * stride + size_t(x) * 4;
            const double i = double(p[0]) + p[1] + p[2];
            soma += i;
            somaX += i * (x + 0.5);
        }
    return soma > 0 ? somaX / soma : -1;
}

// Só a translação muda entre frames do jogo: o motor compensa só rotação, então todo o
// movimento que ele vê é o do quadrado. Pose diferente = quadro novo para o proxy.
vr::HmdMatrix34_t Pose(int jogo) {
    vr::HmdMatrix34_t m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    m.m[0][3] = 0.001f * float(jogo);
    m.m[1][3] = 1.7f;
    return m;
}

// Quanto o quadrado anda no frame gerado (alfa 1/2) depois do frame `jogo`, pela referência
// em CPU (framegen_ref, o algoritmo dos shaders). Fica abaixo dos +4 px ideais: a mediana
// dos vetores segura as bordas de um objeto sobre fundo liso, só o miolo anda 4.
double DeslocRef(int jogo) {
    gf::ref::Olho o;
    std::vector<uint8_t> a, b, gerado(size_t(kOlho) * kH * 4);
    Desenha(jogo - 1, a);
    Desenha(jogo, b);
    gf::ref::NovoFrame(o, a.data(), kW * 4, kOlho, kH, Pose(jogo - 1));
    gf::ref::NovoFrame(o, b.data(), kW * 4, kOlho, kH, Pose(jogo));
    if (!gf::ref::Gerar(o, 0.5f, gerado.data(), kOlho * 4)) return 0;
    return CentroX(gerado.data(), kOlho * 4, kOlho, kH) - CentroX(b.data(), kW * 4, kOlho, kH);
}

std::string Hex(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

bool CriaDevice(ComPtr<ID3D11Device> &dev, ComPtr<ID3D11DeviceContext> &ctx, HRESULT &hr) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                           dev.GetAddressOf(), nullptr, ctx.GetAddressOf());
    return SUCCEEDED(hr);
}

std::string Adaptador(ID3D11Device *dev) {
    ComPtr<IDXGIDevice> d;
    ComPtr<IDXGIAdapter> a;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(d.GetAddressOf()))) || FAILED(d->GetAdapter(a.GetAddressOf())) ||
        FAILED(a->GetDesc(&desc)))
        return "?";
    char nome[128] = {}, r[200];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nome, int(sizeof(nome)), nullptr, nullptr);
    std::snprintf(r, sizeof(r), "%s (vendor 0x%04x, device 0x%04x)", nome, desc.VendorId, desc.DeviceId);
    return r;
}

ComPtr<ID3D11Texture2D> CriaTextura(ID3D11Device *dev, UINT w, UINT h, UINT misc, const void *dados) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = kFormato;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = misc;
    D3D11_SUBRESOURCE_DATA sd{dados, w * 4, 0};
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, dados ? &sd : nullptr, t.GetAddressOf()))) return nullptr;
    return t;
}

vr::SharedTextureHandle_t Handle(ID3D11Texture2D *t) {
    ComPtr<IDXGIResource> r;
    HANDLE h = nullptr;
    if (!t || FAILED(t->QueryInterface(IID_PPV_ARGS(r.GetAddressOf()))) || FAILED(r->GetSharedHandle(&h))) return 0;
    return vr::SharedTextureHandle_t(reinterpret_cast<uintptr_t>(h));
}

HRESULT Abre(ID3D11Device *dev, vr::SharedTextureHandle_t h, ComPtr<ID3D11Texture2D> &t) {
    return dev->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(h)), IID_PPV_ARGS(t.ReleaseAndGetAddressOf()));
}

// Espera a GPU deste contexto terminar.
void Espera(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> q;
    if (FAILED(dev->CreateQuery(&qd, q.GetAddressOf()))) return;
    ctx->End(q.Get());
    BOOL feito = FALSE;
    const double t0 = gf::NowSeconds();
    while ((ctx->GetData(q.Get(), &feito, sizeof(feito), 0) != S_OK || !feito) && gf::NowSeconds() - t0 < 10) Sleep(0);
}

std::vector<uint8_t> Le(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t) {
    std::vector<uint8_t> out;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, st.GetAddressOf()))) return out;
    ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    out.resize(size_t(d.Width) * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(&out[size_t(y) * d.Width * 4], static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch,
                    size_t(d.Width) * 4);
    ctx->Unmap(st.Get(), 0);
    return out;
}

// O caminho real precisa de device de hardware que compartilhe textura por handle legado
// com outro device, com keyed mutex (o WARP não faz isso). Descobre antes de começar.
// Numa GPU de verdade a escrita entra numa fila: a sonda escreve como o compositor falso
// (UpdateSubresource + espera a GPU) e, para diagnóstico, também do jeito antigo (conteúdo na
// criação) e com o keyed mutex na própria textura. Imprime o que cada jeito deu.
std::string Diferenca(const std::vector<uint8_t> &lido, const std::vector<uint8_t> &esperado) {
    if (lido.size() != esperado.size()) return "leitura falhou";
    size_t n = 0, primeiro = lido.size();
    for (size_t i = 0; i < lido.size(); ++i)
        if (lido[i] != esperado[i]) { if (!n++) primeiro = i; }
    if (!n) return "ok";
    char b[160];
    std::snprintf(b, sizeof(b), "%zu de %zu bytes diferentes (1o em %zu: lido %u, esperado %u)", n, lido.size(),
                  primeiro, unsigned(lido[primeiro]), unsigned(esperado[primeiro]));
    return b;
}

bool Sonda(std::string &motivo) {
    ComPtr<ID3D11Device> a, b;
    ComPtr<ID3D11DeviceContext> ca, cb;
    HRESULT hr = S_OK;
    if (!CriaDevice(a, ca, hr) || !CriaDevice(b, cb, hr)) {
        motivo = "D3D11CreateDevice(D3D_DRIVER_TYPE_HARDWARE) falhou: " + Hex(hr);
        return false;
    }
    const std::string adaptador = Adaptador(a.Get());
    std::printf("adaptador padrão: %s\n", adaptador.c_str());
    std::vector<uint8_t> px(64 * 64 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7 + 3);

    // 1) Como o compositor falso: textura compartilhada vazia, UpdateSubresource, espera a GPU.
    ComPtr<ID3D11Texture2D> ta = CriaTextura(a.Get(), 64, 64, D3D11_RESOURCE_MISC_SHARED, nullptr), tb;
    const vr::SharedTextureHandle_t h = Handle(ta.Get());
    if (!h) {
        motivo = "o adaptador " + adaptador + " não cria textura compartilhada (MISC_SHARED + GetSharedHandle)";
        return false;
    }
    if (FAILED(hr = Abre(b.Get(), h, tb))) {
        motivo = "no adaptador " + adaptador + " outro device não abre a textura compartilhada: " + Hex(hr);
        return false;
    }
    ca->UpdateSubresource(ta.Get(), 0, nullptr, px.data(), 64 * 4, 0);
    ca->Flush();
    Espera(a.Get(), ca.Get());
    std::string r1 = Diferenca(Le(b.Get(), cb.Get(), tb.Get()), px);
    // Se chegou diferente, a escrita pode só estar atrasada: lê de novo depois de um tempo.
    std::string r1b = r1;
    for (int i = 0; i < 5 && r1b != "ok"; ++i) {
        Sleep(20);
        r1b = Diferenca(Le(b.Get(), cb.Get(), tb.Get()), px);
    }

    // 2) Jeito antigo: conteúdo passado na criação da textura compartilhada.
    ComPtr<ID3D11Texture2D> ia = CriaTextura(a.Get(), 64, 64, D3D11_RESOURCE_MISC_SHARED, px.data()), ib;
    Espera(a.Get(), ca.Get());
    std::string r2 = Handle(ia.Get()) && SUCCEEDED(Abre(b.Get(), Handle(ia.Get()), ib)) ? Diferenca(Le(b.Get(), cb.Get(), ib.Get()), px) : "não abriu";

    // 3) Keyed mutex na própria textura (escrita e leitura com a chave).
    ComPtr<ID3D11Texture2D> ka = CriaTextura(a.Get(), 64, 64, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, nullptr), kb;
    const vr::SharedTextureHandle_t hk = Handle(ka.Get());
    ComPtr<IDXGIKeyedMutex> kma, kmb;
    std::string r3 = "não abriu";
    if (hk && SUCCEEDED(Abre(b.Get(), hk, kb)) && SUCCEEDED(ka.As(&kma)) && SUCCEEDED(kb.As(&kmb))) {
        if (kma->AcquireSync(0, 1000) == S_OK) {
            ca->UpdateSubresource(ka.Get(), 0, nullptr, px.data(), 64 * 4, 0);
            kma->ReleaseSync(1);
            if (kmb->AcquireSync(1, 1000) == S_OK) {
                r3 = Diferenca(Le(b.Get(), cb.Get(), kb.Get()), px);
                kmb->ReleaseSync(0);
            } else {
                r3 = "o outro device não pegou a chave";
            }
        } else {
            r3 = "não pegou a chave";
        }
    }
    std::printf("sonda: escrita+espera=%s; relida=%s; conteúdo na criação=%s; keyed mutex=%s\n", r1.c_str(),
                r1b.c_str(), r2.c_str(), r3.c_str());
    // O teste usa o jeito 1 (como o SteamVR: escreve, espera, solta o sync). Se nem relendo
    // ele funcionar, os números do teste não valeriam nesta máquina.
    if (r1b != "ok") {
        motivo = "no adaptador " + adaptador + " a textura compartilhada chega diferente no outro device (escrita+espera: " + r1b + ")";
        return false;
    }
    if (r3 == "não abriu" || r3 == "não pegou a chave" || r3 == "o outro device não pegou a chave") {
        motivo = "no adaptador " + adaptador + " não há keyed mutex entre devices (" + r3 + ")";
        return false;
    }
    return true;
}

// ---- SteamVR falso: host e contexto que o driver recebe (como no fake_vrserver do Linux) ----
vr::ITrackedDeviceServerDriver *g_hmd = nullptr;

class Host : public vr::IVRServerDriverHost {
public:
    bool TrackedDeviceAdded(const char *, vr::ETrackedDeviceClass c, vr::ITrackedDeviceServerDriver *d) override {
        if (c == vr::TrackedDeviceClass_HMD) g_hmd = d;
        return true;
    }
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

// Propriedades: como o Pico, o headset informa os Hz (o proxy usa no relógio próprio). Um vsync
// do teste dura kVsync, então o "headset" roda a 1/kVsync Hz.
class Props : public vr::IVRProperties {
public:
    vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t c, vr::PropertyRead_t *b, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            b[i].eError = vr::TrackedProp_UnknownProperty;
            if (c == 77 && b[i].prop == vr::Prop_DisplayFrequency_Float && b[i].unBufferSize >= sizeof(float)) {
                *static_cast<float *>(b[i].pvBuffer) = float(1.0 / kVsync);
                b[i].unRequiredBufferSize = sizeof(float);
                b[i].unTag = vr::k_unFloatPropertyTag;
                b[i].eError = vr::TrackedProp_Success;
            }
        }
        return vr::TrackedProp_Success;
    }
    vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t, vr::PropertyWrite_t *b, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) b[i].eError = vr::TrackedProp_Success;
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

// ---- O que o Pico falso viu (funções exportadas por ele) ----
template <class F> F Funcao(HMODULE m, const char *nome) {
    return reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(m, nome)));
}

struct Fake {
    void *(*dm8)();
    int (*presents)();
    int (*postPresents)();
    int (*conjuntosDoProxy)();
    int (*syncFalhou)();
    int (*registro)(int, uint64_t *, int *, float *);
    bool Ok() const { return dm8 && presents && postPresents && conjuntosDoProxy && syncFalhou && registro; }
};

struct Reg {
    uint64_t h;
    int doProxy;
    float cx;
};

std::vector<Reg> Registros(const Fake &f, int de) {
    std::vector<Reg> r;
    Reg x{};
    for (int i = de; f.registro(i, &x.h, &x.doProxy, &x.cx); ++i) r.push_back(x);
    return r;
}

// ---- Compositor falso ----
struct Compositor {
    DM8 *dm = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    vr::SharedTextureHandle_t h[3] = {};
    ComPtr<ID3D11Texture2D> tex[3];
    vr::SharedTextureHandle_t hSync = 0;
    ComPtr<ID3D11Texture2D> sync;
    ComPtr<IDXGIKeyedMutex> km;
    uint32_t idx = 0;
    int syncOcupado = 0; // vezes que o compositor não pegou o sync de volta
    std::vector<uint8_t> img;
};

// Um vsync: desenha o frame `jogo` na textura que o driver indicou, submete com a pose dele,
// Present e PostPresent (trava = nFramesToThrottle). Devolve o handle submetido.
double g_tPresent = 0; // quando o último Vsync chamou Present

vr::SharedTextureHandle_t Vsync(Compositor &c, int jogo, uint32_t trava) {
    const uint32_t i = c.idx;
    Desenha(jogo, c.img);
    const bool chave = c.km->AcquireSync(0, 1000) == S_OK;
    c.syncOcupado += !chave;
    c.ctx->UpdateSubresource(c.tex[i].Get(), 0, nullptr, c.img.data(), kW * 4, 0);
    Espera(c.dev.Get(), c.ctx.Get()); // o driver falso lê a textura sem passar pelo sync
    if (chave) c.km->ReleaseSync(0);
    DM8::SubmitLayerPerEye_t e[2] = {};
    for (int olho = 0; olho < 2; ++olho) {
        e[olho].hTexture = c.h[i];
        e[olho].bounds = kBounds[olho];
        e[olho].mHmdPose = Pose(jogo);
    }
    c.dm->SubmitLayer(e);
    g_tPresent = gf::NowSeconds(); // daqui em diante o tempo é do proxy e do driver
    c.dm->Present(c.hSync);
    // Como o compositor do SteamVR (visto num log real com o Pico): já pega o sync para o
    // próximo quadro e o segura enquanto o PostPresent roda. Gerado que dependesse dele travaria.
    const bool segura = c.km->AcquireSync(0, 1000) == S_OK;
    const DM8::Throttling_t t{trava, 0};
    c.dm->PostPresent(&t);
    if (segura) c.km->ReleaseSync(0);
    vr::SharedTextureHandle_t hs[2] = {c.h[0], c.h[0]};
    uint32_t prox[2] = {3, 3};
    c.dm->GetNextSwapTextureSetIndex(hs, &prox);
    c.idx = prox[0] < 3 ? prox[0] : (i + 1) % 3;
    return c.h[i];
}

double g_centro[kPosicoes], g_ref[kPosicoes];

// Real intacto: o handle do compositor e o quadrado onde ele desenhou.
bool Intacto(const Reg &r, vr::SharedTextureHandle_t h, int jogo) {
    return r.h == h && !r.doProxy && std::fabs(r.cx - g_centro[jogo % kPosicoes]) < 0.01;
}

struct Desloc {
    std::vector<double> medido, esperado;
    // Gerado depois do frame `jogo`: quanto o quadrado andou além do último real.
    void Anota(const Reg &r, int jogo) {
        const int p = jogo % kPosicoes;
        if (p == 0) return; // o quadrado voltou ao começo: o par não é movimento
        medido.push_back(r.cx - g_centro[p]);
        esperado.push_back(g_ref[p]);
    }
    void Confere(const char *nome) {
        CHECK(!medido.empty());
        if (medido.empty()) return;
        std::vector<double> erro;
        double m = 0, r = 0;
        for (size_t i = 0; i < medido.size(); ++i) {
            erro.push_back(std::fabs(medido[i] - esperado[i]));
            m += medido[i];
            r += esperado[i];
        }
        std::sort(erro.begin(), erro.end());
        const size_t perto = size_t(std::count_if(erro.begin(), erro.end(), [](double x) { return x <= 1.0; }));
        std::printf("%s) quadrado no gerado: medido %+.2f px, referência em CPU %+.2f px (ideal sem bordas +4), "
                    "erro mediano %.2f, máx %.2f, %zu de %zu a até 1 px\n",
                    nome, m / double(medido.size()), r / double(medido.size()), erro[erro.size() / 2], erro.back(),
                    perto, erro.size());
        CHECK(erro[erro.size() / 2] <= 0.5);
        CHECK(perto * 10 >= erro.size() * 9);
    }
};

void EscreveIni(const std::string &caminho, bool ligado, const char *extra = "") {
    std::ofstream f(caminho, std::ios::trunc);
    f << "; teste ponta a ponta\n[framegen]\nativo = " << (ligado ? 1 : 0) << "\n" << extra;
}

// Fim do ghostframe.log desta execução (o arquivo é aberto em modo append).
void MostraLog(const std::string &caminho) {
    std::ifstream f(caminho);
    std::vector<std::string> linhas;
    for (std::string l; std::getline(f, l);) {
        if (l.find("===== nova sessao") != std::string::npos) linhas.clear();
        linhas.push_back(l);
    }
    std::printf("\n---- %s (fim desta execução) ----\n", caminho.c_str());
    for (size_t i = linhas.size() > 200 ? linhas.size() - 200 : 0; i < linhas.size(); ++i)
        std::printf("%s\n", linhas[i].c_str());
}

std::string Barras(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("uso: %s <proxy.dll> <driver_real.dll>\n", argv[0]);
        return 2;
    }
    const std::string proxy = Barras(argv[1]), real = Barras(argv[2]);
    const std::string pastaProxy = proxy.substr(0, proxy.find_last_of('\\') + 1);

    std::string motivo;
    if (!Sonda(motivo)) {
        std::printf("SKIP: %s\n", motivo.c_str());
        return 77;
    }

    // ghostframe.ini numa pasta só do teste: o proxy lê %LOCALAPPDATA%\ghostframeVR.
    char tmp[MAX_PATH + 1] = {};
    GetTempPathA(DWORD(sizeof(tmp)), tmp);
    const std::string local = std::string(tmp) + "ghostframe_e2e_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(local.c_str(), nullptr);
    CreateDirectoryA((local + "\\ghostframeVR").c_str(), nullptr);
    SetEnvironmentVariableA("LOCALAPPDATA", local.c_str());
    // O "headset" deste teste roda a ~33 Hz (vsync de 30 ms): o relógio próprio aceita.
    SetEnvironmentVariableA("GF_TESTE_HZ_LIVRE", "1");
    const std::string ini = local + "\\ghostframeVR\\ghostframe.ini";
    EscreveIni(ini, false);

    for (int p = 0; p < kPosicoes; ++p) {
        std::vector<uint8_t> img;
        Desenha(p, img);
        g_centro[p] = CentroX(img.data(), kW * 4, kOlho, kH);
        g_ref[p] = p ? DeslocRef(p) : 0;
    }

    // ---- SteamVR: carrega o proxy, Init, HMD ----
    HMODULE hp = LoadLibraryA(proxy.c_str());
    CHECK(hp != nullptr);
    if (!hp) return 1;
    auto factory = Funcao<void *(*)(const char *, int *)>(hp, "HmdDriverFactory");
    CHECK(factory != nullptr);
    if (!factory) return 1;
    int rc = -1;
    auto *prov = static_cast<vr::IServerTrackedDeviceProvider *>(factory(vr::IServerTrackedDeviceProvider_Version, &rc));
    CHECK(prov != nullptr && rc == 0);
    if (!prov) return 1;
    CHECK(prov->Init(&g_ctx) == vr::VRInitError_None);

    // O proxy já carregou o Pico falso: pega as funções dele.
    HMODULE hReal = GetModuleHandleA(real.substr(real.find_last_of('\\') + 1).c_str());
    CHECK(hReal != nullptr);
    if (!hReal) return 1;
    const Fake fake{Funcao<void *(*)()>(hReal, "FakePico_DM8"),
                    Funcao<int (*)()>(hReal, "FakePico_Presents"),
                    Funcao<int (*)()>(hReal, "FakePico_PostPresents"),
                    Funcao<int (*)()>(hReal, "FakePico_ConjuntosDoProxy"),
                    Funcao<int (*)()>(hReal, "FakePico_SyncFalhou"),
                    Funcao<int (*)(int, uint64_t *, int *, float *)>(hReal, "FakePico_Registro")};
    CHECK(fake.Ok());
    CHECK(g_hmd != nullptr);
    if (!fake.Ok() || !g_hmd) return 1;
    CHECK(g_hmd->Activate(0) == vr::VRInitError_None);

    // Como o vrserver com o Pico de verdade: sem VirtualDisplay, Display 002 e DirectMode 008.
    CHECK(g_hmd->GetComponent(vr::IVRVirtualDisplay_Version) == nullptr);
    CHECK(g_hmd->GetComponent(vr::legado::IVRDisplayComponent_Version) != nullptr);
    Compositor c;
    c.dm = static_cast<DM8 *>(g_hmd->GetComponent(vr::legado::IVRDriverDirectModeComponent_Version));
    CHECK(c.dm != nullptr && c.dm != fake.dm8()); // embrulhado
    if (!c.dm) return 1;

    // ---- Compositor: device próprio, swap set do driver, sync com keyed mutex ----
    HRESULT erro = S_OK;
    CHECK(CriaDevice(c.dev, c.ctx, erro));
    if (!c.dev) return 1;
    const DM8::SwapTextureSetDesc_t desc{kW, kH, uint32_t(kFormato), 1};
    DM8::SwapTextureSet_t set{};
    c.dm->CreateSwapTextureSet(kPidCompositor, &desc, &set);
    bool abriu = set.unTextureFlags == 0;
    for (int i = 0; i < 3; ++i) {
        c.h[i] = set.rSharedTextureHandles[i];
        abriu = abriu && c.h[i] && SUCCEEDED(Abre(c.dev.Get(), c.h[i], c.tex[i]));
    }
    CHECK(abriu);
    c.sync = CriaTextura(c.dev.Get(), 16, 16, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, nullptr);
    c.hSync = Handle(c.sync.Get());
    CHECK(c.hSync != 0 && SUCCEEDED(c.sync.As(&c.km)));
    if (!abriu || !c.km) return 1;

    int jogo = 1;

    // ---- a) frame gen desligado: tudo chega ao driver como o compositor mandou ----
    std::printf("\n== a) frame gen desligado ==\n");
    {
        const int n0 = fake.presents();
        int intactos = 0;
        for (int v = 0; v < 20; ++v) {
            const double t0 = gf::NowSeconds();
            const int de = fake.presents();
            const vr::SharedTextureHandle_t h = Vsync(c, jogo, 0);
            const std::vector<Reg> r = Registros(fake, de);
            intactos += r.size() == 1 && Intacto(r[0], h, jogo);
            jogo += v % 2; // meia taxa: cada frame do jogo em dois vsyncs
            gf::DormirAte(t0 + kVsync);
        }
        std::printf("a) %d Presents do compositor, %d no driver, %d intactos\n", 20, fake.presents() - n0, intactos);
        CHECK(fake.presents() - n0 == 20);
        CHECK(intactos == 20);
    }

    // ---- b) frame gen ligado, jogo a meia taxa sem trava: o repetido vira gerado ----
    std::printf("\n== b) frame gen ligado, meia taxa, sem trava ==\n");
    EscreveIni(ini, true);
    {
        // O proxy relê o ini a cada 1 s e o motor inicia numa thread (device + shaders); está
        // pronto quando o proxy pede ao driver o swap set de saída.
        const double t0 = gf::NowSeconds();
        while (fake.conjuntosDoProxy() == 0 && gf::NowSeconds() - t0 < 20) {
            for (int v = 0; v < 2; ++v) {
                const double tv = gf::NowSeconds();
                Vsync(c, jogo, 0);
                gf::DormirAte(tv + kVsync);
            }
            ++jogo;
        }
        if (fake.conjuntosDoProxy() > 0) std::printf("b) motor pronto em %.1f s\n", gf::NowSeconds() - t0);
        else std::printf("b) motor não ficou pronto em %.0f s (veja FrameGen no log abaixo)\n", gf::NowSeconds() - t0);
        CHECK(fake.conjuntosDoProxy() > 0);
        if (fake.conjuntosDoProxy() == 0) {
            MostraLog(pastaProxy + "ghostframe.log");
            return 1;
        }

        int reais = 0, reaisIntactos = 0, repetidos = 0, trocados = 0, intactosRep = 0;
        Desloc d;
        for (int k = 0; k < 66; ++k, ++jogo) {
            for (int rep = 0; rep < 2; ++rep) {
                const double tv = gf::NowSeconds();
                const int de = fake.presents();
                const vr::SharedTextureHandle_t h = Vsync(c, jogo, 0);
                const std::vector<Reg> r = Registros(fake, de);
                const bool conta = k >= 6; // antes: médias de intervalo do proxy assentando
                if (conta && !rep) {
                    reais++;
                    reaisIntactos += r.size() == 1 && Intacto(r[0], h, jogo);
                } else if (conta) {
                    repetidos++;
                    if (r.size() == 1 && r[0].doProxy) {
                        trocados++;
                        d.Anota(r[0], jogo);
                    } else {
                        intactosRep += r.size() == 1 && Intacto(r[0], h, jogo);
                    }
                }
                gf::DormirAte(tv + kVsync);
            }
        }
        std::printf("b) reais intactos %d de %d; repetidos trocados pelo gerado %d de %d (%.0f%%), os outros %d "
                    "passaram intactos\n",
                    reaisIntactos, reais, trocados, repetidos, 100.0 * trocados / repetidos, intactosRep);
        CHECK(reaisIntactos == reais);
        CHECK(trocados * 10 >= repetidos * 8);
        CHECK(trocados + intactosRep == repetidos);
        d.Confere("b");
    }

    // ---- c) trava 1: o compositor manda cada frame uma vez e o PostPresent do proxy
    // entrega um gerado no vsync seguinte ----
    std::printf("\n== c) trava 1 (PostPresent com nFramesToThrottle = 1) ==\n");
    {
        const int pp0 = fake.postPresents();
        int quadros = 0, comDois = 0, reaisIntactos = 0, gerados = 0;
        Desloc d;
        for (int k = 0; k < 70; ++k, ++jogo) {
            const double tv = gf::NowSeconds();
            const int de = fake.presents();
            const vr::SharedTextureHandle_t h = Vsync(c, jogo, 1);
            const std::vector<Reg> r = Registros(fake, de);
            if (k >= 10) { // o proxy ajusta o período do vsync pelos Presents
                quadros++;
                comDois += r.size() == 2;
                reaisIntactos += !r.empty() && Intacto(r[0], h, jogo);
                if (r.size() == 2 && r[1].doProxy) {
                    gerados++;
                    d.Anota(r[1], jogo);
                }
            }
            gf::DormirAte(tv + 2 * kVsync);
        }
        std::printf("c) %d frames do compositor: %d com 2 Presents no driver, %d reais intactos, %d extras gerados "
                    "(%.0f%%); PostPresents no driver: %d em 70 frames\n",
                    quadros, comDois, reaisIntactos, gerados, 100.0 * gerados / quadros, fake.postPresents() - pp0);
        // Sem o gerado pronto, o real não é reenviado com o sync do compositor ocupado (o driver
        // repete sozinho): um quadro ou outro tem 1 Present só.
        CHECK(comDois * 10 >= quadros * 9);
        CHECK(reaisIntactos == quadros);
        CHECK(gerados * 10 >= quadros * 8);
        d.Confere("c");
    }

    // ---- d) como o Pico Connect: sem trava e sem repetir quadro. O compositor só chama Present
    // quando o jogo tem quadro novo (a metade dos Hz do headset); o proxy gera no vsync vazio. ----
    std::printf("\n== d) jogo a meia taxa sem repetir quadro e sem trava (relógio próprio) ==\n");
    {
        int quadros = 0, comDois = 0, reaisIntactos = 0, gerados = 0;
        Desloc d;
        for (int k = 0; k < 70; ++k, ++jogo) {
            const double tv = gf::NowSeconds();
            const int de = fake.presents();
            const vr::SharedTextureHandle_t h = Vsync(c, jogo, 0);
            const std::vector<Reg> r = Registros(fake, de);
            if (k >= 10) { // o proxy mede o ritmo do jogo pelos Presents
                quadros++;
                comDois += r.size() == 2;
                reaisIntactos += !r.empty() && Intacto(r[0], h, jogo);
                if (r.size() == 2 && r[1].doProxy) {
                    gerados++;
                    d.Anota(r[1], jogo);
                }
            }
            gf::DormirAte(tv + 2 * kVsync); // o jogo entrega um quadro a cada 2 vsyncs
        }
        std::printf("d) %d quadros do jogo: %d com 2 Presents no driver, %d reais intactos, %d gerados no vsync vazio (%.0f%%)\n",
                    quadros, comDois, reaisIntactos, gerados, 100.0 * gerados / quadros);
        CHECK(reaisIntactos == quadros);
        CHECK(gerados * 10 >= quadros * 8);
        d.Confere("d");
    }

    // ---- e) jogo a 2/3 dos Hz (como 60 fps em 90 Hz): um gerado a cada 2 reais, nunca mais
    // Presents que os vsyncs. ----
    std::printf("\n== e) jogo a 2/3 dos Hz sem repetir (crédito: 1 gerado a cada 2 reais) ==\n");
    auto lerImagem = Funcao<void (*)(int)>(hReal, "FakePico_LerImagem");
    CHECK(lerImagem != nullptr);
    if (lerImagem) lerImagem(0); // e, f e g só contam quadros
    {
        int quadros = 0, gerados = 0, presents = 0;
        for (int k = 0; k < 80; ++k, ++jogo) {
            const double tv = gf::NowSeconds();
            const int de = fake.presents();
            Vsync(c, jogo, 0);
            const std::vector<Reg> r = Registros(fake, de);
            if (k >= 10) {
                quadros++;
                presents += int(r.size());
                gerados += r.size() == 2 && r[1].doProxy;
            }
            gf::DormirAte(tv + 1.5 * kVsync);
        }
        // 70 quadros em 70*1.5 vsyncs: o ideal é 35 gerados (headset cheio, sem passar).
        std::printf("e) %d quadros do jogo: %d gerados (%.0f%% dos quadros), %d Presents no driver em %.0f vsyncs\n",
                    quadros, gerados, 100.0 * gerados / quadros, presents, quadros * 1.5);
        CHECK(gerados * 10 >= quadros * 3 && gerados * 10 <= quadros * 7);
        CHECK(presents <= int(quadros * 1.5 * 1.05) + 2); // headset cheio, sem passar (5% de folga de tempo)
    }

    // ---- f) driver cujo PostPresent demora 20 ms (como um que espera o próprio vsync), jogo a
    // 1/3 dos Hz: cabe um gerado, e o proxy nunca segura o compositor até o próximo quadro. ----
    std::printf("\n== f) PostPresent do driver demorando 20 ms ==\n");
    {
        auto espera = Funcao<void (*)(int)>(hReal, "FakePico_EsperaPostPresent");
        CHECK(espera != nullptr);
        if (espera) espera(20);
        int quadros = 0, gerados = 0;
        std::vector<double> preso;
        for (int k = 0; k < 60; ++k, ++jogo) {
            const double tv = gf::NowSeconds();
            const int de = fake.presents();
            Vsync(c, jogo, 0);
            // Do Present do compositor até o proxy devolver o PostPresent (o desenho do
            // compositor falso antes do Present não é do proxy).
            const double segurou = gf::NowSeconds() - g_tPresent;
            const std::vector<Reg> r = Registros(fake, de);
            if (k >= 10) {
                quadros++;
                gerados += r.size() == 2 && r[1].doProxy;
                preso.push_back(segurou);
            }
            gf::DormirAte(tv + 3 * kVsync);
        }
        if (espera) espera(0);
        std::sort(preso.begin(), preso.end());
        const double p50 = preso[preso.size() / 2], p90 = preso[preso.size() * 9 / 10], pmax = preso.back();
        const int passou = int(std::count_if(preso.begin(), preso.end(), [](double x) { return x >= 3 * kVsync; }));
        std::printf("f) %d quadros: %d gerados (%.0f%%); compositor preso: mediana %.1f ms, 90%% %.1f ms, máx %.1f ms, "
                    "%d de %d além do próximo quadro (%.0f ms)\n",
                    quadros, gerados, 100.0 * gerados / quadros, p50 * 1000, p90 * 1000, pmax * 1000, passou, quadros,
                    3 * kVsync * 1000);
        // A guarda decide antes de gerar, com o que mediu; um soluço isolado da "GPU" em software
        // da CI pode passar. O que não pode é virar regra.
        CHECK(p90 < 3 * kVsync - 0.002);
        CHECK(passou * 10 <= quadros);
        CHECK(gerados * 10 >= quadros * 3);
    }

    // ---- g) Manual 2x sem trava do SteamVR: o jogo tenta rodar no máximo e o proxy o trava na
    // metade dos Hz, com um gerado em cada vsync do meio (o pedido "sempre 2x"). ----
    std::printf("\n== g) manual 2x: o proxy trava o jogo na metade e gera o resto ==\n");
    EscreveIni(ini, true, "modo = manual\nmultiplicador = 2\n");
    Sleep(1500); // o proxy relê o ini a cada 1 s
    {
        int quadros = 0, comDois = 0, gerados = 0;
        double total = 0;
        for (int k = 0; k < 60; ++k, ++jogo) {
            const int de = fake.presents();
            const double tv = gf::NowSeconds();
            Vsync(c, jogo, 0); // nenhuma espera do "jogo": quem segura o ritmo é o proxy
            const double dur = gf::NowSeconds() - tv;
            const std::vector<Reg> r = Registros(fake, de);
            if (k >= 10) {
                quadros++;
                comDois += r.size() == 2;
                gerados += r.size() == 2 && r[1].doProxy;
                total += dur;
            }
        }
        const double media = total / quadros;
        std::printf("g) %d quadros do jogo: %d com 2 Presents, %d gerados (%.0f%%); cada quadro real levou em média "
                    "%.1f ms (2 vsyncs = %.0f ms)\n",
                    quadros, comDois, gerados, 100.0 * gerados / quadros, media * 1000, 2 * kVsync * 1000);
        CHECK(gerados * 10 >= quadros * 8);
        CHECK(media >= 1.7 * kVsync); // o jogo ficou na metade dos Hz
    }
    EscreveIni(ini, true);
    Sleep(1500);
    if (lerImagem) lerImagem(1);

    // O proxy nunca segurou o sync na hora errada: driver e compositor sempre o acharam livre.
    CHECK(fake.syncFalhou() == 0);
    CHECK(c.syncOcupado == 0);

    c.dm->DestroySwapTextureSet(c.h[0]);
    prov->Cleanup();
    MostraLog(pastaProxy + "ghostframe.log");
    std::printf(g_fail ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", g_fail);
    return g_fail ? 1 : 0;
}
