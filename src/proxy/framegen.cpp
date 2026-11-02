// Motor do frame gen em D3D11 (compute shaders). O algoritmo é o de framegen_ref.cpp,
// passo a passo; mudou um, muda o outro (o framegen_d3d_test compara os dois).
#include "framegen.h"

#ifdef _WIN32

#include "framegen_ref.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

namespace gf {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kCache = 32;          // texturas abertas por handle: swap sets do compositor, saída e sync
constexpr int kMedidas = 8;         // medidas de tempo em voo (lidas sem bloquear)

// Layout igual ao cbuffer Params do HLSL (registradores de 16 bytes).
struct Params {
    float h[3][4];
    uint32_t cheio[2], luma[2], blocos[2];
    float alfa, vies, custoBom, custoRuim, pad[2];
};
static_assert(sizeof(Params) == 96, "layout do cbuffer Params");

// ESCALA, BLOCO e BUSCA chegam como macros, tirados de framegen_ref.h.
const char kHlsl[] = R"hlsl(
#define INVALIDO (-1.0)
#define GRANDE 1e9
#define JANELA (BLOCO + 2 * BUSCA)
#define LADO_BUSCA (2 * BUSCA + 1)
#define NCAND (LADO_BUSCA * LADO_BUSCA)

cbuffer Params : register(b0) {
    float4 gH0, gH1, gH2; // homografia luma atual -> luma anterior
    uint2 gCheio;         // região do olho em pixels cheios
    uint2 gLuma;
    uint2 gBlocos;
    float gAlfa;
    float gVies;
    float gCustoBom;
    float gCustoRuim;
    float2 gPad;
};

Texture2D<float4> tCor : register(t0);
Texture2D<float> tLumaA : register(t1);
Texture2D<float> tLumaB : register(t2);
Texture2D<float4> tVetores : register(t3);
RWTexture2D<float> uLuma : register(u0);
RWTexture2D<float4> uVetores : register(u1);
RWTexture2D<float4> uCor : register(u2);

// Bilinear manual (centro do texel em i + 0.5, borda repetida): igual à referência em CPU.
void Pesos(float2 s, uint2 lado, out int2 i0, out int2 i1, out float2 f) {
    float2 c = clamp(s - 0.5, 0.0, float2(lado) - 1.0);
    i0 = int2(floor(c));
    f = c - float2(i0);
    i1 = min(i0 + 1, int2(lado) - 1);
}

[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gLuma)) return;
    float soma = 0;
    [unroll] for (int j = 0; j < ESCALA; ++j)
        [unroll] for (int i = 0; i < ESCALA; ++i) {
            float3 c = tCor.Load(int3(int2(id.xy) * ESCALA + int2(i, j), 0)).rgb;
            soma += c.r * 0.2126 + c.g * 0.7152 + c.b * 0.0722;
        }
    uLuma[id.xy] = soma / (ESCALA * ESCALA);
}

[numthreads(8, 8, 1)]
void CSRotaciona(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gLuma)) return;
    float2 p = float2(id.xy) + 0.5;
    float qx = gH0.x * p.x + gH0.y * p.y + gH0.z;
    float qy = gH1.x * p.x + gH1.y * p.y + gH1.z;
    float qz = gH2.x * p.x + gH2.y * p.y + gH2.z;
    float r = INVALIDO;
    if (qz > 1e-6) {
        float2 s = float2(qx, qy) / qz;
        if (all(s >= 0.0) && all(s <= float2(gLuma))) {
            int2 i0, i1;
            float2 f;
            Pesos(s, gLuma, i0, i1, f);
            float a = lerp(tLumaA.Load(int3(i0.x, i0.y, 0)), tLumaA.Load(int3(i1.x, i0.y, 0)), f.x);
            float b = lerp(tLumaA.Load(int3(i0.x, i1.y, 0)), tLumaA.Load(int3(i1.x, i1.y, 0)), f.x);
            r = lerp(a, b, f.y);
        }
    }
    uLuma[id.xy] = r;
}

groupshared float sJanela[JANELA * JANELA];
groupshared float sBloco[NBLOCO];
groupshared float sCusto[NBLOCO];
groupshared float sBruto[NBLOCO];
groupshared uint sIdx[NBLOCO];

float LeLuma(Texture2D<float> t, int2 q) {
    if (any(q < 0) || any(q >= int2(gLuma))) return INVALIDO;
    return t.Load(int3(q, 0));
}

