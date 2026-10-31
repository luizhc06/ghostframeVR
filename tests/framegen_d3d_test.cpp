// Teste do motor na GPU (Windows). Roda no WARP (o CI não tem GPU) e no adaptador de
// hardware quando houver. Faz NovoFrame×2 + Gerar pela API pública e compara o frame
// gerado com a referência em CPU. É o primeiro lugar onde os shaders compilam e rodam.
#include "cena_sintetica.h"
#include "framegen.h"
#include "framegen_ref.h"
#include "log.h"
#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace gf;

namespace {

int g_fail = 0;
void Check(bool ok, const char *expr, int linha) {
    std::printf("%s: %s (linha %d)\n", ok ? "ok" : "FALHOU", expr, linha);
    if (!ok) g_fail++;
}
#define CHECK(c) Check((c), #c, __LINE__)

constexpr int kW = 512, kH = 256;   // textura do jogo: os dois olhos lado a lado
constexpr int kRw = 256, kRh = 192; // região de cada olho
constexpr int kY0 = 32;             // vMin = 0.125
const vr::VRTextureBounds_t kBounds[2] = {{0.0f, 0.125f, 0.5f, 0.875f}, {0.5f, 0.125f, 1.0f, 0.875f}};
const ProjecaoOlho kProj[2] = {{-1.2f, 0.9f, -1.1f, 0.95f}, {-0.9f, 1.2f, -1.1f, 0.95f}};

// Textura inteira do jogo (RGBA8): cada olho na sua região, o resto preto.
std::vector<uint8_t> Frame(const cena::Cena &c, double t, const vr::HmdMatrix34_t &pose) {
    std::vector<uint8_t> img(size_t(kW) * kH * 4, 0);
    for (int e = 0; e < 2; ++e)
        cena::Renderiza(c, t, pose, kProj[e], kRw, kRh, &img[(size_t(kY0) * kW + size_t(e) * kRw) * 4], kW * 4);
    return img;
}

void TrocaRB(std::vector<uint8_t> &img) {
    for (size_t i = 0; i + 3 < img.size(); i += 4) std::swap(img[i], img[i + 2]);
}

bool EhBgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

// Como o driver cria as texturas do swap set: sem UAV.
ComPtr<ID3D11Texture2D> CriaTextura(ID3D11Device *dev, DXGI_FORMAT fmt, UINT misc, const std::vector<uint8_t> &dados) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kW;
    d.Height = kH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = misc;
    D3D11_SUBRESOURCE_DATA sd{dados.data(), UINT(kW * 4), 0};
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, &sd, t.GetAddressOf()))) return nullptr;
    return t;
}

vr::SharedTextureHandle_t Handle(ID3D11Texture2D *t) {
    ComPtr<IDXGIResource> r;
    HANDLE h = nullptr;
    if (!t || FAILED(t->QueryInterface(IID_PPV_ARGS(r.GetAddressOf()))) || FAILED(r->GetSharedHandle(&h))) return 0;
    return vr::SharedTextureHandle_t(reinterpret_cast<uintptr_t>(h));
}

// Espera a GPU deste contexto terminar (o "driver" precisa ter subido as texturas).
void Espera(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> q;
    if (FAILED(dev->CreateQuery(&qd, q.GetAddressOf()))) return;
    ctx->End(q.Get());
    BOOL feito = FALSE;
    double t0 = NowSeconds();
    while ((ctx->GetData(q.Get(), &feito, sizeof(feito), 0) != S_OK || !feito) && NowSeconds() - t0 < 10) Sleep(1);
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
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &map))) return out;
    out.resize(size_t(kW) * kH * 4);
    for (int y = 0; y < kH; ++y)
        std::memcpy(&out[size_t(y) * kW * 4], static_cast<const uint8_t *>(map.pData) + size_t(y) * map.RowPitch,
                    size_t(kW) * 4);
    ctx->Unmap(st.Get(), 0);
    return out;
}

