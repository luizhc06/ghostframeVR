// Testes da referência em CPU do frame gen (roda em qualquer plataforma).
// Cena sintética: ruído em volta da cabeça + quadrado andando, com a cabeça girando.
#include "cena_sintetica.h"
#include "framegen_ref.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace gf;

namespace {

int g_fail = 0;
void Check(bool ok, const char *expr, int linha) {
    std::printf("%s: %s (linha %d)\n", ok ? "ok" : "FALHOU", expr, linha);
    if (!ok) g_fail++;
}
#define CHECK(c) Check((c), #c, __LINE__)

constexpr int W = 512, H = 512;
const ProjecaoOlho kProj{-1.15f, 0.95f, -1.05f, 1.0f}; // assimétrica de propósito

struct Campo {
    int bw = 0, bh = 0;
    const std::vector<float> *v = nullptr;
    float x(int bx, int by) const { return (*v)[(size_t(by) * bw + bx) * 4]; }
    float y(int bx, int by) const { return (*v)[(size_t(by) * bw + bx) * 4 + 1]; }
};

Campo CampoDe(const std::vector<float> &v) { return {ref::NumBlocos(W), ref::NumBlocos(H), &v}; }

// Fração dos blocos com |v|∞ abaixo de `lim`.
double FracaoParada(const Campo &c, float lim) {
    int n = 0;
    for (int y = 0; y < c.bh; ++y)
        for (int x = 0; x < c.bw; ++x) n += std::fabs(c.x(x, y)) < lim && std::fabs(c.y(x, y)) < lim;
    return double(n) / (c.bw * c.bh);
}

// Direção do centro do bloco (pixels cheios) na vista `pose`.
void DirecaoBloco(const vr::HmdMatrix34_t &pose, int bx, int by, double dx, double dy, double d[3]) {
    const double passo = ref::kEscala * ref::kBloco;
    cena::Direcao(pose, kProj, W, H, (bx + 0.5 + dx) * passo, (by + 0.5 + dy) * passo, d);
}

// Bloco inteiro dentro do quadrado (com margem) nos instantes t0 e t1.
bool BlocoNoQuadrado(const cena::Cena &c, const vr::HmdMatrix34_t &pose, int bx, int by, double t0, double t1) {
    for (int k = 0; k < 4; ++k) {
        double d[3], az, el;
        DirecaoBloco(pose, bx, by, k % 2 ? 0.5 : -0.5, k / 2 ? 0.5 : -0.5, d);
        cena::Angulos(d, az, el);
        if (!c.NoQuadrado(az, el, t0, 0.02) || !c.NoQuadrado(az, el, t1, 0.02)) return false;
    }
    return true;
}

// Bloco longe do quadrado nos dois instantes.
bool BlocoNoFundo(const cena::Cena &c, const vr::HmdMatrix34_t &pose, int bx, int by, double t0, double t1) {
    double d[3], az, el;
    DirecaoBloco(pose, bx, by, 0, 0, d);
    cena::Angulos(d, az, el);
    return !c.NoQuadrado(az, el, t0, -0.2) && !c.NoQuadrado(az, el, t1, -0.2);
}

// Erro médio por canal entre duas imagens W×H nos pixels onde `mascara` vale.
template <class F> double Erro(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, F mascara) {
    double s = 0;
    long n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!mascara(x, y)) continue;
            for (int c = 0; c < 3; ++c) s += std::abs(int(a[(size_t(y) * W + x) * 4 + c]) - int(b[(size_t(y) * W + x) * 4 + c]));
            n += 3;
        }
    return n ? s / n : -1;
}

ref::Olho Analisa(const std::vector<uint8_t> &f0, const vr::HmdMatrix34_t &p0, const std::vector<uint8_t> &f1,
                  const vr::HmdMatrix34_t &p1) {
    ref::Olho o;
    o.proj = kProj;
    ref::NovoFrame(o, f0.data(), W * 4, W, H, p0);
    ref::NovoFrame(o, f1.data(), W * 4, W, H, p1);
    return o;
}