// Um grupo por bloco: janela de busca em memória compartilhada, cada thread testa um
// candidato a cada NBLOCO e o grupo reduz pelo menor (custo, índice).
[numthreads(NBLOCO, 1, 1)]
void CSBlocos(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    int2 base = int2(gid.xy) * BLOCO;
    for (uint i = tid; i < JANELA * JANELA; i += NBLOCO)
        sJanela[i] = LeLuma(tLumaB, base - BUSCA + int2(i % JANELA, i / JANELA));
    sBloco[tid] = LeLuma(tLumaA, base + int2(tid % BLOCO, tid / BLOCO));
    GroupMemoryBarrierWithGroupSync();

    uint nBloco = 0, nVisto = 0;
    for (uint p = 0; p < NBLOCO; ++p) {
        bool valido = sBloco[p] >= 0;
        nBloco += valido ? 1 : 0;
        nVisto += (valido && sJanela[(p / BLOCO + BUSCA) * JANELA + p % BLOCO + BUSCA] >= 0) ? 1 : 0;
    }

    float melhor = GRANDE, bruto = GRANDE;
    uint idx = NCAND;
    for (uint k = tid; k < NCAND; k += NBLOCO) {
        int dx = int(k % LADO_BUSCA) - BUSCA, dy = int(k / LADO_BUSCA) - BUSCA;
        float sad = 0;
        uint n = 0;
        [unroll] for (uint q = 0; q < NBLOCO; ++q) {
            float c = sBloco[q];
            float a = sJanela[(int(q / BLOCO) + BUSCA - dy) * JANELA + int(q % BLOCO) + BUSCA - dx];
            if (c >= 0 && a >= 0) {
                sad += abs(c - a);
                n += 1;
            }
        }
        if (n > 0 && 2 * n >= nBloco) {
            float custo = sad / float(n);
            float comVies = custo + gVies * float(abs(dx) + abs(dy));
            if (comVies < melhor) {
                melhor = comVies;
                bruto = custo;
                idx = k;
            }
        }
    }
    sCusto[tid] = melhor;
    sBruto[tid] = bruto;
    sIdx[tid] = idx;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = NBLOCO / 2; s > 0; s /= 2) {
        if (tid < s) {
            float c2 = sCusto[tid + s];
            uint i2 = sIdx[tid + s];
            if (c2 < sCusto[tid] || (c2 == sCusto[tid] && i2 < sIdx[tid])) {
                sCusto[tid] = c2;
                sBruto[tid] = sBruto[tid + s];
                sIdx[tid] = i2;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0 && all(gid.xy < gBlocos)) {
        float4 r = float4(0, 0, GRANDE, 0);
        if (nBloco > 0 && 2 * nVisto >= nBloco && sIdx[0] < NCAND)
            r = float4(float(int(sIdx[0] % LADO_BUSCA) - BUSCA), float(int(sIdx[0] / LADO_BUSCA) - BUSCA), sBruto[0], 0);
        uVetores[gid.xy] = r;
    }
}

[numthreads(8, 8, 1)]
void CSSuaviza(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gBlocos)) return;
    float2 v[9];
    [unroll] for (int i = 0; i < 9; ++i) {
        int2 q = clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), 0, int2(gBlocos) - 1);
        v[i] = tVetores.Load(int3(q, 0)).xy;
    }
    float melhor = GRANDE;
    float2 med = 0;
    [unroll] for (int a = 0; a < 9; ++a) {
        float s = 0;
        [unroll] for (int b = 0; b < 9; ++b) s += abs(v[a].x - v[b].x) + abs(v[a].y - v[b].y);
        if (s < melhor) {
            melhor = s;
            med = v[a];
        }
    }
    float custo = tVetores.Load(int3(id.xy, 0)).z;
    float conf = saturate((gCustoRuim - custo) / (gCustoRuim - gCustoBom));
    uVetores[id.xy] = float4(med * conf, 0, 0);
}

[numthreads(8, 8, 1)]
void CSGera(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gCheio)) return;
    float2 x = float2(id.xy) + 0.5;
    int2 i0, i1;
    float2 f;
    Pesos(x / (ESCALA * BLOCO), gBlocos, i0, i1, f);
    float2 a = lerp(tVetores.Load(int3(i0.x, i0.y, 0)).xy, tVetores.Load(int3(i1.x, i0.y, 0)).xy, f.x);
    float2 b = lerp(tVetores.Load(int3(i0.x, i1.y, 0)).xy, tVetores.Load(int3(i1.x, i1.y, 0)).xy, f.x);
    float2 v = lerp(a, b, f.y) * ESCALA;
    Pesos(x - gAlfa * v, gCheio, i0, i1, f);
    float4 c = lerp(tCor.Load(int3(i0.x, i0.y, 0)), tCor.Load(int3(i1.x, i0.y, 0)), f.x);
    float4 d = lerp(tCor.Load(int3(i0.x, i1.y, 0)), tCor.Load(int3(i1.x, i1.y, 0)), f.x);
    uCor[id.xy] = lerp(c, d, f.y);
}
)hlsl";

