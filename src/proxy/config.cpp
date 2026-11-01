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

// false = não deu para abrir (antivírus, arquivo sendo regravado): o chamador mantém o que tinha.
bool LerArquivo(const std::string &caminho, std::string &r) {
    r.clear();
    FILE *f = caminho.empty() ? nullptr : std::fopen(caminho.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && r.size() < 65536) r.append(buf, n);
    std::fclose(f);
    return true;
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
    // Não abriu (sem ini ou ocupado agora): mantém o que tinha; o padrão é desligado.
    std::string texto;
    if (!LerArquivo(CaminhoIni(), texto)) return;
    bool fg = IniLigado(texto.c_str(), "framegen", "ativo");
    if (fg != g_opcoes.framegen.exchange(fg)) {
        GF_LOG("FRAMEGEN %s pelo ghostframe.ini", fg ? "ligado" : "desligado");
        TravaPorJogo(fg);
    }
}

void Laco() {
    uint32_t reais0 = 0, gerados0 = 0, repetidos0 = 0;
    uint64_t presents0 = 0;
    int semGerar = 0;   // segundos com frame gen ligado, frames passando e nada gerado
    uint32_t rep10 = 0; // repetidos no começo dessa janela
    bool explicou = false;
    int semFrames = 0; // segundos com o headset ativo e nenhum Present no proxy
    bool avisou = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_parar; })) return;
        }
        LerOpcoes();
        LerHzDoHeadset();

        if (g_estatFG.presents.load() == 0 && g_estatFG.poses.load() > 0) ++semFrames;
        if (semFrames >= 15 && !avisou) {
            avisou = true;
            GF_LOG("!! headset ativo há 15 s e nenhum frame passou pelo proxy neste processo: "
                   "o driver entrega o vídeo por uma interface que o proxy não embrulha (veja as linhas !! acima)");
        }
        bool fg = g_opcoes.framegen.load();

        // Estatísticas a cada segundo (o app e o HUD mostram ao vivo), só com algo para contar.
        uint32_t r = g_estatFG.reais.load(), g = g_estatFG.gerados.load(), rep = g_estatFG.repetidos.load();
        uint64_t p = g_estatFG.presents.load();
        int estado = g_estatFG.estado.load();
        // Só o processo que vê os frames fala do frame gen.
        if ((fg && p > 0) || estado != FG_DESLIGADO) {
            GF_LOG("FRAMEGEN estado=%s reais=%.1f/s gerados=%.1f/s falhas=%u trava=%u gpu_ms=%.2f presents=%.1f/s repetidos=%.1f/s",
                   NomeEstado(estado), double(r - reais0), double(g - gerados0), g_estatFG.falhas.load(),
                   g_estatFG.trava.load(), g_estatFG.msGpu.load(), double(p - presents0), double(rep - repetidos0));
        }
        // Ligado, frames passando e nada gerado por 10 s: diz em qual etapa parou.
        semGerar = fg && p > presents0 && g == gerados0 ? semGerar + 1 : 0;
        if (semGerar == 0) explicou = false; // voltou a gerar: o próximo problema também é explicado
        if (semGerar == 1) rep10 = repetidos0;
        if (semGerar >= 10 && !explicou) {
            explicou = true;
            if (estado == FG_FALHOU)
                GF_LOG("!! FRAMEGEN parado: o motor desistiu (veja a linha \"FRAMEGEN falhou\" acima)");
            else if (rep == rep10 && g_estatFG.trava.load() == 0)
                GF_LOG("!! FRAMEGEN sem o que gerar: em 10 s o SteamVR não repetiu nenhum quadro e não pediu trava "
                       "(jogo na taxa do headset, ou o SteamVR não repete quadros para este driver)");
            else
                GF_LOG("!! FRAMEGEN com quadros para trocar e nada gerado: estado=%s falhas=%u",
                       NomeEstado(estado), g_estatFG.falhas.load());
        }
        reais0 = r;
        gerados0 = g;
        repetidos0 = rep;
        presents0 = p;
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
