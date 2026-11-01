// Opções do ghostframe.ini (%LOCALAPPDATA%\ghostframeVR, gravado pelo app), relidas
// por uma thread de fundo: o hot path só lê atômicos. A mesma thread escreve no log
// as estatísticas do frame gen (o app lê essas linhas para o painel e o HUD).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace gf {

struct Opcoes {
    std::atomic<bool> framegen{false}; // [framegen] ativo=1
    // [framegen] modo=manual + multiplicador=2|3: o proxy trava o jogo nessa fração dos Hz e
    // gera o resto (como a trava do SteamVR, que o Pico Connect não recebe). 0 = Auto.
    std::atomic<int> multiplicador{0};
};
extern Opcoes g_opcoes;

enum EstadoFG : int { FG_DESLIGADO, FG_ESPERANDO, FG_PRONTO, FG_FALHOU };

struct EstatFG {
    std::atomic<uint32_t> reais{0}, gerados{0}, falhas{0};
    std::atomic<uint32_t> repetidos{0}; // quadros que o compositor repetiu (o jogo atrasou)
    std::atomic<uint32_t> trava{0};  // framesToThrottle visto no último PostPresent
    std::atomic<float> msGpu{-1.0f};
    std::atomic<float> msPostPresent{-1.0f}; // quanto o PostPresent do driver demora (média)
    // Diagnóstico: headset mandando poses sem nenhum frame passar pelo proxy.
    std::atomic<uint64_t> presents{0}, poses{0};
    std::atomic<int> estado{FG_DESLIGADO};
};
extern EstatFG g_estatFG;

// Lê "[secao] chave=valor" de um ini já carregado em memória. Exposto para teste.
bool IniLigado(const char *texto, const char *secao, const char *chave);
// O valor de "[secao] chave=valor" ("" se não tem). Exposto para teste.
std::string IniValor(const char *texto, const char *secao, const char *chave);

// Definida em wrappers.cpp: liga/desliga a trava por jogo no headset junto com o frame gen.
void TravaPorJogo(bool ligado);
// Definida em wrappers.cpp: relê os Hz do headset (Prop_DisplayFrequency_Float), fora do hot path.
void LerHzDoHeadset();

void IniciarFundo();
void PararFundo();

} // namespace gf