// Primeira falha de cada ponto vai para o log; repetir a cada frame só encheria o arquivo.
bool PrimeiraVez(bool &feito) {
    bool r = !feito;
    feito = true;
    return r;
}
#define GF_LOG_1X(...)                                                                                               \
    {                                                                                                                \
        static bool feito_ = false;                                                                                  \
        if (PrimeiraVez(feito_)) GF_LOG(__VA_ARGS__);                                                                \
    }

// Família TYPELESS (para copiar bits crus) e a vista UNORM usada nos shaders.
bool Familia(DXGI_FORMAT f, DXGI_FORMAT &familia, DXGI_FORMAT &vista) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        familia = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        vista = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        familia = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        vista = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;
    default:
        return false;
    }
}

// Bounds do OpenVR -> retângulo em pixels. Bounds invertidos (vMin > vMax, coisa de
// OpenGL) não aparecem no caminho D3D11 e ficam de fora.
bool Regiao(const D3D11_TEXTURE2D_DESC &d, const vr::VRTextureBounds_t &b, D3D11_BOX &c) {
    if (!(b.uMin >= 0 && b.vMin >= 0 && b.uMax <= 1 && b.vMax <= 1 && b.uMin < b.uMax && b.vMin < b.vMax))
        return false;
    c.left = UINT(std::lround(b.uMin * float(d.Width)));
    c.right = UINT(std::lround(b.uMax * float(d.Width)));
    c.top = UINT(std::lround(b.vMin * float(d.Height)));
    c.bottom = UINT(std::lround(b.vMax * float(d.Height)));
    c.front = 0;
    c.back = 1;
    return c.right > c.left && c.bottom > c.top;
}

UINT Grupos(UINT n) { return (n + 7) / 8; }

} // namespace

struct FrameGen::Impl {
    struct TexAberta {
        vr::SharedTextureHandle_t handle = 0;
        bool nt = false;
        ComPtr<ID3D11Texture2D> tex; // vazio = abrir falhou; não tenta de novo até Reiniciar
        D3D11_TEXTURE2D_DESC desc{};
        uint64_t uso = 0;
    };
    struct Medida {
        ComPtr<ID3D11Query> disjunto, t0, t1;
        int tipo = 0; // 0 = NovoFrame, 1 = Gerar
        bool pendente = false;
    };
    struct Olho {
        ProjecaoOlho proj;
        UINT w = 0, h = 0;
        DXGI_FORMAT familia = DXGI_FORMAT_UNKNOWN;
        int frames = 0, atual = 0; // atual = qual luma[] é do último frame
        vr::HmdMatrix34_t pose{};
        ComPtr<ID3D11Texture2D> ultimo, gerado, luma[2], lumaAntW, blocos, campo;
        ComPtr<ID3D11ShaderResourceView> ultimoSrv, lumaSrv[2], lumaAntWSrv, blocosSrv, campoSrv;
        ComPtr<ID3D11UnorderedAccessView> geradoUav, lumaUav[2], lumaAntWUav, blocosUav, campoUav;
    };

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device1> dev1; // só para handles NT
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> syncProprio; // keyed mutex nosso para os Presents dos gerados
    vr::SharedTextureHandle_t hSyncProprio = 0;
    ComPtr<ID3D11ComputeShader> csLuma, csRotaciona, csBlocos, csSuaviza, csGera;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Query> evento;
    ComPtr<IDXGIKeyedMutex> travado;
    bool gerou = false;
    Params p{};
    Olho olhos[2];
    TexAberta cache[kCache];
    uint64_t relogio = 0;
    int falhasAbrir = 0, aberturas = 0;
    Medida medidas[kMedidas];
    int proxMedida = 0;
    Medida *medindo = nullptr;
    float ms[2] = {-1, -1}; // último tempo de GPU de NovoFrame e de Gerar

    // Entrada do cache para o handle; se não existe, recicla a usada há mais tempo.
    TexAberta &Entrada(vr::SharedTextureHandle_t h, bool nt, bool &achou) {
        ++relogio;
        TexAberta *vitima = &cache[0];
        for (TexAberta &c : cache) {
            if (c.handle == h && c.nt == nt) {
                c.uso = relogio;
                achou = true;
                return c;
            }
            if (c.uso < vitima->uso) vitima = &c;
        }
        achou = false;
        vitima->handle = h;
        vitima->nt = nt;
        vitima->uso = relogio;
        vitima->tex.Reset();
        vitima->desc = {};
        return *vitima;
    }

