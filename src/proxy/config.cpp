#include "config.h"

#include "log.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gf {

Opcoes g_opcoes;
EstatFG g_estatFG;

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_parar = false;
std::thread g_thread;

std::string CaminhoIni() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return {};
    return std::string(buf, n) + "\\ghostframeVR\\ghostframe.ini";
#else
    // Só nos testes (o proxy de verdade roda no Windows).
    const char *p = std::getenv("GF_INI");
    return p ? p : "";
#endif
}

std::string LerArquivo(const std::string &caminho) {
    std::string r;
    FILE *f = caminho.empty() ? nullptr : std::fopen(caminho.c_str(), "rb");
    if (!f) return r;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && r.size() < 65536) r.append(buf, n);
    std::fclose(f);
    return r;
}

const char *NomeEstado(int e) {
    switch (e) {
    case FG_ESPERANDO: return "esperando";
    case FG_PRONTO: return "pronto";
    case FG_FALHOU: return "falhou";
    default: return "desligado";
    }
}

void LerOpcoes() {
    bool fg = IniLigado(LerArquivo(CaminhoIni()).c_str(), "framegen", "ativo");
    if (fg != g_opcoes.framegen.exchange(fg)) GF_LOG("FRAMEGEN %s pelo ghostframe.ini", fg ? "ligado" : "desligado");
}

void Laco() {
    uint32_t reais0 = 0, gerados0 = 0;
    for (int tick = 0;; ++tick) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_parar; })) return;
        }
        LerOpcoes();
        bool fg = g_opcoes.framegen.load();

        // Estatísticas a cada 2 s, só com algo para contar.
        if (tick % 2) continue;
        uint32_t r = g_estatFG.reais.load(), g = g_estatFG.gerados.load();
        int estado = g_estatFG.estado.load();
        if (fg || estado != FG_DESLIGADO) {
            GF_LOG("FRAMEGEN estado=%s reais=%.1f/s gerados=%.1f/s falhas=%u trava=%u gpu_ms=%.2f",
                   NomeEstado(estado), (r - reais0) / 2.0, (g - gerados0) / 2.0, g_estatFG.falhas.load(),
                   g_estatFG.trava.load(), g_estatFG.msGpu.load());
        }
        reais0 = r;
        gerados0 = g;
    }
}

} // namespace

bool IniLigado(const char *texto, const char *secao, const char *chave) {
    bool naSecao = false;
    const size_t ns = std::strlen(secao), nc = std::strlen(chave);
    for (const char *l = texto; l && *l;) {
        const char *fim = std::strchr(l, '\n');
        std::string linha(l, fim ? size_t(fim - l) : std::strlen(l));
        l = fim ? fim + 1 : nullptr;
        size_t a = linha.find_first_not_of(" \t\r"), b = linha.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        linha = linha.substr(a, b - a + 1);
        if (linha[0] == '[') {
            naSecao = linha.size() == ns + 2 && linha.compare(1, ns, secao) == 0 && linha.back() == ']';
        } else if (naSecao && linha.compare(0, nc, chave) == 0) {
            size_t eq = linha.find_first_not_of(" \t", nc);
            if (eq != std::string::npos && linha[eq] == '=') {
                size_t v = linha.find_first_not_of(" \t", eq + 1);
                return v != std::string::npos && linha[v] == '1';
            }
        }
    }
    return false;
}

void IniciarFundo() {
    LerOpcoes(); // já no Init: o primeiro quadro vê a opção certa
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_thread.joinable()) return;
    g_parar = false;
    g_thread = std::thread(Laco);
}

void PararFundo() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_parar = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

} // namespace gf
