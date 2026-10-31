// Opções do ghostframe.ini (%LOCALAPPDATA%\ghostframeVR, gravado pelo app), relidas
// por uma thread de fundo: o hot path só lê atômicos. A mesma thread escreve no log
// as estatísticas do frame gen (o app lê essas linhas para o painel e o HUD).
#pragma once

#include <atomic>
#include <cstdint>

namespace gf {

struct Opcoes {
    std::atomic<bool> framegen{false}; // [framegen] ativo=1
};
extern Opcoes g_opcoes;

enum EstadoFG : int { FG_DESLIGADO, FG_ESPERANDO, FG_PRONTO, FG_FALHOU };

struct EstatFG {
    std::atomic<uint32_t> reais{0}, gerados{0}, falhas{0};
    std::atomic<uint32_t> trava{0};  // framesToThrottle visto no último PostPresent
    std::atomic<float> msGpu{-1.0f};
    std::atomic<int> estado{FG_DESLIGADO};
};
extern EstatFG g_estatFG;

// Lê "[secao] chave=valor" de um ini já carregado em memória. Exposto para teste.
bool IniLigado(const char *texto, const char *secao, const char *chave);

void IniciarFundo();
void PararFundo();

} // namespace gf