    TexAberta *Abrir(vr::SharedTextureHandle_t h, bool nt) {
        if (!h) return nullptr;
        bool achou = false;
        TexAberta &e = Entrada(h, nt, achou);
        if (achou) return e.tex ? &e : nullptr;
        // Primeira vez deste handle: abrir aloca, mas só no aquecimento.
        HANDLE hh = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h));
        HRESULT hr = E_NOINTERFACE;
        if (!nt)
            hr = dev->OpenSharedResource(hh, IID_PPV_ARGS(e.tex.ReleaseAndGetAddressOf()));
        else if (dev1)
            hr = dev1->OpenSharedResource1(hh, IID_PPV_ARGS(e.tex.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !e.tex) {
            e.tex.Reset();
            if (falhasAbrir++ < 8)
                GF_LOG("FrameGen: não abriu a textura 0x%llx (nt=%d): 0x%08lx", (unsigned long long)h, (int)nt,
                       (unsigned long)hr);
            return nullptr;
        }
        e.tex->GetDesc(&e.desc);
        // Para o primeiro teste real: se o driver usar keyed mutex nas texturas (misc 0x100),
        // cópias sem AcquireSync são descartadas, e é aqui que dá para ver.
        if (aberturas++ < 16)
            GF_LOG("FrameGen: abriu 0x%llx %ux%u fmt=%d amostras=%u bind=0x%x misc=0x%x", (unsigned long long)h,
                   e.desc.Width, e.desc.Height, (int)e.desc.Format, e.desc.SampleDesc.Count, e.desc.BindFlags,
                   e.desc.MiscFlags);
        return &e;
    }

    bool CriaTex(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, ComPtr<ID3D11Texture2D> &tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, tex.ReleaseAndGetAddressOf()));
    }

    bool CriaSrv(ID3D11Texture2D *t, DXGI_FORMAT fmt, ComPtr<ID3D11ShaderResourceView> &v) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipLevels = 1;
        return SUCCEEDED(dev->CreateShaderResourceView(t, &d, v.ReleaseAndGetAddressOf()));
    }

    bool CriaUav(ID3D11Texture2D *t, DXGI_FORMAT fmt, ComPtr<ID3D11UnorderedAccessView> &v) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        return SUCCEEDED(dev->CreateUnorderedAccessView(t, &d, v.ReleaseAndGetAddressOf()));
    }

    bool CriaTrabalho(UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D> &t,
                      ComPtr<ID3D11ShaderResourceView> &s, ComPtr<ID3D11UnorderedAccessView> &u) {
        return CriaTex(w, h, fmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, t) &&
               CriaSrv(t.Get(), fmt, s) && CriaUav(t.Get(), fmt, u);
    }

    // Texturas do olho para este tamanho/família. Só aloca quando muda (e aí o histórico recomeça).
    bool Prepara(Olho &o, UINT w, UINT h, DXGI_FORMAT familia, DXGI_FORMAT vista) {
        if (o.ultimo && o.w == w && o.h == h && o.familia == familia) return true;
        const ProjecaoOlho proj = o.proj;
        o = Olho();
        o.proj = proj;
        const UINT wq = w / ref::kEscala, hq = h / ref::kEscala;
        const UINT bw = UINT(ref::NumBlocos(int(w))), bh = UINT(ref::NumBlocos(int(h)));
        // `gerado` tem UAV porque as texturas do driver provavelmente não têm; depois vai por cópia.
        bool ok = CriaTex(w, h, familia, D3D11_BIND_SHADER_RESOURCE, o.ultimo) &&
                  CriaSrv(o.ultimo.Get(), vista, o.ultimoSrv) &&
                  CriaTex(w, h, familia, D3D11_BIND_UNORDERED_ACCESS, o.gerado) &&
                  CriaUav(o.gerado.Get(), vista, o.geradoUav) &&
                  CriaTrabalho(wq, hq, DXGI_FORMAT_R32_FLOAT, o.luma[0], o.lumaSrv[0], o.lumaUav[0]) &&
                  CriaTrabalho(wq, hq, DXGI_FORMAT_R32_FLOAT, o.luma[1], o.lumaSrv[1], o.lumaUav[1]) &&
                  CriaTrabalho(wq, hq, DXGI_FORMAT_R32_FLOAT, o.lumaAntW, o.lumaAntWSrv, o.lumaAntWUav) &&
                  CriaTrabalho(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, o.blocos, o.blocosSrv, o.blocosUav) &&
                  CriaTrabalho(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, o.campo, o.campoSrv, o.campoUav);
        if (!ok) {
            GF_LOG_1X("FrameGen: falha criando as texturas de %ux%u fmt=%d", w, h, (int)familia);
            o = Olho();
            o.proj = proj;
            return false;
        }
        o.w = w;
        o.h = h;
        o.familia = familia;
        GF_LOG("FrameGen: olho %ux%u fmt=%d, luma %ux%u, %ux%u blocos", w, h, (int)familia, wq, hq, bw, bh);
        return true;
    }

    void Tamanhos(const Olho &o) {
        p.cheio[0] = o.w;
        p.cheio[1] = o.h;
        p.luma[0] = o.w / ref::kEscala;
        p.luma[1] = o.h / ref::kEscala;
        p.blocos[0] = UINT(ref::NumBlocos(int(o.w)));
        p.blocos[1] = UINT(ref::NumBlocos(int(o.h)));
    }

    void Despacha(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *const (&srv)[4],
                  ID3D11UnorderedAccessView *const (&uav)[3], UINT gx, UINT gy) {
        static ID3D11ShaderResourceView *const kSemSrv[4] = {};
        static ID3D11UnorderedAccessView *const kSemUav[3] = {};
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, &p, 0, 0);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetShaderResources(0, 4, srv);
        ctx->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
        ctx->Dispatch(gx, gy, 1);
        // Desliga tudo: a próxima passada usa as mesmas texturas no papel oposto.
        ctx->CSSetShaderResources(0, 4, kSemSrv);
        ctx->CSSetUnorderedAccessViews(0, 3, kSemUav, nullptr);
    }

    void IniciaMedida(int tipo) {
        medindo = &medidas[proxMedida];
        proxMedida = (proxMedida + 1) % kMedidas;
        medindo->tipo = tipo;
        medindo->pendente = true;
        ctx->Begin(medindo->disjunto.Get());
        ctx->End(medindo->t0.Get());
    }

    void TerminaMedida() {
        ctx->End(medindo->t1.Get());
        ctx->End(medindo->disjunto.Get());
    }

    // Lê as medidas prontas, da mais antiga para a mais nova, sem esperar a GPU.
    void LerTempos() {
        for (int i = 0; i < kMedidas; ++i) {
            Medida &md = medidas[(proxMedida + i) % kMedidas];
            if (!md.pendente) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 a = 0, b = 0;
            if (ctx->GetData(md.disjunto.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(md.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(md.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            md.pendente = false;
            if (!dj.Disjoint && dj.Frequency && b >= a) ms[md.tipo] = float(double(b - a) * 1000.0 / double(dj.Frequency));
        }
    }

    bool CompilaShaders(decltype(&D3DCompile) compila) {
        char escala[8], bloco[8], busca[8], nbloco[8];
        std::snprintf(escala, sizeof(escala), "%d", ref::kEscala);
        std::snprintf(bloco, sizeof(bloco), "%d", ref::kBloco);
        std::snprintf(busca, sizeof(busca), "%d", ref::kBusca);
        // Já calculado: o [numthreads] de alguns compiladores (vkd3d no Wine) não aceita conta.
        std::snprintf(nbloco, sizeof(nbloco), "%d", ref::kBloco * ref::kBloco);
        const D3D_SHADER_MACRO macros[] = {
            {"ESCALA", escala}, {"BLOCO", bloco}, {"BUSCA", busca}, {"NBLOCO", nbloco}, {nullptr, nullptr}};
        struct {
            const char *nome;
            ComPtr<ID3D11ComputeShader> *cs;
        } lista[] = {{"CSLuma", &csLuma},
                     {"CSRotaciona", &csRotaciona},
                     {"CSBlocos", &csBlocos},
                     {"CSSuaviza", &csSuaviza},
                     {"CSGera", &csGera}};
        for (auto &s : lista) {
            ComPtr<ID3DBlob> codigo, erros;
            HRESULT hr = compila(kHlsl, sizeof(kHlsl) - 1, "framegen.hlsl", macros, nullptr, s.nome, "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, codigo.GetAddressOf(), erros.GetAddressOf());
            if (FAILED(hr)) {
                GF_LOG("FrameGen: shader %s não compilou (0x%08lx): %s", s.nome, (unsigned long)hr,
                       erros ? static_cast<const char *>(erros->GetBufferPointer()) : "");
                return false;
            }
            hr = dev->CreateComputeShader(codigo->GetBufferPointer(), codigo->GetBufferSize(), nullptr,
                                          s.cs->ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                GF_LOG("FrameGen: CreateComputeShader(%s) falhou: 0x%08lx", s.nome, (unsigned long)hr);
                return false;
            }
        }
        return true;
    }

    // O driver faz AcquireSync(0)/ReleaseSync(0) no syncTexture de cada Present; o nosso fica
    // sempre livre. Sem ele (falhou criar), os gerados usam o do compositor, como antes.
    void CriaSyncProprio() {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 1;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<IDXGIResource> r;
        HANDLE h = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, syncProprio.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(syncProprio.As(&r)) && SUCCEEDED(r->GetSharedHandle(&h)) && h) {
            hSyncProprio = vr::SharedTextureHandle_t(reinterpret_cast<uintptr_t>(h));
            GF_LOG("FrameGen: syncTexture próprio 0x%llx para os gerados", (unsigned long long)hSyncProprio);
        } else {
            syncProprio.Reset();
            GF_LOG("FrameGen: sem syncTexture próprio, os gerados usam o do compositor");
        }
    }

    bool CriaFixos() {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Params);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, cb.ReleaseAndGetAddressOf()))) return false;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(dev->CreateQuery(&qd, evento.ReleaseAndGetAddressOf()))) return false;
        for (Medida &md : medidas) {
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(dev->CreateQuery(&qd, md.disjunto.ReleaseAndGetAddressOf()))) return false;
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev->CreateQuery(&qd, md.t0.ReleaseAndGetAddressOf())) ||
                FAILED(dev->CreateQuery(&qd, md.t1.ReleaseAndGetAddressOf())))
                return false;
        }
        p.vies = ref::kVies;
        p.custoBom = ref::kCustoBom;
        p.custoRuim = ref::kCustoRuim;
        return true;
    }
};