void Cenario(bool warp, DXGI_FORMAT fmtJogo, DXGI_FORMAT fmtSaida, bool obrigatorio) {
    std::printf("\n== %s: jogo fmt %d, saída fmt %d ==\n", warp ? "WARP" : "hardware", (int)fmtJogo, (int)fmtSaida);
    FrameGen fg;
    if (!fg.Iniciar(warp)) {
        if (warp)
            Check(false, "fg.Iniciar(WARP)", __LINE__);
        else
            std::printf("   sem adaptador de hardware utilizável: pulando\n");
        return;
    }
    for (int e = 0; e < 2; ++e) fg.DefinirProjecao(e, kProj[e]);

    // Cena: quadrado andando para a direita, cabeça girando um pouco entre os frames.
    cena::Cena c;
    c.az0 = -0.5;
    c.el0 = -0.45;
    c.lado = 1.0;
    c.vel = 0.1;
    c.celulas = 12;
    const vr::HmdMatrix34_t p0 = cena::Pose(0, 0, 0);
    const vr::HmdMatrix34_t p1 = cena::Pose(cena::Graus(2), cena::Graus(1), cena::Graus(0.5));
    const auto f0 = Frame(c, 0, p0), f1 = Frame(c, 1, p1);
    std::vector<uint8_t> up0 = f0, up1 = f1;
    if (EhBgra(fmtJogo)) {
        TrocaRB(up0);
        TrocaRB(up1);
    }
    const std::vector<uint8_t> fundo(size_t(kW) * kH * 4, 0x55);
    const bool rgbaNaSaida = !EhBgra(fmtSaida);
    // 0, 1: frames do jogo; 2: saída; 3: saída de outra família (deve ser recusada).
    const std::vector<uint8_t> *dados[4] = {&up0, &up1, &fundo, &fundo};
    const DXGI_FORMAT fmts[4] = {fmtJogo, fmtJogo, fmtSaida,
                                 rgbaNaSaida ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM};
    ComPtr<ID3D11Texture2D> tex[4];
    vr::SharedTextureHandle_t h[4] = {};

    // Caminho real: outro device (o "driver") cria texturas compartilhadas e o motor abre os handles.
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    bool compartilha = SUCCEEDED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                                   nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(),
                                                   nullptr, ctx.GetAddressOf()));
    for (int i = 0; i < 4 && compartilha; ++i) {
        tex[i] = CriaTextura(dev.Get(), fmts[i], D3D11_RESOURCE_MISC_SHARED, *dados[i]);
        h[i] = Handle(tex[i].Get());
        compartilha = h[i] != 0;
    }
    if (!compartilha) {
        // O WARP não compartilha texturas: cria no device do motor e injeta no cache.
        std::printf("   sem texturas compartilhadas neste device: usando o gancho de teste\n");
        dev = FrameGenTeste::Device(fg);
        ctx.Reset();
        dev->GetImmediateContext(ctx.GetAddressOf());
        for (int i = 0; i < 4; ++i) {
            tex[i] = CriaTextura(dev.Get(), fmts[i], 0, *dados[i]);
            h[i] = 0x1000 + vr::SharedTextureHandle_t(i);
            CHECK(tex[i] && FrameGenTeste::Injetar(fg, h[i], tex[i].Get()));
        }
    } else {
        std::printf("   texturas compartilhadas por handle (caminho real)\n");
    }
    if (!tex[0] || !tex[1] || !tex[2]) {
        Check(false, "criar as texturas do teste", __LINE__);
        return;
    }
    Espera(dev.Get(), ctx.Get());

    const vr::SharedTextureHandle_t in0[2] = {h[0], h[0]}, in1[2] = {h[1], h[1]}, out[2] = {h[2], h[2]},
                                    outra[2] = {h[3], h[3]};
    const bool primeiro = fg.NovoFrame(in0, kBounds, p0, false);
    if (!primeiro && !obrigatorio) {
        std::printf("   AVISO: formato não suportado neste device (veja o log abaixo); sem falha\n");
        return;
    }
    CHECK(primeiro);
    CHECK(!fg.Pronto());
    CHECK(fg.NovoFrame(in1, kBounds, p1, false));
    CHECK(fg.Pronto());

    // Entradas ruins: nada vai para a GPU.
    CHECK(!fg.Gerar(0.0f, out, kBounds, false));
    CHECK(!fg.Gerar(1.5f, out, kBounds, false));
    const vr::VRTextureBounds_t outroTamanho[2] = {{0.0f, 0.125f, 0.25f, 0.875f}, kBounds[1]};
    CHECK(!fg.Gerar(0.5f, out, outroTamanho, false));
    if (tex[3]) CHECK(!fg.Gerar(0.5f, outra, kBounds, false));
    const vr::SharedTextureHandle_t nulo[2] = {0, 0};
    CHECK(!fg.NovoFrame(nulo, kBounds, p1, false));
    CHECK(!fg.Concluido());

    CHECK(fg.Gerar(0.5f, out, kBounds, false));
    const double t0 = NowSeconds();
    bool pronto = fg.Concluido();
    while (!pronto && NowSeconds() - t0 < 30) {
        Sleep(1);
        pronto = fg.Concluido();
    }
    CHECK(pronto);
    for (int i = 0; i < 50 && fg.MsGpu() < 0; ++i) {
        fg.Concluido(); // também lê as medidas de tempo
        Sleep(2);
    }
    std::printf("   tempo de GPU (NovoFrame + Gerar): %.3f ms\n", fg.MsGpu());

    std::vector<uint8_t> lido = Le(dev.Get(), ctx.Get(), tex[2].Get());
    if (lido.empty()) {
        Check(false, "ler a saída", __LINE__);
        return;
    }
    if (EhBgra(fmtSaida)) TrocaRB(lido);

    // Referência em CPU, olho por olho, a partir das mesmas regiões.
    long ruins = 0, total = 0, movidos = 0, movidosGpu = 0;
    double soma = 0;
    int maxDif = 0;
    for (int e = 0; e < 2; ++e) {
        ref::Olho o;
        o.proj = kProj[e];
        const size_t ini = (size_t(kY0) * kW + size_t(e) * kRw) * 4;
        ref::NovoFrame(o, &f0[ini], kW * 4, kRw, kRh, p0);
        ref::NovoFrame(o, &f1[ini], kW * 4, kRw, kRh, p1);
        std::vector<uint8_t> esperado(size_t(kRw) * kRh * 4);
        CHECK(ref::Gerar(o, 0.5f, esperado.data(), kRw * 4));
        for (int y = 0; y < kRh; ++y)
            for (int x = 0; x < kRw * 4; ++x) {
                const size_t i = ini + size_t(y) * kW * 4 + size_t(x);
                const int esp = esperado[size_t(y) * kRw * 4 + size_t(x)];
                const int d = std::abs(int(lido[i]) - esp);
                maxDif = std::max(maxDif, d);
                soma += d;
                ruins += d > 2;
                movidos += std::abs(esp - int(f1[i])) > 8;
                movidosGpu += std::abs(int(lido[i]) - int(f1[i])) > 8;
                total++;
            }
    }
    std::printf("   GPU vs CPU: %ld de %ld bytes com diferença > 2 (máx %d, média %.3f)\n", ruins, total, maxDif,
                soma / double(total));
    std::printf("   mudaram vs último frame: %ld bytes na CPU, %ld na GPU\n", movidos, movidosGpu);
    CHECK(movidos * 20 > total);                               // a cena tem movimento de verdade (> 5%)
    CHECK(movidosGpu * 10 >= movidos * 8);                     // e a GPU moveu também
    CHECK(ruins * 100 < total * 3 / 2 && soma < total * 1.0); // até 1,5%: um empate de vetor pode virar diferente

    // Fora das regiões dos olhos a saída fica intacta.
    bool intacta = true;
    for (int y = 0; y < kH; ++y)
        if (y < kY0 || y >= kY0 + kRh)
            for (int x = 0; x < kW * 4; ++x) intacta = intacta && lido[size_t(y) * kW * 4 + size_t(x)] == 0x55;
    CHECK(intacta);

    // Keyed mutex do syncTexture: só com compartilhamento de verdade.
    if (compartilha) {
        ComPtr<ID3D11Texture2D> sync =
            CriaTextura(dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, fundo);
        ComPtr<IDXGIKeyedMutex> km;
        const vr::SharedTextureHandle_t hs = Handle(sync.Get());
        if (sync && hs && SUCCEEDED(sync.As(&km))) {
            CHECK(km->AcquireSync(0, 1000) == S_OK);
            CHECK(!fg.Travar(hs)); // o "driver" está com a chave: estoura o tempo e devolve false
            CHECK(km->ReleaseSync(0) == S_OK);
            CHECK(fg.Travar(hs));
            fg.Destravar();
            CHECK(km->AcquireSync(0, 1000) == S_OK); // a chave voltou
            km->ReleaseSync(0);
        } else {
            std::printf("   sem keyed mutex neste device: pulando Travar\n");
        }
    }
}

void MostraLog() {
    std::string self = OwnModulePath();
    size_t barra = self.find_last_of("/\\");
    std::ifstream f((barra == std::string::npos ? std::string(".") : self.substr(0, barra)) + "/ghostframe.log");
    std::printf("\n---- ghostframe.log ----\n");
    if (f) std::cout << f.rdbuf();
    std::cout << std::flush;
}

} // namespace

int main() {
    LogInit();
    Cenario(true, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true);
    Cenario(true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    Cenario(true, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, false);
    Cenario(false, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true);
    MostraLog();
    std::printf(g_fail ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", g_fail);
    return g_fail ? 1 : 0;
}
