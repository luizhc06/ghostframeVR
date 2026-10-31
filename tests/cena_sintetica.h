// Cena sintética dos testes do frame gen: ruído suave pintado na esfera em volta da
// cabeça e um quadrado texturizado que anda em azimute. Tudo é função da direção no
// mundo, então girar a cabeça move a imagem mas não move nada no mundo.
#pragma once

#include "framegen.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace cena {

constexpr double kPi = 3.14159265358979323846;
inline double Graus(double g) { return g * kPi / 180.0; }

inline float Hash(int x, int y, uint32_t s) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return float(h & 0xffffffu) / float(0xffffffu);
}

// Value noise com interpolação suave, em [0, 1].
inline float Ruido(double x, double y, uint32_t s) {
    double fx = std::floor(x), fy = std::floor(y);
    int ix = int(fx), iy = int(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    double a = Hash(ix, iy, s) + tx * (Hash(ix + 1, iy, s) - Hash(ix, iy, s));
    double b = Hash(ix, iy + 1, s) + tx * (Hash(ix + 1, iy + 1, s) - Hash(ix, iy + 1, s));
    return float(a + ty * (b - a));
}

// Pose HMD -> mundo (convenção OpenVR: x direita, y para cima, -z para frente).
// R = Ry(yaw) · Rx(pitch) · Rz(roll); yaw positivo vira para a esquerda.
inline vr::HmdMatrix34_t Pose(double yaw, double pitch, double roll) {
    double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    double cr = std::cos(roll), sr = std::sin(roll);
    const double Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double Rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const double Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    double A[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) A[i][j] = Ry[i][0] * Rx[0][j] + Ry[i][1] * Rx[1][j] + Ry[i][2] * Rx[2][j];
    vr::HmdMatrix34_t m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[i][j] = float(A[i][0] * Rz[0][j] + A[i][1] * Rz[1][j] + A[i][2] * Rz[2][j]);
    m.m[0][3] = 0.1f; // translação não entra na compensação
    m.m[1][3] = 1.7f;
    return m;
}

// Direção no mundo vista pelo ponto (x, y) da imagem (pixels contínuos, centro em i + 0.5).
inline void Direcao(const vr::HmdMatrix34_t &pose, const gf::ProjecaoOlho &p, int w, int h, double x, double y,
                    double d[3]) {
    double tx = p.esq + x / w * (p.dir - p.esq);
    double ty = p.cima + y / h * (p.baixo - p.cima); // tangente com y para baixo
    const double hmd[3] = {tx, -ty, -1};
    double n = std::sqrt(tx * tx + ty * ty + 1);
    for (int i = 0; i < 3; ++i)
        d[i] = (pose.m[i][0] * hmd[0] + pose.m[i][1] * hmd[1] + pose.m[i][2] * hmd[2]) / n;
}

// Inverso de Direcao. false se a direção está atrás do olho.
inline bool Projeta(const vr::HmdMatrix34_t &pose, const gf::ProjecaoOlho &p, int w, int h, const double d[3],
                    double &x, double &y) {
    double hmd[3];
    for (int i = 0; i < 3; ++i) hmd[i] = pose.m[0][i] * d[0] + pose.m[1][i] * d[1] + pose.m[2][i] * d[2];
    if (hmd[2] >= 0) return false;
    double tx = hmd[0] / -hmd[2], ty = -hmd[1] / -hmd[2];
    x = (tx - p.esq) / (p.dir - p.esq) * w;
    y = (ty - p.cima) / (p.baixo - p.cima) * h;
    return true;
}

inline void Angulos(const double d[3], double &az, double &el) {
    az = std::atan2(d[0], -d[2]);
    el = std::atan2(d[1], std::hypot(d[0], d[2]));
}

inline void DeAngulos(double az, double el, double d[3]) {
    d[0] = std::cos(el) * std::sin(az);
    d[1] = std::sin(el);
    d[2] = -std::cos(el) * std::cos(az);
}

struct Cena {
    double az0 = -0.3, el0 = -0.3, lado = 0.6; // quadrado em t = 0 (rad)
    double vel = 0;                            // rad de azimute por frame
    double celulas = 20;                       // células de ruído por radiano

    bool NoQuadrado(double az, double el, double t, double margem = 0) const {
        double a = az0 + vel * t;
        return az >= a + margem && az <= a + lado - margem && el >= el0 + margem && el <= el0 + lado - margem;
    }

    // Cor RGBA8 da direção d no instante t (em frames).
    void Cor(const double d[3], double t, uint8_t px[4]) const {
        double az, el;
        Angulos(d, az, el);
        float r, g, b;
        if (NoQuadrado(az, el, t)) {
            // Textura presa ao quadrado: anda junto com ele.
            double u = (az - az0 - vel * t) * celulas, v = (el - el0) * celulas;
            float n = Ruido(u, v, 2), m = Ruido(u * 0.5, v * 0.5, 3);
            r = 0.55f + 0.45f * n;
            g = 0.35f + 0.5f * m;
            b = 0.1f + 0.3f * n;
        } else {
            float n = Ruido(az * celulas, el * celulas, 1), m = Ruido(az * celulas * 0.5, el * celulas * 0.5, 4);
            r = 0.1f + 0.5f * n;
            g = 0.15f + 0.4f * m;
            b = 0.6f - 0.4f * n;
        }
        px[0] = uint8_t(r * 255 + 0.5f);
        px[1] = uint8_t(g * 255 + 0.5f);
        px[2] = uint8_t(b * 255 + 0.5f);
        px[3] = 255;
    }
};

// Renderiza um olho w×h (RGBA8, stride em bytes).
inline void Renderiza(const Cena &c, double t, const vr::HmdMatrix34_t &pose, const gf::ProjecaoOlho &p, int w,
                      int h, uint8_t *rgba, int stride) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double d[3];
            Direcao(pose, p, w, h, x + 0.5, y + 0.5, d);
            c.Cor(d, t, rgba + size_t(y) * stride + size_t(x) * 4);
        }
}

inline std::vector<uint8_t> Renderiza(const Cena &c, double t, const vr::HmdMatrix34_t &pose,
                                      const gf::ProjecaoOlho &p, int w, int h) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    Renderiza(c, t, pose, p, w, h, img.data(), w * 4);
    return img;
}

} // namespace cena