FrameGen::FrameGen() : m(new (std::nothrow) Impl) {}

FrameGen::~FrameGen() {
    Destravar();
    delete m;
}

bool FrameGen::Iniciar(bool warp) {
    if (!m) return false;
    if (m->ctx) return true;
    // Carregado na hora: sem d3dcompiler_47.dll o proxy ainda carrega e só fica sem frame gen.
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    auto compila = dll ? reinterpret_cast<decltype(&D3DCompile)>(reinterpret_cast<void *>(GetProcAddress(dll, "D3DCompile")))
                       : nullptr;
    if (!compila) {
        GF_LOG("FrameGen: d3dcompiler_47.dll/D3DCompile indisponível");
        return false;
    }
    const D3D_FEATURE_LEVEL niveis[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_DRIVER_TYPE tipo = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    D3D_FEATURE_LEVEL nivel{};
    HRESULT hr = D3D11CreateDevice(nullptr, tipo, nullptr, 0, niveis, 2, D3D11_SDK_VERSION,
                                   m->dev.ReleaseAndGetAddressOf(), &nivel, m->ctx.ReleaseAndGetAddressOf());
    if (hr == E_INVALIDARG) // runtime antigo sem 11_1
        hr = D3D11CreateDevice(nullptr, tipo, nullptr, 0, niveis + 1, 1, D3D11_SDK_VERSION,
                               m->dev.ReleaseAndGetAddressOf(), &nivel, m->ctx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        GF_LOG("FrameGen: D3D11CreateDevice(%s) falhou: 0x%08lx", warp ? "WARP" : "hardware", (unsigned long)hr);
        m->ctx.Reset();
        return false;
    }
    m->dev.As(&m->dev1);
    if (!m->CompilaShaders(compila) || !m->CriaFixos()) {
        GF_LOG("FrameGen: iniciação incompleta, frame gen desligado");
        m->ctx.Reset();
        m->dev1.Reset();
        m->dev.Reset();
        return false;
    }
    m->CriaSyncProprio();
    // Qual GPU: as texturas do compositor só abrem no mesmo adaptador do SteamVR.
    char gpu[128] = "?";
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adaptador;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(m->dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adaptador)) && SUCCEEDED(adaptador->GetDesc(&desc)))
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu, sizeof(gpu), nullptr, nullptr);
    GF_LOG("FrameGen: iniciado (%s, %s, feature level 0x%x)", warp ? "WARP" : "hardware", gpu, (unsigned)nivel);
    return true;
}

