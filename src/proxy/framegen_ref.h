// Referência em CPU do motor do frame gen: o mesmo algoritmo dos shaders de
// framegen.cpp, passo a passo, em C++ puro. É a especificação executável: os testes
// no Linux validam o algoritmo aqui e o framegen_d3d_test compara a GPU com isto.
// Convenções (pose, homografia, sentido dos vetores) em docs/FRAMEGEN.md.
#pragma once

#include "framegen.h"

#include <cstdint>
#include <vector>

namespace gf {
namespace ref {

// Parâmetros compartilhados com os shaders (viram macros e constant buffer lá).
constexpr int kEscala = 4;            // luma em 1/4 da resolução, por eixo
constexpr int kBloco = 8;             // blocos de 8×8 pixels de luma
constexpr int kBusca = 8;             // ±8 pixels de luma (±32 na resolução cheia)
constexpr int kMinLado = 64;          // região mínima de cada olho, em pixels cheios
constexpr float kVies = 1.0f / 1024;  // custo extra por pixel de deslocamento: em área lisa ganha o zero
constexpr float kCustoBom = 0.02f;    // |Δluma| médio do melhor casamento: até aqui, vetor inteiro
constexpr float kCustoRuim = 0.06f;   // daqui para cima, vetor zero (fica o último frame)
constexpr float kInvalido = -1.0f;    // luma do frame anterior fora da vista atual
constexpr float kGrande = 1e9f;

inline int NumBlocos(int ladoCheio) { return (ladoCheio / kEscala + kBloco - 1) / kBloco; }

// H (3×3, por linhas) leva um ponto da luma do frame atual ao ponto da luma do frame
// anterior que via a mesma direção do mundo. w×h = região do olho em pixels cheios.
void Homografia(const vr::HmdMatrix34_t &poseAnt, const vr::HmdMatrix34_t &poseAtual,
                const ProjecaoOlho &p, int w, int h, float H[3][3]);

// Passos (as mesmas passadas dos compute shaders). Imagens RGBA8 com stride em bytes.
void Luma(const uint8_t *rgba, int stride, int w, int h, std::vector<float> &luma);
void Rotaciona(const std::vector<float> &lumaAnt, int wq, int hq, const float H[3][3], std::vector<float> &out);
// 4 floats por bloco: vx, vy (pixels de luma, atual(p) ≈ antW(p − v)), custo, 0.
void Blocos(const std::vector<float> &atual, const std::vector<float> &antW, int wq, int hq,
            std::vector<float> &blocos);
// 4 floats por bloco: mediana vetorial 3×3 vezes a confiança do bloco, 0, 0.
void Suaviza(const std::vector<float> &blocos, int bw, int bh, std::vector<float> &campo);
void Gera(const uint8_t *ultimo, int stride, int w, int h, const std::vector<float> &campo, float alfa,
          uint8_t *saida, int strideSaida);

// Estado de um olho, como o motor da GPU guarda.
struct Olho {
    ProjecaoOlho proj;
    int w = 0, h = 0, frames = 0;
    std::vector<uint8_t> ultimo; // RGBA8, stride w*4
    std::vector<float> luma, lumaAnt, campo;
    vr::HmdMatrix34_t pose{};
};

bool NovoFrame(Olho &o, const uint8_t *rgba, int stride, int w, int h, const vr::HmdMatrix34_t &pose);
inline bool Pronto(const Olho &o) { return o.frames >= 2; }
bool Gerar(const Olho &o, float alfa, uint8_t *saida, int stride);

} // namespace ref
} // namespace gf