// Campo sem a compensação certa: casa a luma atual direto com a anterior passada por H.
std::vector<float> CampoCom(const std::vector<uint8_t> &f0, const std::vector<uint8_t> &f1, const float Hm[3][3]) {
    const int wq = W / ref::kEscala, hq = H / ref::kEscala;
    std::vector<float> l0, l1, antW, blocos, campo;
    ref::Luma(f0.data(), W * 4, W, H, l0);
    ref::Luma(f1.data(), W * 4, W, H, l1);
    ref::Rotaciona(l0, wq, hq, Hm, antW);
    ref::Blocos(l1, antW, wq, hq, blocos);
    ref::Suaviza(blocos, ref::NumBlocos(W), ref::NumBlocos(H), campo);
    return campo;
}

void TesteHomografia() {
    std::printf("\n== homografia ==\n");
    float Hm[3][3];
    vr::HmdMatrix34_t p0 = cena::Pose(0, 0, 0);
    ref::Homografia(p0, p0, kProj, W, H, Hm);
    float erro = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) erro = std::max(erro, std::fabs(Hm[i][j] - (i == j ? 1.0f : 0.0f)));
    CHECK(erro < 1e-5f);
    // Cabeça virou para a esquerda: o que o centro vê agora estava à esquerda do centro antes.
    ref::Homografia(p0, cena::Pose(cena::Graus(5), 0, 0), kProj, W, H, Hm);
    const float cx = -kProj.esq * (W / ref::kEscala) / (kProj.dir - kProj.esq);
    const float cy = -kProj.cima * (H / ref::kEscala) / (kProj.baixo - kProj.cima);
    float qx = (Hm[0][0] * cx + Hm[0][1] * cy + Hm[0][2]) / (Hm[2][0] * cx + Hm[2][1] * cy + Hm[2][2]);
    float qy = (Hm[1][0] * cx + Hm[1][1] * cy + Hm[1][2]) / (Hm[2][0] * cx + Hm[2][1] * cy + Hm[2][2]);
    std::printf("   centro (%.2f, %.2f) -> anterior (%.2f, %.2f)\n", cx, cy, qx, qy);
    CHECK(qx < cx - 3 && std::fabs(qy - cy) < 0.5f);
}

