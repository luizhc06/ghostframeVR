// Motor do frame gen (D3D11, dentro do vrserver). Gera um frame por extrapolação a
// partir dos dois últimos frames reais do jogo:
//   1. luma reduzida dos dois frames;
//   2. o frame anterior é reprojetado pela diferença de pose (só rotação), para o
//      giro da cabeça não virar "movimento" (o timewarp do headset cuida dele);
//   3. fluxo por blocos entre os dois (movimento dos objetos);
//   4. o último frame real é deformado em `alfa` desse movimento para a frente.
// O frame gerado sai com a pose do último frame real; o headset faz o resto.
//
// Hot path: sem alocação e sem esperar a GPU. Toda falha devolve false e o proxy
// repassa o frame original.
#pragma once

#include <openvr_driver.h>

#include <cstdint>

#ifdef _WIN32
struct ID3D11Device;
struct ID3D11Texture2D;
#endif

namespace gf {

// Tangentes do frustum de um olho, como em IVRServerDriverHost::SetDisplayProjectionRaw.
struct ProjecaoOlho {
    float esq = -1, dir = 1, cima = -1, baixo = 1;
};

class FrameGen {
public:
    FrameGen();
    ~FrameGen();
    FrameGen(const FrameGen &) = delete;
    FrameGen &operator=(const FrameGen &) = delete;

    // Cria o device D3D11 próprio no adaptador padrão (warp = software, só para testes).
    // false = indisponível; o motivo já foi para o log.
    bool Iniciar(bool warp = false);
    bool Iniciado() const;

    void DefinirProjecao(int olho, const ProjecaoOlho &p);

    // Trava o syncTexture do compositor (keyed mutex, como os drivers fazem no Present)
    // enquanto as texturas do jogo são lidas. Sempre chame Destravar depois de Travar == true.
    bool Travar(vr::SharedTextureHandle_t sync);
    void Destravar();

    // Frame novo do jogo: copia a região `bounds` de cada olho para o histórico. Se o sync
    // estiver travado, NovoFrame mesmo o solta logo depois das cópias (Destravar vira no-op).
    // `pose` = mHmdPose do SubmitLayer. `ntHandle` = VRSwapTextureFlag_Shared_NTHandle.
    bool NovoFrame(const vr::SharedTextureHandle_t tex[2], const vr::VRTextureBounds_t bounds[2],
                   const vr::HmdMatrix34_t &pose, bool ntHandle);

    // Há dois frames no histórico com o mesmo tamanho: dá para gerar.
    bool Pronto() const;

    // Gera o frame em `alfa` (0..1] do intervalo entre os dois últimos frames reais e
    // escreve na região `bounds` das texturas `saida` (do swap set do proxy). Só enfileira
    // na GPU: use Concluido() antes de entregar ao driver.
    bool Gerar(float alfa, const vr::SharedTextureHandle_t saida[2], const vr::VRTextureBounds_t bounds[2],
               bool ntHandle);

    // A GPU terminou o último Gerar? Não bloqueia.
    bool Concluido();

    // Tempo de GPU do último Gerar concluído, em ms (timestamp queries; -1 sem medida).
    float MsGpu() const;

    // Esquece o histórico (ex.: resolução mudou, frame gen desligado) e solta as texturas
    // abertas pelos handles: chame também quando swap sets forem destruídos, porque um
    // handle novo pode reaproveitar o valor de um antigo.
    void Reiniciar();

    // Solta a textura aberta por esse handle (swap set destruído), sem perder o histórico.
    void Esquecer(vr::SharedTextureHandle_t h);

    struct Impl;

private:
    friend struct FrameGenTeste;
    Impl *m = nullptr;
};

#ifdef _WIN32
// Gancho só do framegen_d3d_test: o WARP não compartilha texturas entre devices, então o
// teste cria as texturas no device do motor e as põe no cache como se viessem de handles.
struct FrameGenTeste {
    static ::ID3D11Device *Device(FrameGen &fg);
    static bool Injetar(FrameGen &fg, vr::SharedTextureHandle_t h, ::ID3D11Texture2D *tex);
};
#endif

} // namespace gf