bool FrameGen::Iniciado() const { return m && m->ctx; }
vr::SharedTextureHandle_t FrameGen::SyncProprio() const { return m ? m->hSyncProprio : 0; }

void FrameGen::DefinirProjecao(int olho, const ProjecaoOlho &p) {
    if (!m || olho < 0 || olho > 1) return;
    if (!std::isfinite(p.esq) || !std::isfinite(p.dir) || !std::isfinite(p.cima) || !std::isfinite(p.baixo) ||
        !(p.dir > p.esq && p.baixo > p.cima)) {
        GF_LOG_1X("FrameGen: projeção inválida no olho %d [%f %f %f %f], ignorada", olho, p.esq, p.dir, p.cima,
                  p.baixo);
        return;
    }
    m->olhos[olho].proj = p;
}

bool FrameGen::Travar(vr::SharedTextureHandle_t sync, uint32_t esperaMs) {
    if (!Iniciado() || m->travado) return false;
    // Só handle legado: um NT do sync seria de outro processo (o compositor), e abrir o
    // valor aqui pegaria um objeto qualquer do vrserver.
    Impl::TexAberta *t = m->Abrir(sync, false);
    if (!t) return false;
    ComPtr<IDXGIKeyedMutex> km;
    if (FAILED(t->tex.As(&km))) {
        GF_LOG_1X("FrameGen: syncTexture 0x%llx sem keyed mutex", (unsigned long long)sync);
        return false;
    }
    // WAIT_TIMEOUT e WAIT_ABANDONED passam em SUCCEEDED(); só S_OK é a chave na mão.
    HRESULT hr = km->AcquireSync(0, esperaMs);
    if (hr != S_OK) {
        GF_LOG_1X("FrameGen: AcquireSync do sync falhou: 0x%08lx", (unsigned long)hr);
        return false;
    }
    m->travado = km;
    return true;
}