void TesteRotacaoPura() {
    std::printf("\n== só a cabeça gira: depois da compensação, nada se move ==\n");
    cena::Cena c;
    vr::HmdMatrix34_t p0 = cena::Pose(0, 0, 0);
    vr::HmdMatrix34_t p1 = cena::Pose(cena::Graus(2.5), cena::Graus(1.5), cena::Graus(1));
    auto f0 = cena::Renderiza(c, 0, p0, kProj, W, H), f1 = cena::Renderiza(c, 1, p1, kProj, W, H);

    ref::Olho o = Analisa(f0, p0, f1, p1);
    CHECK(ref::Pronto(o));
    double parada = FracaoParada(CampoDe(o.campo), 0.5f);
    std::printf("   blocos parados com compensação: %.1f%%\n", 100 * parada);
    CHECK(parada >= 0.95);

    // Controles: sem compensação e com a convenção invertida o giro aparece como movimento.
    float I[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, Hinv[3][3];
    auto semComp = CampoCom(f0, f1, I);
    ref::Homografia(p1, p0, kProj, W, H, Hinv);
    auto invertida = CampoCom(f0, f1, Hinv);
    double movSem = 1 - FracaoParada(CampoDe(semComp), 1.0f), movInv = 1 - FracaoParada(CampoDe(invertida), 1.0f);
    std::printf("   blocos movendo sem compensação: %.1f%%, com convenção invertida: %.1f%%\n", 100 * movSem,
                100 * movInv);
    CHECK(movSem >= 0.5);
    CHECK(movInv >= 0.5);

    std::vector<uint8_t> g(f1.size());
    CHECK(ref::Gerar(o, 1.0f, g.data(), W * 4));
    double e = Erro(g, f1, [](int, int) { return true; });
    std::printf("   gerado vs último frame: %.3f\n", e);
    CHECK(e < 0.5);
}

void TesteQuadradoAndando() {
    std::printf("\n== quadrado anda para a direita enquanto a cabeça gira ==\n");
    cena::Cena c;
    c.vel = 0.04;
    vr::HmdMatrix34_t p0 = cena::Pose(0, 0, 0);
    vr::HmdMatrix34_t p1 = cena::Pose(cena::Graus(2.5), cena::Graus(-1), 0);
    auto f0 = cena::Renderiza(c, 0, p0, kProj, W, H), f1 = cena::Renderiza(c, 1, p1, kProj, W, H);
    ref::Olho o = Analisa(f0, p0, f1, p1);
    Campo campo = CampoDe(o.campo);

    int dentro = 0, dentroOk = 0, fundo = 0, fundoOk = 0;
    for (int by = 0; by < campo.bh; ++by)
        for (int bx = 0; bx < campo.bw; ++bx) {
            if (BlocoNoQuadrado(c, p1, bx, by, 0, 1)) {
                // Esperado: de onde o mesmo ponto do quadrado estava em t = 0, visto com a pose atual.
                double d[3], az, el, dAnt[3], x1, y1, x0, y0;
                DirecaoBloco(p1, bx, by, 0, 0, d);
                cena::Angulos(d, az, el);
                cena::DeAngulos(az - c.vel, el, dAnt);
                cena::Projeta(p1, kProj, W, H, d, x1, y1);
                cena::Projeta(p1, kProj, W, H, dAnt, x0, y0);
                double ex = (x1 - x0) / ref::kEscala, ey = (y1 - y0) / ref::kEscala;
                bool ok = std::fabs(campo.x(bx, by) - ex) <= 1.0 && std::fabs(campo.y(bx, by) - ey) <= 1.0;
                if (dentro == 0) std::printf("   bloco (%d,%d): v=(%.2f, %.2f) esperado (%.2f, %.2f)\n", bx, by,
                                             campo.x(bx, by), campo.y(bx, by), ex, ey);
                dentro++;
                dentroOk += ok;
            } else if (BlocoNoFundo(c, p1, bx, by, 0, 1)) {
                fundo++;
                fundoOk += std::fabs(campo.x(bx, by)) < 0.5f && std::fabs(campo.y(bx, by)) < 0.5f;
            }
        }
    std::printf("   quadrado: %d/%d blocos com o vetor esperado; fundo: %d/%d parados\n", dentroOk, dentro, fundoOk,
                fundo);
    CHECK(dentro >= 4 && dentroOk >= dentro * 0.8);
    CHECK(fundo >= 50 && fundoOk >= fundo * 0.9);

    // Frame gerado em alfa = 0.5 contra o ideal (cena em t = 1.5 com a pose do último frame).
    std::vector<uint8_t> g(f1.size());
    CHECK(ref::Gerar(o, 0.5f, g.data(), W * 4));
    auto ideal = cena::Renderiza(c, 1.5, p1, kProj, W, H);
    auto noQuadrado = [&](int x, int y) {
        double d[3], az, el;
        cena::Direcao(p1, kProj, W, H, x + 0.5, y + 0.5, d);
        cena::Angulos(d, az, el);
        return c.NoQuadrado(az, el, 1, 0.08) && c.NoQuadrado(az, el, 1.5, 0.08);
    };
    auto noFundo = [&](int x, int y) {
        double d[3], az, el;
        cena::Direcao(p1, kProj, W, H, x + 0.5, y + 0.5, d);
        cena::Angulos(d, az, el);
        return !c.NoQuadrado(az, el, 1, -0.25) && !c.NoQuadrado(az, el, 1.5, -0.25);
    };
    double eG = Erro(g, ideal, noQuadrado), eU = Erro(f1, ideal, noQuadrado);
    std::printf("   erro no quadrado: gerado %.2f, último frame %.2f (níveis de 0..255)\n", eG, eU);
    CHECK(eG < 0.5 * eU);
    double eFundo = Erro(g, f1, noFundo);
    std::printf("   fundo gerado vs último frame: %.3f\n", eFundo);
    CHECK(eFundo < 0.5);
}

void TesteBorda() {
    std::printf("\n== quadrado saindo pela borda direita ==\n");
    cena::Cena c;
    c.az0 = 0.55;
    c.vel = 0.04;
    vr::HmdMatrix34_t p = cena::Pose(cena::Graus(-1), 0, 0);
    auto f0 = cena::Renderiza(c, 0, p, kProj, W, H), f1 = cena::Renderiza(c, 1, p, kProj, W, H);
    ref::Olho o = Analisa(f0, p, f1, p);
    Campo campo = CampoDe(o.campo);
    bool limitado = true;
    for (int by = 0; by < campo.bh; ++by)
        for (int bx = 0; bx < campo.bw; ++bx)
            limitado = limitado && std::fabs(campo.x(bx, by)) <= ref::kBusca && std::fabs(campo.y(bx, by)) <= ref::kBusca;
    CHECK(limitado);

    std::vector<uint8_t> g(f1.size());
    CHECK(ref::Gerar(o, 0.5f, g.data(), W * 4));
    auto ideal = cena::Renderiza(c, 1.5, p, kProj, W, H);
    auto noQuadrado = [&](int x, int y) {
        double d[3], az, el;
        cena::Direcao(p, kProj, W, H, x + 0.5, y + 0.5, d);
        cena::Angulos(d, az, el);
        return c.NoQuadrado(az, el, 1, 0.08) && c.NoQuadrado(az, el, 1.5, 0.08);
    };
    double eG = Erro(g, ideal, noQuadrado), eU = Erro(f1, ideal, noQuadrado);
    std::printf("   erro na parte visível do quadrado: gerado %.2f, último frame %.2f\n", eG, eU);
    CHECK(eG < 0.6 * eU);
    // Última coluna: amostra para dentro da imagem, nunca fora dela (sem lixo).
    double eBorda = Erro(g, ideal, [&](int x, int y) { return x == W - 1 && noQuadrado(x - 8, y); });
    std::printf("   erro na última coluna dentro do quadrado: %.2f\n", eBorda);
    CHECK(eBorda >= 0 && eBorda < eU + 10);
}

void TesteEntradasRuins() {
    std::printf("\n== entradas ruins ==\n");
    cena::Cena c;
    vr::HmdMatrix34_t p0 = cena::Pose(0, 0, 0);
    auto f = cena::Renderiza(c, 0, p0, kProj, W, H);
    std::vector<uint8_t> g(f.size());
    ref::Olho o;
    o.proj = kProj;
    CHECK(!ref::NovoFrame(o, nullptr, W * 4, W, H, p0));
    CHECK(!ref::NovoFrame(o, f.data(), W * 4, 32, H, p0));
    CHECK(!ref::NovoFrame(o, f.data(), W, W, H, p0));
    CHECK(ref::NovoFrame(o, f.data(), W * 4, W, H, p0));
    CHECK(!ref::Gerar(o, 0.5f, g.data(), W * 4)); // só um frame
    CHECK(ref::NovoFrame(o, f.data(), W * 4, W, H, p0));
    CHECK(!ref::Gerar(o, 0.0f, g.data(), W * 4));
    CHECK(!ref::Gerar(o, -0.5f, g.data(), W * 4));
    CHECK(!ref::Gerar(o, 1.5f, g.data(), W * 4));
    CHECK(!ref::Gerar(o, std::numeric_limits<float>::quiet_NaN(), g.data(), W * 4));
    CHECK(!ref::Gerar(o, 0.5f, nullptr, W * 4));
    CHECK(ref::Gerar(o, 1.0f, g.data(), W * 4));
    // Tamanho mudou: histórico recomeça.
    CHECK(ref::NovoFrame(o, f.data(), W * 4, 256, 256, p0));
    CHECK(!ref::Pronto(o));

    // Meia volta entre os frames: nada do anterior é visível, então nenhum vetor e o gerado é o último.
    vr::HmdMatrix34_t p1 = cena::Pose(cena::kPi, 0, 0);
    auto f1 = cena::Renderiza(c, 1, p1, kProj, W, H);
    ref::Olho m = Analisa(f, p0, f1, p1);
    CHECK(FracaoParada(CampoDe(m.campo), 1e-6f) == 1.0);
    CHECK(ref::Gerar(m, 1.0f, g.data(), W * 4));
    CHECK(g == f1);
}

} // namespace

int main() {
    TesteHomografia();
    TesteRotacaoPura();
    TesteQuadradoAndando();
    TesteBorda();
    TesteEntradasRuins();
    std::printf(g_fail ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", g_fail);
    return g_fail ? 1 : 0;
}
