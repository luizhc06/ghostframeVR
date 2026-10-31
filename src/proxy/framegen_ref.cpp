#include "framegen_ref.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gf {
namespace ref {

void Homografia(const vr::HmdMatrix34_t &ant, const vr::HmdMatrix34_t &atual, const ProjecaoOlho &p, int w, int h,
                float H[3][3]) {
    // Rotação relativa no espaço do HMD: d_ant = R_ant^T · R_atual · d_atual
    // (pose = HMD -> mundo, então R^T leva do mundo de volta ao HMD).
    double R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            R[i][j] = ant.m[0][i] * atual.m[0][j] + ant.m[1][i] * atual.m[1][j] + ant.m[2][i] * atual.m[2][j];
    // Câmera (x direita, y para baixo, z para frente) = F · HMD do OpenVR (y para cima,
    // -z para frente), F = diag(1, -1, -1). M = F·R·F só troca sinais.
    const double s[3] = {1, -1, -1};
    // K da luma: tangente -> pixel, com o centro do pixel i em i + 0.5.
    double fx = double(w) / kEscala / (p.dir - p.esq), cx = -p.esq * fx;
    double fy = double(h) / kEscala / (p.baixo - p.cima), cy = -p.cima * fy;
    const double K[3][3] = {{fx, 0, cx}, {0, fy, cy}, {0, 0, 1}};
    const double Ki[3][3] = {{1 / fx, 0, -cx / fx}, {0, 1 / fy, -cy / fy}, {0, 0, 1}};
    double MK[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            MK[i][j] = 0;
            for (int k = 0; k < 3; ++k) MK[i][j] += s[i] * R[i][k] * s[k] * Ki[k][j];
        }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += K[i][k] * MK[k][j];
            H[i][j] = float(v);
        }
}

void Luma(const uint8_t *rgba, int stride, int w, int h, std::vector<float> &luma) {
    int wq = w / kEscala, hq = h / kEscala;
    luma.assign(size_t(wq) * hq, 0.0f);
    for (int qy = 0; qy < hq; ++qy)
        for (int qx = 0; qx < wq; ++qx) {
            float soma = 0;
            for (int j = 0; j < kEscala; ++j)
                for (int i = 0; i < kEscala; ++i) {
                    const uint8_t *px = rgba + size_t(qy * kEscala + j) * stride + size_t(qx * kEscala + i) * 4;
                    soma += px[0] / 255.0f * 0.2126f + px[1] / 255.0f * 0.7152f + px[2] / 255.0f * 0.0722f;
                }
            luma[size_t(qy) * wq + qx] = soma / (kEscala * kEscala);
        }
}

// Bilinear com centro do texel em i + 0.5 e borda repetida (igual aos shaders).
static void Pesos(float s, int lado, int &i0, int &i1, float &f) {
    float c = std::min(std::max(s - 0.5f, 0.0f), float(lado - 1));
    i0 = int(std::floor(c));
    f = c - float(i0);
    i1 = std::min(i0 + 1, lado - 1);
}

static float Lerp(float a, float b, float f) { return a + f * (b - a); }

void Rotaciona(const std::vector<float> &ant, int wq, int hq, const float H[3][3], std::vector<float> &out) {
    out.assign(size_t(wq) * hq, kInvalido);
    for (int y = 0; y < hq; ++y)
        for (int x = 0; x < wq; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float qx = H[0][0] * px + H[0][1] * py + H[0][2];
            float qy = H[1][0] * px + H[1][1] * py + H[1][2];
            float qz = H[2][0] * px + H[2][1] * py + H[2][2];
            if (!(qz > 1e-6f)) continue; // atrás da câmera anterior
            float sx = qx / qz, sy = qy / qz;
            if (!(sx >= 0 && sy >= 0 && sx <= float(wq) && sy <= float(hq))) continue;
            int x0, x1, y0, y1;
            float fx, fy;
            Pesos(sx, wq, x0, x1, fx);
            Pesos(sy, hq, y0, y1, fy);
            float a = Lerp(ant[size_t(y0) * wq + x0], ant[size_t(y0) * wq + x1], fx);
            float b = Lerp(ant[size_t(y1) * wq + x0], ant[size_t(y1) * wq + x1], fx);
            out[size_t(y) * wq + x] = Lerp(a, b, fy);
        }
}