void FrameGen::Destravar() {
    if (!m || !m->travado) return;
    m->travado->ReleaseSync(0);
    m->travado.Reset();
}

bool FrameGen::NovoFrame(const vr::SharedTextureHandle_t tex[2], const vr::VRTextureBounds_t bounds[2],
                         const vr::HmdMatrix34_t &pose, bool ntHandle) {
    if (!Iniciado() || !tex || !bounds) return false;
    m->LerTempos();
    // Valida os dois olhos antes de mandar qualquer coisa para a GPU.
    ID3D11Texture2D *src[2] = {};
    D3D11_BOX caixa[2] = {};
    for (int e = 0; e < 2; ++e) {
        Impl::TexAberta *t = m->Abrir(tex[e], ntHandle);
        if (!t) return false;
        DXGI_FORMAT familia, vista;
        if (!Familia(t->desc.Format, familia, vista)) {
            GF_LOG_1X("FrameGen: formato %d do jogo não suportado (só RGBA8/BGRA8)", (int)t->desc.Format);
            return false;
        }
        if (t->desc.SampleDesc.Count != 1) {
            GF_LOG_1X("FrameGen: textura do jogo com MSAA (%u amostras) não suportada", t->desc.SampleDesc.Count);
            return false;
        }
        if (!Regiao(t->desc, bounds[e], caixa[e])) {
            GF_LOG_1X("FrameGen: bounds inválidos [%f %f %f %f]", bounds[e].uMin, bounds[e].vMin, bounds[e].uMax,
                      bounds[e].vMax);
            return false;
        }
        const UINT w = caixa[e].right - caixa[e].left, h = caixa[e].bottom - caixa[e].top;
        if (w < UINT(ref::kMinLado) || h < UINT(ref::kMinLado)) {
            GF_LOG_1X("FrameGen: região %ux%u pequena demais", w, h);
            return false;
        }
        if (!m->Prepara(m->olhos[e], w, h, familia, vista)) return false;
        src[e] = t->tex.Get();
    }

    m->IniciaMedida(0);
    for (int e = 0; e < 2; ++e)
        m->ctx->CopySubresourceRegion(m->olhos[e].ultimo.Get(), 0, 0, 0, 0, src[e], 0, &caixa[e]);
    // As texturas do jogo já estão copiadas: devolve o sync ao driver antes do cálculo do
    // movimento (o ReleaseSync faz o driver esperar só pelo que veio antes dele).
    if (m->travado) {
        m->travado->ReleaseSync(0);
        m->travado.Reset();
    }
    for (int e = 0; e < 2; ++e) {
        Impl::Olho &o = m->olhos[e];
        m->Tamanhos(o);
        const UINT wq = m->p.luma[0], hq = m->p.luma[1], bw = m->p.blocos[0], bh = m->p.blocos[1];
        const int novo = o.atual ^ 1;
        m->Despacha(m->csLuma.Get(), {o.ultimoSrv.Get(), nullptr, nullptr, nullptr},
                    {o.lumaUav[novo].Get(), nullptr, nullptr}, Grupos(wq), Grupos(hq));
        if (o.frames >= 1) {
            float H[3][3];
            ref::Homografia(o.pose, pose, o.proj, int(o.w), int(o.h), H);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) m->p.h[i][j] = H[i][j];
            m->Despacha(m->csRotaciona.Get(), {nullptr, o.lumaSrv[o.atual].Get(), nullptr, nullptr},
                        {o.lumaAntWUav.Get(), nullptr, nullptr}, Grupos(wq), Grupos(hq));
            m->Despacha(m->csBlocos.Get(), {nullptr, o.lumaSrv[novo].Get(), o.lumaAntWSrv.Get(), nullptr},
                        {nullptr, o.blocosUav.Get(), nullptr}, bw, bh);
            m->Despacha(m->csSuaviza.Get(), {nullptr, nullptr, nullptr, o.blocosSrv.Get()},
                        {nullptr, o.campoUav.Get(), nullptr}, Grupos(bw), Grupos(bh));
        }
        o.atual = novo;
        o.pose = pose;
        o.frames = std::min(o.frames + 1, 2);
    }
    m->TerminaMedida();
    return true;
}

bool FrameGen::Pronto() const { return Iniciado() && m->olhos[0].frames >= 2 && m->olhos[1].frames >= 2; }