void Blocos(const std::vector<float> &atual, const std::vector<float> &antW, int wq, int hq,
            std::vector<float> &blocos) {
    const int bw = (wq + kBloco - 1) / kBloco, bh = (hq + kBloco - 1) / kBloco;
    const int lado = 2 * kBusca + 1, janela = kBloco + 2 * kBusca;
    blocos.assign(size_t(bw) * bh * 4, 0.0f);
    auto le = [&](const std::vector<float> &t, int x, int y) {
        return (x < 0 || y < 0 || x >= wq || y >= hq) ? kInvalido : t[size_t(y) * wq + x];
    };
    float jan[janela * janela], blo[kBloco * kBloco];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            int x0 = bx * kBloco, y0 = by * kBloco;
            for (int i = 0; i < janela * janela; ++i)
                jan[i] = le(antW, x0 - kBusca + i % janela, y0 - kBusca + i / janela);
            int nBloco = 0, nVisto = 0;
            for (int i = 0; i < kBloco * kBloco; ++i) {
                blo[i] = le(atual, x0 + i % kBloco, y0 + i / kBloco);
                nBloco += blo[i] >= 0;
                nVisto += blo[i] >= 0 && jan[(i / kBloco + kBusca) * janela + i % kBloco + kBusca] >= 0;
            }
            float *b = &blocos[(size_t(by) * bw + bx) * 4];
            // Borda revelada pelo giro: o frame anterior não via o bloco, qualquer vetor
            // seria chute. Sem vetor e confiança zero (fica o último frame).
            if (nBloco == 0 || 2 * nVisto < nBloco) {
                b[2] = kGrande;
                continue;
            }
            // Ordem fixa dos candidatos; empate fica com o primeiro (a GPU reduz igual).
            float melhor = kGrande, bruto = kGrande;
            int idx = -1;
            for (int k = 0; k < lado * lado; ++k) {
                int dx = k % lado - kBusca, dy = k / lado - kBusca;
                float sad = 0;
                int n = 0;
                for (int i = 0; i < kBloco * kBloco; ++i) {
                    float c = blo[i];
                    float a = jan[(i / kBloco + kBusca - dy) * janela + (i % kBloco + kBusca - dx)];
                    if (c >= 0 && a >= 0) {
                        sad += std::fabs(c - a);
                        ++n;
                    }
                }
                // Metade do bloco precisa ter casado com pixels válidos.
                if (n == 0 || 2 * n < nBloco) continue;
                float custo = sad / float(n);
                float comVies = custo + kVies * float(std::abs(dx) + std::abs(dy));
                if (comVies < melhor) {
                    melhor = comVies;
                    bruto = custo;
                    idx = k;
                }
            }
            if (idx < 0) {
                b[2] = kGrande; // sem casamento: vetor zero e confiança zero
                continue;
            }
            b[0] = float(idx % lado - kBusca);
            b[1] = float(idx / lado - kBusca);
            b[2] = bruto;
        }
}

void Suaviza(const std::vector<float> &blocos, int bw, int bh, std::vector<float> &campo) {
    campo.assign(size_t(bw) * bh * 4, 0.0f);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            float vx[9], vy[9];
            for (int i = 0; i < 9; ++i) {
                int qx = std::min(std::max(x + i % 3 - 1, 0), bw - 1);
                int qy = std::min(std::max(y + i / 3 - 1, 0), bh - 1);
                vx[i] = blocos[(size_t(qy) * bw + qx) * 4];
                vy[i] = blocos[(size_t(qy) * bw + qx) * 4 + 1];
            }
            // Mediana vetorial: o vizinho com menor soma de distâncias L1 aos outros.
            float melhor = kGrande, mx = 0, my = 0;
            for (int a = 0; a < 9; ++a) {
                float s = 0;
                for (int b = 0; b < 9; ++b) s += std::fabs(vx[a] - vx[b]) + std::fabs(vy[a] - vy[b]);
                if (s < melhor) {
                    melhor = s;
                    mx = vx[a];
                    my = vy[a];
                }
            }
            float custo = blocos[(size_t(y) * bw + x) * 4 + 2];
            float conf = std::min(std::max((kCustoRuim - custo) / (kCustoRuim - kCustoBom), 0.0f), 1.0f);
            campo[(size_t(y) * bw + x) * 4] = mx * conf;
            campo[(size_t(y) * bw + x) * 4 + 1] = my * conf;
        }
}

void Gera(const uint8_t *ultimo, int stride, int w, int h, const std::vector<float> &campo, float alfa,
          uint8_t *saida, int strideSaida) {
    const int bw = NumBlocos(w), bh = NumBlocos(h);
    const float passo = float(kEscala * kBloco);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            // Centro do bloco b em (b + 0.5) * passo pixels cheios.
            int bx0, bx1, by0, by1;
            float fx, fy;
            Pesos(px / passo, bw, bx0, bx1, fx);
            Pesos(py / passo, bh, by0, by1, fy);
            float v[2];
            for (int c = 0; c < 2; ++c) {
                float a = Lerp(campo[(size_t(by0) * bw + bx0) * 4 + c], campo[(size_t(by0) * bw + bx1) * 4 + c], fx);
                float b = Lerp(campo[(size_t(by1) * bw + bx0) * 4 + c], campo[(size_t(by1) * bw + bx1) * 4 + c], fx);
                v[c] = Lerp(a, b, fy) * kEscala;
            }
            int x0, x1, y0, y1;
            Pesos(px - alfa * v[0], w, x0, x1, fx);
            Pesos(py - alfa * v[1], h, y0, y1, fy);
            const uint8_t *p00 = ultimo + size_t(y0) * stride + size_t(x0) * 4;
            const uint8_t *p10 = ultimo + size_t(y0) * stride + size_t(x1) * 4;
            const uint8_t *p01 = ultimo + size_t(y1) * stride + size_t(x0) * 4;
            const uint8_t *p11 = ultimo + size_t(y1) * stride + size_t(x1) * 4;
            uint8_t *o = saida + size_t(y) * strideSaida + size_t(x) * 4;
            for (int c = 0; c < 4; ++c) {
                float a = Lerp(p00[c] / 255.0f, p10[c] / 255.0f, fx);
                float b = Lerp(p01[c] / 255.0f, p11[c] / 255.0f, fx);
                float r = std::min(std::max(Lerp(a, b, fy), 0.0f), 1.0f);
                o[c] = uint8_t(r * 255.0f + 0.5f);
            }
        }
}

bool NovoFrame(Olho &o, const uint8_t *rgba, int stride, int w, int h, const vr::HmdMatrix34_t &pose) {
    if (!rgba || w < kMinLado || h < kMinLado || stride < w * 4) return false;
    if (w != o.w || h != o.h) {
        o.w = w;
        o.h = h;
        o.frames = 0;
    }
    o.ultimo.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) std::memcpy(&o.ultimo[size_t(y) * w * 4], rgba + size_t(y) * stride, size_t(w) * 4);
    o.lumaAnt.swap(o.luma);
    Luma(o.ultimo.data(), w * 4, w, h, o.luma);
    if (o.frames >= 1) {
        const int wq = w / kEscala, hq = h / kEscala;
        float H[3][3];
        Homografia(o.pose, pose, o.proj, w, h, H);
        std::vector<float> antW, blocos;
        Rotaciona(o.lumaAnt, wq, hq, H, antW);
        Blocos(o.luma, antW, wq, hq, blocos);
        Suaviza(blocos, NumBlocos(w), NumBlocos(h), o.campo);
    }
    o.pose = pose;
    o.frames = std::min(o.frames + 1, 2);
    return true;
}

bool Gerar(const Olho &o, float alfa, uint8_t *saida, int stride) {
    if (!Pronto(o) || !saida || stride < o.w * 4 || !(alfa > 0.0f && alfa <= 1.0f)) return false;
    Gera(o.ultimo.data(), o.w * 4, o.w, o.h, o.campo, alfa, saida, stride);
    return true;
}

} // namespace ref
} // namespace gf