bool FrameGen::Gerar(float alfa, const vr::SharedTextureHandle_t saida[2], const vr::VRTextureBounds_t bounds[2],
                     bool ntHandle) {
    if (!Pronto() || !saida || !bounds || !(alfa > 0.0f && alfa <= 1.0f)) return false;
    m->gerou = false;
    ID3D11Texture2D *dst[2] = {};
    D3D11_BOX caixa[2] = {};
    for (int e = 0; e < 2; ++e) {
        const Impl::Olho &o = m->olhos[e];
        Impl::TexAberta *t = m->Abrir(saida[e], ntHandle);
        if (!t) return false;
        DXGI_FORMAT familia, vista;
        if (!Familia(t->desc.Format, familia, vista) || familia != o.familia) {
            GF_LOG_1X("FrameGen: formato %d da saída não é da família do jogo (%d)", (int)t->desc.Format,
                      (int)o.familia);
            return false;
        }
        if (t->desc.SampleDesc.Count != 1 || !Regiao(t->desc, bounds[e], caixa[e]) ||
            caixa[e].right - caixa[e].left != o.w || caixa[e].bottom - caixa[e].top != o.h) {
            GF_LOG_1X("FrameGen: região da saída não casa com a do jogo (%ux%u)", o.w, o.h);
            return false;
        }
        dst[e] = t->tex.Get();
    }

    m->IniciaMedida(1);
    m->p.alfa = alfa;
    for (int e = 0; e < 2; ++e) {
        const Impl::Olho &o = m->olhos[e];
        m->Tamanhos(o);
        m->Despacha(m->csGera.Get(), {o.ultimoSrv.Get(), nullptr, nullptr, o.campoSrv.Get()},
                    {nullptr, nullptr, o.geradoUav.Get()}, Grupos(o.w), Grupos(o.h));
        m->ctx->CopySubresourceRegion(dst[e], 0, caixa[e].left, caixa[e].top, 0, o.gerado.Get(), 0, nullptr);
    }
    m->TerminaMedida();
    m->ctx->End(m->evento.Get());
    m->ctx->Flush(); // sem isto o Concluido() com DONOTFLUSH poderia nunca ver o fim
    m->gerou = true;
    return true;
}

bool FrameGen::Concluido() {
    if (!Iniciado() || !m->gerou) return false;
    m->LerTempos();
    BOOL feito = FALSE;
    return m->ctx->GetData(m->evento.Get(), &feito, sizeof(feito), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && feito;
}

float FrameGen::MsGpu() const {
    if (!m || m->ms[1] < 0) return -1;
    return m->ms[1] + std::max(m->ms[0], 0.0f);
}

void FrameGen::Esquecer(vr::SharedTextureHandle_t h) {
    if (!m || !h) return;
    for (Impl::TexAberta &c : m->cache)
        if (c.handle == h) c = Impl::TexAberta();
}

void FrameGen::Reiniciar() {
    if (!m) return;
    for (Impl::Olho &o : m->olhos) o.frames = 0;
    for (Impl::TexAberta &c : m->cache) c = Impl::TexAberta();
    m->gerou = false;
}

ID3D11Device *FrameGenTeste::Device(FrameGen &fg) { return fg.m ? fg.m->dev.Get() : nullptr; }

bool FrameGenTeste::Injetar(FrameGen &fg, vr::SharedTextureHandle_t h, ID3D11Texture2D *tex) {
    if (!fg.Iniciado() || !h || !tex) return false;
    bool achou = false;
    FrameGen::Impl::TexAberta &e = fg.m->Entrada(h, false, achou);
    e.tex = tex;
    tex->GetDesc(&e.desc);
    return true;
}

} // namespace gf

#else // Fora do Windows não há D3D11: tudo indisponível, só para o proxy compilar nos testes.

namespace gf {

struct FrameGen::Impl {};

FrameGen::FrameGen() {}
FrameGen::~FrameGen() { delete m; }
bool FrameGen::Iniciar(bool) { return false; }
bool FrameGen::Iniciado() const { return false; }
void FrameGen::DefinirProjecao(int, const ProjecaoOlho &) {}
bool FrameGen::Travar(vr::SharedTextureHandle_t, uint32_t) { return false; }
vr::SharedTextureHandle_t FrameGen::SyncProprio() const { return 0; }
void FrameGen::Destravar() {}
bool FrameGen::NovoFrame(const vr::SharedTextureHandle_t[2], const vr::VRTextureBounds_t[2], const vr::HmdMatrix34_t &,
                         bool) {
    return false;
}
bool FrameGen::Pronto() const { return false; }
bool FrameGen::Gerar(float, const vr::SharedTextureHandle_t[2], const vr::VRTextureBounds_t[2], bool) { return false; }
bool FrameGen::Concluido() { return false; }
float FrameGen::MsGpu() const { return -1; }
void FrameGen::Esquecer(vr::SharedTextureHandle_t) {}
void FrameGen::Reiniciar() {}

} // namespace gf

#endif
