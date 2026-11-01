#include "wrappers.h"

#include "config.h"
#include "log.h"
#include "platform.h"

#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstring>

namespace gf {

// Quantas chamadas de cada tipo logamos em detalhe antes de passar só a resumir.
static constexpr uint64_t kVerboseCalls = 30;

static const char *DeviceClassName(vr::ETrackedDeviceClass c) {
    switch (c) {
    case vr::TrackedDeviceClass_HMD: return "HMD";
    case vr::TrackedDeviceClass_Controller: return "Controller";
    case vr::TrackedDeviceClass_GenericTracker: return "GenericTracker";
    case vr::TrackedDeviceClass_TrackingReference: return "TrackingReference";
    case vr::TrackedDeviceClass_DisplayRedirect: return "DisplayRedirect";
    default: return "Invalid/Unknown";
    }
}

// ===========================================================================
void RateStats::Tick() {
    std::lock_guard<std::mutex> lock(m_mutex);
    double now = NowSeconds();
    if (m_windowStart == 0.0) {
        m_windowStart = m_last = now;
        return;
    }
    double dt = now - m_last;
    m_last = now;
    ++m_count;
    m_sumDt += dt;
    if (dt < m_minDt) m_minDt = dt;
    if (dt > m_maxDt) m_maxDt = dt;

    double window = now - m_windowStart;
    if (window >= 2.0 && m_count > 0) {
        GF_LOG("STATS %-28s %6.1f/s  dt(ms) avg=%.2f min=%.2f max=%.2f",
                m_name, m_count / window, 1000.0 * m_sumDt / m_count,
                1000.0 * m_minDt, 1000.0 * m_maxDt);
        m_windowStart = now;
        m_count = 0;
        m_sumDt = 0.0;
        m_minDt = 1e9;
        m_maxDt = 0.0;
    }
}

// ===========================================================================
// IVRVirtualDisplay
void VirtualDisplayWrapper::Present(const vr::PresentInfo_t *info, uint32_t size) {
    uint64_t n = m_calls++;
    if (n < kVerboseCalls && info) {
        GF_LOG("VirtualDisplay::Present #%llu tex=0x%llx vsync=%d frameId=%llu vsyncTime=%.6f size=%u",
                (unsigned long long)n, (unsigned long long)info->backbufferTextureHandle,
                (int)info->vsync, (unsigned long long)info->nFrameId,
                info->flVSyncTimeInSeconds, size);
    }
    m_presentRate.Tick();
    g_estatFG.presents.fetch_add(1, std::memory_order_relaxed);
    // >>> Ponto de inserção do frame gen (caminho VirtualDisplay) <<<
    m_real->Present(info, size);
}

void VirtualDisplayWrapper::WaitForPresent() { m_real->WaitForPresent(); }

bool VirtualDisplayWrapper::GetTimeSinceLastVsync(float *pfSeconds, uint64_t *pulFrameCounter) {
    return m_real->GetTimeSinceLastVsync(pfSeconds, pulFrameCounter);
}

// ===========================================================================
// IVRDriverDirectModeComponent

// Dados que o frame gen precisa e que chegam por outros caminhos (host e HMD).
namespace {
std::atomic<double> g_periodoVsync{0.0}; // média dos intervalos entre VsyncEvent
std::atomic<float> g_hzHeadset{0.0f};    // Prop_DisplayFrequency_Float (o Pico não manda VsyncEvent)
// O proxy declarou a trava por jogo num headset cujo driver não declara: trava > 0 que o
// SteamVR mandar é nossa e o driver não sabe segurar, então o proxy segura (fica até o fim).
std::atomic<bool> g_travaDoProxy{false};

double Periodo() {
    const double p = g_periodoVsync.load();
    if (p > 0) return p;
    const float hz = g_hzHeadset.load();
    return hz > 20 ? 1.0 / hz : 1.0 / 90.0;
}

// O relógio próprio só roda com os Hz do headset conhecidos e plausíveis; senão, 0 (desligado).
double PeriodoRelogio() {
    const double p = g_periodoVsync.load();
    const float hz = g_hzHeadset.load();
    const double per = p > 0 ? p : hz > 0 ? 1.0 / hz : 0;
    // O teste ponta a ponta roda o "headset" a ~33 Hz (vsync de 30 ms) e avisa por variável.
    static const bool livre = std::getenv("GF_TESTE_HZ_LIVRE") != nullptr;
    return per >= 1.0 / 144.5 && per <= 1.0 / 59.5 ? per : (livre && per > 0 ? per : 0);
}
std::mutex g_mutexProj;
ProjecaoOlho g_proj[2];
std::atomic<bool> g_temProj{false};

void DefinirProjecaoGlobal(int olho, float esq, float dir, float cima, float baixo) {
    std::lock_guard<std::mutex> lock(g_mutexProj);
    g_proj[olho] = {esq, dir, cima, baixo};
    g_temProj = true;
}

// No vrcompositor.exe não há host nem HMD para perguntar o frustum: ele sai da matriz de
// projeção que vem no SubmitLayer (mesma forma do IVRSystem::GetProjectionMatrix:
// m00 = 2/(r-l), m02 = (r+l)/(r-l), idem em y com t/b).
bool ProjecaoDaMatriz(const vr::HmdMatrix44_t &m, ProjecaoOlho &p) {
    const float sx = m.m[0][0], sy = m.m[1][1];
    if (!(std::fabs(sx) > 1e-3f && std::fabs(sy) > 1e-3f)) return false;
    p.esq = (m.m[0][2] - 1) / sx;
    p.dir = (m.m[0][2] + 1) / sx;
    p.cima = (m.m[1][2] - 1) / sy;
    p.baixo = (m.m[1][2] + 1) / sy;
    return true;
}

ProjecaoOlho Projecao(int olho) {
    std::lock_guard<std::mutex> lock(g_mutexProj);
    return g_proj[olho];
}

void Media(double &media, double amostra) {
    if (amostra <= 0 || amostra > 0.25) return; // pausas e travadas não entram
    media = media > 0 ? media * 0.9 + amostra * 0.1 : amostra;
}
} // namespace

// Um handle novo pode reaproveitar o valor de um destruído: o motor tem de esquecer a
// textura aberta por ele. Chamado depois do Destroy real, com m_mutexConjuntos.
static void Enfileirar(vr::SharedTextureHandle_t *fila, uint32_t &n, uint32_t max, bool &tudo,
                       const vr::SharedTextureHandle_t (&h)[3]) {
    for (vr::SharedTextureHandle_t x : h) {
        if (!x) continue;
        if (n < max) fila[n++] = x;
        else tudo = true;
    }
}

void DirectModeWrapper::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *d, SwapTextureSet_t *out) {
    m_real->CreateSwapTextureSet(pid, d, out);
    if (d && out) {
        GF_LOG("DirectMode::CreateSwapTextureSet pid=%u %ux%u fmt=%u samples=%u -> [0x%llx 0x%llx 0x%llx] flags=0x%x",
                pid, d->nWidth, d->nHeight, d->nFormat, d->nSampleCount,
                (unsigned long long)out->rSharedTextureHandles[0],
                (unsigned long long)out->rSharedTextureHandles[1],
                (unsigned long long)out->rSharedTextureHandles[2], out->unTextureFlags);
        std::lock_guard<std::mutex> lock(m_mutexConjuntos);
        uint32_t i = 0;
        while (i < m_nConjuntos && m_conjuntos[i].h[0]) ++i; // reaproveita vaga de um destruído
        if (i == kMaxConjuntos) { // cheio: o mais antigo sai (e o motor solta o que abriu dele)
            i = 0;
            Enfileirar(m_esquecer, m_nEsquecer, kMaxEsquecer, m_esquecerTudo, m_conjuntos[0].h);
        } else if (i == m_nConjuntos) {
            ++m_nConjuntos;
        }
        m_conjuntos[i] = {{out->rSharedTextureHandles[0], out->rSharedTextureHandles[1], out->rSharedTextureHandles[2]},
                          *d, out->unTextureFlags, pid};
    }
}

void DirectModeWrapper::DestroySwapTextureSet(vr::SharedTextureHandle_t h) {
    GF_LOG("DirectMode::DestroySwapTextureSet 0x%llx", (unsigned long long)h);
    m_real->DestroySwapTextureSet(h);
    std::lock_guard<std::mutex> lock(m_mutexConjuntos);
    bool achou = false;
    for (uint32_t i = 0; i < m_nConjuntos; ++i) {
        Conjunto &c = m_conjuntos[i];
        if (c.h[0] == h || c.h[1] == h || c.h[2] == h) {
            Enfileirar(m_esquecer, m_nEsquecer, kMaxEsquecer, m_esquecerTudo, c.h);
            c = {};
            achou = true;
        }
    }
    if (!achou) {
        const vr::SharedTextureHandle_t so[3] = {h, 0, 0};
        Enfileirar(m_esquecer, m_nEsquecer, kMaxEsquecer, m_esquecerTudo, so);
    }
}

void DirectModeWrapper::DestroyAllSwapTextureSets(uint32_t pid) {
    GF_LOG("DirectMode::DestroyAllSwapTextureSets pid=%u", pid);
    m_real->DestroyAllSwapTextureSets(pid);
    std::lock_guard<std::mutex> lock(m_mutexConjuntos);
    for (uint32_t i = 0; i < m_nConjuntos; ++i) {
        Conjunto &c = m_conjuntos[i];
        if (c.h[0] && c.pid == pid) {
            Enfileirar(m_esquecer, m_nEsquecer, kMaxEsquecer, m_esquecerTudo, c.h);
            c = {};
        }
    }
}

// Na thread do Present: solta o que foi destruído. Só zera o histórico se a textura
// da cena estava entre elas; se for de outra camada, só não reenvia mais as camadas guardadas.
void DirectModeWrapper::Esquecidos() {
    vr::SharedTextureHandle_t fila[kMaxEsquecer];
    uint32_t n;
    bool tudo;
    {
        std::lock_guard<std::mutex> lock(m_mutexConjuntos);
        n = m_nEsquecer;
        tudo = m_esquecerTudo;
        std::memcpy(fila, m_esquecer, n * sizeof(fila[0]));
        m_nEsquecer = 0;
        m_esquecerTudo = false;
    }
    if (!n && !tudo) return;
    bool historico = tudo, camada = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (MotorOk()) m_fg.Esquecer(fila[i]);
        for (uint32_t c = 0; c < m_nUltimas; ++c)
            for (uint32_t e = 0; e < 2; ++e)
                if (fila[i] == m_ultimas[c][e].hTexture) (c == 0 ? historico : camada) = true;
    }
    if (historico) {
        if (MotorOk()) m_fg.Reiniciar();
        m_pendente = false;
        m_nUltimas = 0;
    } else if (camada) {
        m_nUltimas = 1; // fica só a cena: os overlays voltam no próximo quadro real
    }
}

void DirectModeWrapper::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) {
    m_real->GetNextSwapTextureSetIndex(handles, pIndices);
}

// O compositor pode copiar o mesmo quadro para outra das 3 texturas do swap set.
bool DirectModeWrapper::MesmoConjunto(vr::SharedTextureHandle_t a, vr::SharedTextureHandle_t b) {
    Conjunto x{}, y{};
    return a == b || (AcharConjunto(a, &x) && AcharConjunto(b, &y) && x.h[0] == y.h[0]);
}

bool DirectModeWrapper::AcharConjunto(vr::SharedTextureHandle_t h, Conjunto *out) {
    std::lock_guard<std::mutex> lock(m_mutexConjuntos);
    for (uint32_t i = 0; i < m_nConjuntos; ++i) {
        const Conjunto &c = m_conjuntos[i];
        if (h && (c.h[0] == h || c.h[1] == h || c.h[2] == h)) {
            *out = c;
            return true;
        }
    }
    return false;
}

DirectModeWrapper::~DirectModeWrapper() {
    if (m_inicio.joinable()) m_inicio.join();
}

void DirectModeWrapper::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    uint64_t n = m_layers++;
    bool primeira = m_layersThisFrame++ == 0;
    if (n < kVerboseCalls) {
        for (int eye = 0; eye < 2; ++eye) {
            const auto &e = perEye[eye];
            GF_LOG("DirectMode::SubmitLayer #%llu eye=%d tex=0x%llx depth=0x%llx bounds=[%.3f %.3f %.3f %.3f] "
                    "posePred=%.4fs pos=(%.3f %.3f %.3f)",
                    (unsigned long long)n, eye, (unsigned long long)e.hTexture,
                    (unsigned long long)e.hDepthTexture, e.bounds.uMin, e.bounds.vMin,
                    e.bounds.uMax, e.bounds.vMax, e.flHmdPosePredictionTimeInSecondsFromNow,
                    e.mHmdPose.m[0][3], e.mHmdPose.m[1][3], e.mHmdPose.m[2][3]);
        }
    }
    if (primeira) m_quadroComFG = g_opcoes.framegen.load(std::memory_order_relaxed) && !m_desistiu;
    if (m_quadroComFG) {
        if (m_nCamadas < kMaxCamadas) {
            std::memcpy(&m_camadas[m_nCamadas++], &perEye, sizeof(Camada));
            return;
        }
        // Camadas demais: repassa o quadro como veio, na ordem.
        for (uint32_t i = 0; i < m_nCamadas; ++i) m_real->SubmitLayer(m_camadas[i]);
        m_nCamadas = 0;
        m_quadroComFG = false;
    }
    m_real->SubmitLayer(perEye);
}

void DirectModeWrapper::Falhou(const char *motivo) {
    g_estatFG.falhas++;
    if (m_falhasSeguidas++ == 0) GF_LOG("FRAMEGEN falhou: %s (repassando o original)", motivo);
    // Falha que se repete: desiste até o frame gen ser desligado e ligado de novo.
    if (m_falhasSeguidas >= 60 && !m_desistiu) {
        m_desistiu = true;
        g_estatFG.estado = FG_FALHOU;
        GF_LOG("FRAMEGEN desativado depois de %u falhas seguidas: %s", m_falhasSeguidas, motivo);
    }
}

bool DirectModeWrapper::Motor() {
    if (m_desistiu) return false;
    switch (m_motor.load()) {
    case 2: return true;
    case 0:
        g_estatFG.estado = FG_ESPERANDO;
        m_motor = 1;
        if (m_inicio.joinable()) m_inicio.join();
        m_inicio = std::thread([this] {
            bool ok = m_fg.Iniciar();
            GF_LOG(ok ? "FRAMEGEN motor iniciado" : "FRAMEGEN indisponível: o motor não iniciou (repassando tudo)");
            m_motor = ok ? 2 : 3;
        });
        return false;
    case 3:
        m_desistiu = true; // sem device D3D11 não há o que tentar
        g_estatFG.estado = FG_FALHOU;
        return false;
    default: return false; // iniciando: repassa até ficar pronto
    }
}

bool DirectModeWrapper::GarantirSaida(const Camada &cena) {
    Conjunto a{}, b{};
    if (!AcharConjunto(cena[0].hTexture, &a) || !AcharConjunto(cena[1].hTexture, &b)) {
        Falhou("textura da cena fora dos swap sets conhecidos");
        return false;
    }
    bool unica = a.h[0] == b.h[0];
    auto mesmo = [](const SwapTextureSetDesc_t &x, const SwapTextureSetDesc_t &y) {
        return x.nWidth == y.nWidth && x.nHeight == y.nHeight && x.nFormat == y.nFormat && x.nSampleCount == y.nSampleCount;
    };
    if (m_temSaida && m_saidaUnica == unica && mesmo(m_saida[0].desc, a.desc) && (unica || mesmo(m_saida[1].desc, b.desc)))
        return true;
    LiberarSaida();
    // O proxy pede ao próprio driver um swap set, como o compositor faz: o driver só
    // aceita no SubmitLayer texturas que ele mesmo criou.
    for (int olho = 0; olho < (unica ? 1 : 2); ++olho) {
        const Conjunto &src = olho ? b : a;
        SwapTextureSet_t out{};
        m_real->CreateSwapTextureSet(ProcessId(), &src.desc, &out);
        if (!out.rSharedTextureHandles[0] || !out.rSharedTextureHandles[1] || !out.rSharedTextureHandles[2]) {
            LiberarSaida();
            Falhou("o driver não criou o swap set do frame gen");
            return false;
        }
        m_saida[olho] = {{out.rSharedTextureHandles[0], out.rSharedTextureHandles[1], out.rSharedTextureHandles[2]},
                         src.desc, out.unTextureFlags, ProcessId()};
        m_temSaida = true;
    }
    m_saidaUnica = unica;
    m_saidaNt = (m_saida[0].flags & vr::VRSwapTextureFlag_Shared_NTHandle) != 0;
    m_idxSaida = 2;
    m_proximoIdx = -1;
    GF_LOG("FRAMEGEN swap set de saída %ux%u fmt=%u (%s)", a.desc.nWidth, a.desc.nHeight, a.desc.nFormat,
           unica ? "um para os dois olhos" : "um por olho");
    return true;
}

void DirectModeWrapper::LiberarSaida() {
    if (m_temSaida && MotorOk()) {
        for (const Conjunto &c : m_saida)
            for (vr::SharedTextureHandle_t h : c.h) m_fg.Esquecer(h);
    }
    if (m_temSaida) {
        m_real->DestroySwapTextureSet(m_saida[0].h[0]);
        if (!m_saidaUnica && m_saida[1].h[0]) m_real->DestroySwapTextureSet(m_saida[1].h[0]);
    }
    m_saida[0] = m_saida[1] = {};
    m_temSaida = false;
    m_pendente = false;
}

// Enfileira na GPU o k-ésimo frame gerado depois do último real (alfa = k / divisor),
// para ele já estar pronto quando chegar a vez dele: o Present nunca espera a GPU.
bool DirectModeWrapper::GerarAdiante(uint32_t k) {
    if (m_desistiu || !MotorOk() || !m_fg.Pronto() || m_nUltimas == 0) return false;
    const double alfa = k * m_alfaPasso;
    if (alfa >= 0.999) return false; // nunca além de um intervalo do jogo
    if (!GarantirSaida(m_ultimas[0])) return false;
    // O índice que o driver pediu; sem ele, nunca a textura entregue por último.
    uint32_t idx = m_proximoIdx >= 0 && m_proximoIdx < 3 ? uint32_t(m_proximoIdx) : (m_idxSaida + 1) % 3;
    if (idx == m_idxSaida) idx = (idx + 1) % 3;
    vr::SharedTextureHandle_t saida[2] = {m_saida[0].h[idx], (m_saidaUnica ? m_saida[0] : m_saida[1]).h[idx]};
    vr::VRTextureBounds_t bounds[2] = {m_ultimas[0][0].bounds, m_ultimas[0][1].bounds};
    if (!m_fg.Gerar(float(alfa), saida, bounds, m_saidaNt)) {
        Falhou("Gerar");
        return false;
    }
    m_pendente = true;
    m_idxPendente = idx;
    m_kPendente = k;
    m_divisorPendente = m_divisor;
    m_passoPendente = m_alfaPasso;
    m_falhasSeguidas = 0;
    g_estatFG.estado = FG_PRONTO;
    return true;
}

// Reenvia camadas (o real, quando o gerado não ficou pronto, ou o gerado) com a predição
// da pose corrigida pelo tempo que já passou desde o quadro real.
void DirectModeWrapper::Reenviar(const Camada *camadas, uint32_t n, double atraso) {
    const double a = NowSeconds();
    for (uint32_t i = 0; i < n; ++i) {
        Camada c;
        std::memcpy(&c, &camadas[i], sizeof(Camada));
        for (auto &olho : c) {
            float p = olho.flHmdPosePredictionTimeInSecondsFromNow - float(atraso);
            olho.flHmdPosePredictionTimeInSecondsFromNow = p > 0 ? p : 0;
        }
        m_real->SubmitLayer(c);
    }
    m_real->Present(m_sync);
    const double d = NowSeconds() - a;
    if (d >= 0 && d < 0.1) m_dPresent = d > m_dPresent * 0.95 ? d : m_dPresent * 0.95;
}

// Entrega o frame gerado pendente no lugar da camada da cena (as outras camadas,
// como overlays, vão como estão). false = ainda não está pronto: o chamador segue
// com o original.
bool DirectModeWrapper::EntregarGerado(const Camada *camadas, uint32_t n) {
    if (!m_pendente || n == 0 || n > kMaxCamadas || !m_fg.Concluido()) return false;
    m_pendente = false;
    Camada todas[kMaxCamadas];
    std::memcpy(todas, camadas, n * sizeof(Camada));
    todas[0][0].hTexture = m_saida[0].h[m_idxPendente];
    todas[0][1].hTexture = (m_saidaUnica ? m_saida[0] : m_saida[1]).h[m_idxPendente];
    todas[0][0].hDepthTexture = todas[0][1].hDepthTexture = 0; // o depth do real não vale para o gerado
    Reenviar(todas, n, NowSeconds() - m_tUltimoPresent);
    // Como o compositor faz depois de cada Present: o driver diz qual índice usar agora.
    vr::SharedTextureHandle_t hs[2] = {m_saida[0].h[0], (m_saidaUnica ? m_saida[0] : m_saida[1]).h[0]};
    uint32_t idx[2] = {3, 3};
    m_real->GetNextSwapTextureSetIndex(hs, &idx);
    m_proximoIdx = idx[0] < 3 ? int32_t(idx[0]) : -1;
    m_idxSaida = m_idxPendente;
    m_falhasSeguidas = 0;
    m_semEntrega = 0;
    g_estatFG.gerados++;
    g_estatFG.msGpu = m_fg.MsGpu();
    return true;
}

void DirectModeWrapper::Present(vr::SharedTextureHandle_t syncTexture) {
    uint64_t n = m_presents++;
    uint32_t layers = m_layersThisFrame.exchange(0);
    if (n < kVerboseCalls)
        GF_LOG("DirectMode::Present #%llu sync=0x%llx layers=%u", (unsigned long long)n,
                (unsigned long long)syncTexture, layers);
    m_presentRate.Tick();
    g_estatFG.presents.fetch_add(1, std::memory_order_relaxed);
    m_tPresent = NowSeconds();

    bool fg = m_quadroComFG;
    m_quadroComFG = false;
    if (!fg) {
        // Frame gen desligado: repassa como sempre (e solta o que tinha alocado).
        m_ultimoFoiNovo = false;
        if (!g_opcoes.framegen.load(std::memory_order_relaxed) && (m_temSaida || m_nUltimas || m_desistiu)) {
            if (m_temSaida) LiberarSaida();
            if (MotorOk()) m_fg.Reiniciar();
            if (m_motor.load() == 3) m_motor = 0; // religar tenta iniciar de novo
            m_desistiu = false;
            m_falhasSeguidas = m_semEntrega = 0;
            m_nUltimas = 0;
            // Religar mede tudo de novo: médias de outra hora enganariam o relógio próprio.
            m_intervaloJogo = m_intervaloPresent = m_tUltimoReal = m_tUltimoPresent = 0;
            m_credito = m_ultimoIntervalo = m_tSaida = 0;
            m_folga = 0.01;
            g_estatFG.estado = FG_DESLIGADO;
        }
        m_real->Present(syncTexture);
        return;
    }

    // >>> frame gen
    Esquecidos();
    double agora = NowSeconds();
    if (m_tUltimoPresent > 0) Media(m_intervaloPresent, agora - m_tUltimoPresent);
    m_tUltimoPresent = agora;
    m_sync = syncTexture;
    uint32_t nc = m_nCamadas;
    m_nCamadas = 0;
    const Camada &cena = m_camadas[0];
    // Quadro repetido = mesma pose de render no mesmo swap set (a textura exata não serve:
    // o compositor pode copiar o quadro para outra textura do set a cada vsync).
    // ponytail: com o tracking congelado (pose idêntica bit a bit) um quadro novo do jogo
    // passa por repetido; só os divisor-1 primeiros viram gerados, depois o original volta.
    bool novo = nc > 0 && (m_nUltimas == 0 ||
                           std::memcmp(&cena[0].mHmdPose, &m_ultimas[0][0].mHmdPose, sizeof(vr::HmdMatrix34_t)) != 0 ||
                           !MesmoConjunto(cena[0].hTexture, m_ultimas[0][0].hTexture));

    // Quadro repetido pelo compositor (o jogo atrasou): troca pelo gerado, se estiver pronto.
    if (nc > 0 && !novo) g_estatFG.repetidos.fetch_add(1, std::memory_order_relaxed);
    if (nc > 0 && !novo && EntregarGerado(m_camadas, nc)) {
        m_repeticoes++;
        GerarAdiante(m_repeticoes + 1);
        m_ultimoFoiNovo = false;
        return;
    }

    if (novo) {
        g_estatFG.reais++;
        m_ultimoIntervalo = m_tUltimoReal > 0 && agora - m_tUltimoReal < 0.25 ? agora - m_tUltimoReal : 0;
        if (m_tUltimoReal > 0) Media(m_intervaloJogo, agora - m_tUltimoReal);
        m_tUltimoReal = agora;
        m_repeticoes = 0;
        m_pendente = false; // o gerado do frame anterior ficou velho
        std::memcpy(m_ultimas, m_camadas, nc * sizeof(Camada));
        m_nUltimas = nc;
        // Sem trava, o divisor sai da razão entre os intervalos do jogo e do compositor.
        if (g_estatFG.trava.load() == 0 && m_intervaloPresent > 0 && m_intervaloJogo > 0) {
            double r = m_intervaloJogo / m_intervaloPresent;
            m_divisor = r < 2.5 ? 2 : 3;
        }
        const int mult = g_opcoes.multiplicador.load(std::memory_order_relaxed);
        if (mult >= 2 && g_estatFG.trava.load() == 0) m_divisor = uint32_t(mult); // manual sem trava do SteamVR
        m_alfaPasso = 1.0 / m_divisor;
        // Relógio próprio: sem trava e sem repetição (Present no ritmo do jogo), com o jogo
        // abaixo dos Hz do headset. Cada quadro real soma I/período - 1 de crédito; a parte
        // inteira vira gerados (máx. 2). Se o compositor ficou esperando pelo proxy (folga ~0
        // entre sair do PostPresent e o quadro novo), o relógio pausa 5 s: segurar o
        // compositor não pode derrubar o fps do próprio jogo.
        if (m_tSaida > 0) {
            const double f = agora - m_tSaida;
            m_folga = m_folga * 0.8 + (f < 0 ? 0 : f > 0.1 ? 0.1 : f) * 0.2;
            if (m_folga < 0.001 && agora >= m_relogioPausaAte) {
                m_relogioPausaAte = agora + 5.0;
                m_folga = 0.01;
                GF_LOG("FRAMEGEN relógio próprio pausado 5 s: o compositor estava esperando pelo proxy");
            }
            m_tSaida = 0;
        }
        const double per = PeriodoRelogio();
        m_extrasRelogio = 0;
        if (per > 0 && mult < 2 && g_estatFG.trava.load() == 0 && agora >= m_relogioPausaAte && m_intervaloJogo > 1.05 * per &&
            m_intervaloPresent > 0.8 * m_intervaloJogo) {
            double c = m_credito + m_intervaloJogo / per - 1.0;
            c = c < 0 ? 0 : c > 2 ? 2 : c;
            m_extrasRelogio = c >= 2 ? 2 : c >= 1 ? 1 : 0;
            m_credito = c - m_extrasRelogio;
            if (m_extrasRelogio) m_alfaPasso = per / m_intervaloJogo;
        } else {
            m_credito = 0;
        }
        if (Motor()) {
            for (int olho = 0; olho < 2; ++olho) {
                ProjecaoOlho p = Projecao(olho);
                if (!g_temProj && ProjecaoDaMatriz(cena[olho].mProjection, p) && g_estatFG.reais.load() <= 1)
                    GF_LOG("FRAMEGEN projeção olho %d pela matriz: l=%.3f r=%.3f t=%.3f b=%.3f", olho, p.esq, p.dir, p.cima, p.baixo);
                m_fg.DefinirProjecao(olho, p);
            }
            Conjunto c{};
            bool nt = AcharConjunto(cena[0].hTexture, &c) && (c.flags & vr::VRSwapTextureFlag_Shared_NTHandle);
            vr::SharedTextureHandle_t tex[2] = {cena[0].hTexture, cena[1].hTexture};
            vr::VRTextureBounds_t bounds[2] = {cena[0].bounds, cena[1].bounds};
            // O syncTexture serializa o acesso às texturas com o compositor (keyed mutex).
            bool travou = !syncTexture || m_fg.Travar(syncTexture);
            bool ok = travou && m_fg.NovoFrame(tex, bounds, cena[0].mHmdPose, nt);
            if (travou && syncTexture) m_fg.Destravar();
            if (!ok) {
                Falhou(travou ? "NovoFrame" : "não travou o syncTexture");
                m_nUltimas = 0; // não extrapola a partir de um histórico velho
            } else if (!m_fg.Pronto()) {
                g_estatFG.estado = FG_ESPERANDO;
            }
        }
    }

    for (uint32_t i = 0; i < nc; ++i) m_real->SubmitLayer(m_camadas[i]);
    m_real->Present(syncTexture);
    m_ultimoFoiNovo = novo;
    if (novo && m_nUltimas) GerarAdiante(1);
}

void DirectModeWrapper::PostPresent(const Throttling_t *t) {
    static std::atomic<uint64_t> chamadas{0};
    uint32_t trava = t ? t->nFramesToThrottle : 0;
    if (chamadas++ < kVerboseCalls)
        GF_LOG("DirectMode::PostPresent throttle=%u predict=%u", trava, t ? t->nAdditionalFramesToPredict : 0);
    const bool novo = m_ultimoFoiNovo;
    m_ultimoFoiNovo = false;
    // Manual 2x/3x sem trava do SteamVR (caso do Pico Connect): o proxy trava o jogo ele
    // mesmo, segurando o compositor os vsyncs da fração e gerando em cada um. Só com o motor
    // pronto: sem gerado, travar o jogo só tiraria fps.
    const int mult = g_opcoes.multiplicador.load(std::memory_order_relaxed);
    const bool travaPropria = trava == 0 && mult >= 2 && novo && !m_desistiu && MotorOk() && m_fg.Pronto() && m_nUltimas;
    if (travaPropria) trava = uint32_t(mult - 1);
    g_estatFG.trava = trava;
    const bool nossa = travaPropria || g_travaDoProxy.load(std::memory_order_relaxed);
    // Sem trava: só o relógio próprio entrega algo aqui (gerado pronto depois de um quadro novo).
    const bool relogio = trava == 0 && novo && !m_desistiu && m_extrasRelogio > 0 && m_pendente && m_nUltimas;
    if ((trava == 0 && !relogio) || (trava > 0 && !nossa && (!novo || m_desistiu))) {
        PostPresentReal(t);
        return;
    }
    // Trava do SteamVR (jogo a 1/(N+1) da taxa): em vez de o driver esperar N vsyncs de
    // uma vez, espera um por vez e entrega um frame em cada um. O protocolo fica sempre
    // Present -> PostPresent: se o gerado não estiver pronto, vai o real de novo.
    const Throttling_t um{0, t ? t->nAdditionalFramesToPredict : 0}; // no relógio próprio `t` pode ser nulo
    const double periodo = Periodo(), t0 = m_tPresent;
    // Trava nossa sem frame para entregar: segura os N+1 vsyncs como o ALVR (o driver não
    // segura e o jogo está sendo previsto N quadros à frente).
    auto segurar = [&] { if (trava) DormirAte(t0 + (trava + 1) * periodo - 0.001); }; // relógio: nada a segurar
    const uint32_t extras = relogio ? m_extrasRelogio : trava < 3 ? trava : 3;
    if (!relogio && novo && !m_desistiu) {
        m_divisor = extras + 1;
        m_alfaPasso = 1.0 / m_divisor;
        // O gerado pendente foi feito com outro passo (ex.: vinha do relógio): refaz.
        if (m_pendente && std::fabs(m_passoPendente - m_alfaPasso) > 1e-6) {
            m_pendente = false;
            GerarAdiante(1); // dá tempo: a GPU tem um vsync inteiro
        }
    }
    // Relógio: o próximo quadro real esperado (o mais cedo entre a média e o último par).
    const double proxReal = t0 + (m_ultimoIntervalo > 0 && m_ultimoIntervalo < m_intervaloJogo ? m_ultimoIntervalo : m_intervaloJogo);
    if (!novo || m_desistiu || !m_pendente || m_nUltimas == 0) { // desligado, aquecendo ou repetido
        if (!nossa) return PostPresentReal(t);
        PostPresentReal(&um);
        segurar();
        return;
    }
    for (uint32_t k = 1; k <= extras; ++k) {
        PostPresentReal(&um);           // o driver espera o vsync do quadro anterior
        if (relogio) {
            // Gerado atrasado é pior que nenhum (o Pico já reprojeta), e nunca segurar o
            // compositor até depois do próximo quadro real (contando o PostPresent do driver).
            const double agora = NowSeconds(), prazo = t0 + k * periodo;
            // Quando sairíamos: o Present do gerado mais o PostPresent do driver (medidos), + 1 ms.
            const double fim = (agora > prazo ? agora : prazo) + m_dPresent + m_dPost;
            if (agora > prazo - 0.0005 || fim + 0.001 > proxReal) {
                m_tSaida = NowSeconds();
                return;
            }
        }
        DormirAte(t0 + k * periodo - 0.001); // e o ritmo se mantém se ele voltar cedo
        Esquecidos(); // um swap set pode ter sido destruído enquanto esperava
        if (m_nUltimas == 0) return segurar(); // não reenvia handle morto
        // O Present extra usa o sync do compositor: se ele não estiver livre, parar aqui
        // é melhor que arriscar o driver travado esperando por ele.
        if (!m_fg.Travar(m_sync)) {
            Falhou("syncTexture ocupado no PostPresent");
            return segurar();
        }
        m_fg.Destravar();
        if (EntregarGerado(m_ultimas, m_nUltimas)) {
            if (k < extras) GerarAdiante(k + 1);
            continue;
        }
        // Não ficou pronto. No relógio o driver já cobre o vsync sozinho: para aqui.
        if (relogio) {
            m_tSaida = NowSeconds();
            return;
        }
        // Com trava: entrega o real de novo (o mesmo que o headset mostraria).
        Reenviar(m_ultimas, m_nUltimas, NowSeconds() - t0);
        g_estatFG.falhas++;
        if (++m_semEntrega >= 120 && !m_desistiu) {
            m_desistiu = true;
            g_estatFG.estado = FG_FALHOU;
            GF_LOG("FRAMEGEN desativado: os frames gerados não ficam prontos a tempo (GPU ocupada?)");
        }
    }
    PostPresentReal(&um);
    // O driver recebeu trava 0: quem fecha a fração (o último vsync) é o proxy.
    segurar();
    if (relogio) m_tSaida = NowSeconds();
}

// PostPresent do driver, medindo quanto ele demora (o relógio próprio conta com isso).
void DirectModeWrapper::PostPresentReal(const Throttling_t *t) {
    const double a = NowSeconds();
    m_real->PostPresent(t);
    const double d = NowSeconds() - a;
    if (d >= 0 && d < 0.1) {
        m_dPost = d > m_dPost * 0.95 ? d : m_dPost * 0.95;
        m_dPostMedia = m_dPostMedia > 0 ? m_dPostMedia * 0.9 + d * 0.1 : d;
    }
    g_estatFG.msPostPresent.store(float(m_dPostMedia * 1000.0), std::memory_order_relaxed);
}

void DirectModeWrapper::GetFrameTiming(vr::DriverDirectMode_FrameTiming *ft) {
    uint32_t flagsIn = ft ? ft->m_nReprojectionFlags : 0;
    m_real->GetFrameTiming(ft);
    static std::atomic<uint64_t> calls{0};
    uint64_t n = calls++;
    if (ft && (n < kVerboseCalls || n % 600 == 0)) {
        GF_LOG("DirectMode::GetFrameTiming #%llu flagsIn=0x%x(motionSmoothing=%s) -> presents=%u misPresented=%u dropped=%u flagsOut=0x%x",
                (unsigned long long)n, flagsIn,
                (flagsIn & (vr::VRCompositor_ReprojectionMotion_Enabled | vr::VRCompositor_ReprojectionMotion_ForcedOn)) ? "on" : "off",
                ft->m_nNumFramePresents, ft->m_nNumMisPresented, ft->m_nNumDroppedFrames,
                ft->m_nReprojectionFlags);
    }
}

// ===========================================================================
// DirectMode 008 <-> 009. Os structs iguais nas duas versões vão byte a byte; o
// SubmitLayerPerEye_t da 008 é o começo do da 009.
namespace {
using Dm9 = vr::IVRDriverDirectModeComponent;
using Dm8 = vr::legado::IVRDriverDirectModeComponent;
static_assert(sizeof(Dm8::SwapTextureSetDesc_t) == sizeof(Dm9::SwapTextureSetDesc_t), "008 != 009");
static_assert(sizeof(Dm8::SwapTextureSet_t) == sizeof(Dm9::SwapTextureSet_t), "008 != 009");
static_assert(sizeof(Dm8::Throttling_t) == sizeof(Dm9::Throttling_t), "008 != 009");
static_assert(offsetof(Dm9::SubmitLayerPerEye_t, flHmdPosePredictionTimeInSecondsFromNow) ==
                  sizeof(Dm8::SubmitLayerPerEye_t), "a 008 tem de ser o começo da 009");

template <class A, class B> A Copia(const B &b) {
    static_assert(sizeof(A) == sizeof(B), "tamanhos diferentes");
    A a;
    std::memcpy(&a, &b, sizeof(A));
    return a;
}
} // namespace

void Driver008::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *d, SwapTextureSet_t *out) {
    Dm8::SwapTextureSetDesc_t d8{};
    Dm8::SwapTextureSet_t o8{};
    if (d) d8 = Copia<Dm8::SwapTextureSetDesc_t>(*d);
    if (out) o8 = Copia<Dm8::SwapTextureSet_t>(*out);
    m_real->CreateSwapTextureSet(pid, d ? &d8 : nullptr, out ? &o8 : nullptr);
    if (out) *out = Copia<SwapTextureSet_t>(o8);
}

void Driver008::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    Dm8::SubmitLayerPerEye_t c[2];
    for (int i = 0; i < 2; ++i) std::memcpy(&c[i], &perEye[i], sizeof(c[i]));
    m_real->SubmitLayer(c);
}

void Driver008::PostPresent(const Throttling_t *t) {
    if (!t) return m_real->PostPresent(nullptr);
    const Dm8::Throttling_t t8 = Copia<Dm8::Throttling_t>(*t);
    m_real->PostPresent(&t8);
}

void Frente008::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *d, SwapTextureSet_t *out) {
    Dm9::SwapTextureSetDesc_t d9{};
    Dm9::SwapTextureSet_t o9{};
    if (d) d9 = Copia<Dm9::SwapTextureSetDesc_t>(*d);
    if (out) o9 = Copia<Dm9::SwapTextureSet_t>(*out);
    m_dm->CreateSwapTextureSet(pid, d ? &d9 : nullptr, out ? &o9 : nullptr);
    if (out) *out = Copia<SwapTextureSet_t>(o9);
}

void Frente008::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    Dm9::SubmitLayerPerEye_t c[2] = {}; // predição da pose = 0: a 008 não manda
    for (int i = 0; i < 2; ++i) std::memcpy(&c[i], &perEye[i], sizeof(perEye[i]));
    m_dm->SubmitLayer(c);
}

void Frente008::PostPresent(const Throttling_t *t) {
    if (!t) return m_dm->PostPresent(nullptr);
    const Dm9::Throttling_t t9 = Copia<Dm9::Throttling_t>(*t);
    m_dm->PostPresent(&t9);
}

// ===========================================================================
// ITrackedDeviceServerDriver

// Propriedades do SteamVR (do contexto real, pedidas pelo próprio proxy no Init).
// (LerHzDoHeadset e TravaPorJogo estão logo abaixo, com o estado do HMD.)
static std::atomic<vr::IVRProperties *> g_props{nullptr};
// Container do HMD e o valor que o driver tinha, para devolver ao desligar o frame gen.
static std::mutex g_mutexTrava;
static vr::PropertyContainerHandle_t g_hmdContainer = vr::k_ulInvalidPropertyContainer;
static bool g_travaOriginal = false, g_travaNossa = false;

// Frame gen ligado: o headset passa a aceitar a trava por jogo do SteamVR (como o Lossless,
// o jogo roda numa fração e o proxy gera o resto). Sem esta propriedade o SteamVR nunca
// manda nFramesToThrottle ao PostPresent; quem segura o vsync é o proxy. Desligado, volta
// o valor do driver. Chamada no Activate do HMD e quando o ghostframe.ini muda.
void TravaPorJogo(bool ligado) {
    vr::IVRProperties *props = g_props.load();
    std::lock_guard<std::mutex> lock(g_mutexTrava);
    if (!props || g_hmdContainer == vr::k_ulInvalidPropertyContainer || ligado == g_travaNossa) return;
    vr::CVRPropertyHelpers h(props);
    const bool valor = ligado || g_travaOriginal;
    const vr::ETrackedPropertyError e = h.SetBoolProperty(g_hmdContainer, vr::Prop_Hmd_SupportsAppThrottling_Bool, valor);
    g_travaNossa = ligado;
    if (ligado && !g_travaOriginal) g_travaDoProxy = true;
    GF_LOG("FRAMEGEN trava por jogo no headset: SupportsAppThrottling=%d (do driver: %d, erro=%d)",
           (int)valor, (int)g_travaOriginal, (int)e);
}

void LerHzDoHeadset() {
    vr::IVRProperties *props = g_props.load();
    std::lock_guard<std::mutex> lock(g_mutexTrava);
    if (!props || g_hmdContainer == vr::k_ulInvalidPropertyContainer) return;
    const float hz = vr::CVRPropertyHelpers(props).GetFloatProperty(g_hmdContainer, vr::Prop_DisplayFrequency_Float);
    if (hz != g_hzHeadset.exchange(hz)) GF_LOG("Headset a %.1f Hz", hz);
}

DeviceWrapper::DeviceWrapper(vr::ITrackedDeviceServerDriver *real, const char *serial, vr::ETrackedDeviceClass cls)
    : m_real(real), m_class(cls) {
    GF_LOG("DeviceWrapper criado: serial=%s class=%s", serial ? serial : "(null)", DeviceClassName(cls));
}

vr::EVRInitError DeviceWrapper::Activate(uint32_t id) {
    vr::EVRInitError r = m_real->Activate(id);
    GF_LOG("Device(%s)::Activate id=%u -> %d", DeviceClassName(m_class), id, (int)r);
    vr::IVRProperties *props = g_props.load();
    if (r == vr::VRInitError_None && m_class == vr::TrackedDeviceClass_HMD && props) {
        {
            std::lock_guard<std::mutex> lock(g_mutexTrava);
            g_hmdContainer = props->TrackedDeviceToPropertyContainer(id);
            g_travaOriginal = vr::CVRPropertyHelpers(props).GetBoolProperty(g_hmdContainer, vr::Prop_Hmd_SupportsAppThrottling_Bool);
            g_travaNossa = false;
        }
        TravaPorJogo(g_opcoes.framegen.load());
    }
    return r;
}

void DeviceWrapper::Deactivate() { m_real->Deactivate(); }
void DeviceWrapper::EnterStandby() { m_real->EnterStandby(); }

void *DeviceWrapper::GetComponent(const char *name) {
    void *comp = m_real->GetComponent(name);
    GF_LOG("Device(%s)::GetComponent(\"%s\") -> %s", DeviceClassName(m_class),
            name ? name : "(null)", comp ? "IMPLEMENTADO" : "null");
    if (!comp || !name) return comp;

    const bool display3 = std::strcmp(name, vr::IVRDisplayComponent_Version) == 0;
    if (display3 || std::strcmp(name, vr::legado::IVRDisplayComponent_Version) == 0) {
        // Só leitura: o frame gen precisa do frustum de cada olho.
        for (int olho = 0; olho < 2; ++olho) {
            float l = -1, r = 1, t = -1, b = 1;
            const vr::EVREye eye = olho ? vr::Eye_Right : vr::Eye_Left;
            if (display3) static_cast<vr::IVRDisplayComponent *>(comp)->GetProjectionRaw(eye, &l, &r, &t, &b);
            else static_cast<vr::legado::IVRDisplayComponent *>(comp)->GetProjectionRaw(eye, &l, &r, &t, &b);
            DefinirProjecaoGlobal(olho, l, r, t, b);
            GF_LOG("  projeção olho %d: l=%.3f r=%.3f t=%.3f b=%.3f", olho, l, r, t, b);
        }
    }

    return m_comp.Embrulhar(DeviceClassName(m_class), name, comp);
}

void *Componentes::Embrulhar(const char *quem, const char *name, void *comp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::strcmp(name, vr::IVRVirtualDisplay_Version) == 0) {
        if (!m_virtualDisplay)
            m_virtualDisplay = std::make_unique<VirtualDisplayWrapper>(static_cast<vr::IVRVirtualDisplay *>(comp));
        GF_LOG("  -> embrulhando IVRVirtualDisplay (%s)", quem);
        return m_virtualDisplay.get();
    }
    if (std::strcmp(name, vr::IVRDriverDirectModeComponent_Version) == 0) {
        if (!m_directMode)
            m_directMode = std::make_unique<DirectModeWrapper>(static_cast<vr::IVRDriverDirectModeComponent *>(comp));
        GF_LOG("  -> embrulhando IVRDriverDirectModeComponent (%s)", quem);
        return m_directMode.get();
    }
    if (std::strcmp(name, vr::legado::IVRDriverDirectModeComponent_Version) == 0) {
        if (!m_frente008) {
            m_driver008 = std::make_unique<Driver008>(static_cast<vr::legado::IVRDriverDirectModeComponent *>(comp));
            m_directMode008 = std::make_unique<DirectModeWrapper>(m_driver008.get());
            m_frente008 = std::make_unique<Frente008>(m_directMode008.get());
        }
        GF_LOG("  -> embrulhando IVRDriverDirectModeComponent_008 (%s)", quem);
        return m_frente008.get();
    }
    if (std::strncmp(name, "IVRVirtualDisplay_", 18) == 0 ||
        std::strncmp(name, "IVRDriverDirectModeComponent_", 29) == 0) {
        GF_LOG("  !! %s: versão que o proxy não conhece (%s / %s / %s), repassando sem embrulhar: o frame gen não pega esses frames",
                name, vr::IVRVirtualDisplay_Version, vr::IVRDriverDirectModeComponent_Version,
                vr::legado::IVRDriverDirectModeComponent_Version);
    }
    return comp;
}

// ===========================================================================
// IVRCompositorPluginProvider (roda dentro do vrcompositor.exe)
vr::EVRInitError CompositorPluginWrapper::Init(vr::IVRDriverContext *ctx) {
    IniciarFundo(); // este processo também precisa do ghostframe.ini e das estatísticas
    vr::EVRInitError r = m_real->Init(ctx);
    GF_LOG("CompositorPlugin::Init -> %d", (int)r);
    if (r != vr::VRInitError_None) PararFundo();
    return r;
}

void CompositorPluginWrapper::Cleanup() {
    GF_LOG("CompositorPlugin::Cleanup");
    PararFundo();
    m_real->Cleanup();
}

const char *const *CompositorPluginWrapper::GetInterfaceVersions() {
    const char *const *v = m_real->GetInterfaceVersions();
    static bool logado = false;
    if (!logado && v) {
        logado = true;
        for (const char *const *p = v; *p; ++p) GF_LOG("Plugin do compositor declara interface: %s", *p);
    }
    return v;
}

void *CompositorPluginWrapper::GetComponent(const char *name) {
    void *comp = m_real->GetComponent(name);
    GF_LOG("CompositorPlugin::GetComponent(\"%s\") -> %s", name ? name : "(null)", comp ? "IMPLEMENTADO" : "null");
    if (!comp || !name) return comp;
    return m_comp.Embrulhar("plugin do compositor", name, comp);
}

void DeviceWrapper::DebugRequest(const char *req, char *buf, uint32_t size) {
    m_real->DebugRequest(req, buf, size);
}

vr::DriverPose_t DeviceWrapper::GetPose() { return m_real->GetPose(); }

// ===========================================================================
// IVRServerDriverHost
bool ServerDriverHostWrapper::TrackedDeviceAdded(const char *serial, vr::ETrackedDeviceClass cls,
                                                 vr::ITrackedDeviceServerDriver *pDriver) {
    GF_LOG("Host::TrackedDeviceAdded serial=%s class=%s", serial ? serial : "(null)", DeviceClassName(cls));
    vr::ITrackedDeviceServerDriver *toRegister = pDriver;
    // O vídeo sai pelo HMD (DirectMode) ou, em drivers de display virtual, por um
    // dispositivo DisplayRedirect à parte (IVRVirtualDisplay): embrulhamos os dois.
    if (pDriver && (cls == vr::TrackedDeviceClass_HMD || cls == vr::TrackedDeviceClass_DisplayRedirect)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_devices.push_back(std::make_unique<DeviceWrapper>(pDriver, serial, cls));
        toRegister = m_devices.back().get();
    }
    return m_real->TrackedDeviceAdded(serial, cls, toRegister);
}

void ServerDriverHostWrapper::TrackedDevicePoseUpdated(uint32_t dev, const vr::DriverPose_t &pose, uint32_t size) {
    if (dev == vr::k_unTrackedDeviceIndex_Hmd) {
        m_hmdPoseRate.Tick();
        g_estatFG.poses.fetch_add(1, std::memory_order_relaxed);
    }
    m_real->TrackedDevicePoseUpdated(dev, pose, size);
}

void ServerDriverHostWrapper::VsyncEvent(double offset) {
    m_vsyncRate.Tick();
    static double ultimo = 0;
    double agora = NowSeconds();
    if (ultimo > 0) {
        double p = g_periodoVsync.load(), d = agora - ultimo;
        if (d > 0.002 && d < 0.05) g_periodoVsync.store(p > 0 ? p * 0.95 + d * 0.05 : d);
    }
    ultimo = agora;
    m_real->VsyncEvent(offset);
}

void ServerDriverHostWrapper::VendorSpecificEvent(uint32_t dev, vr::EVREventType t, const vr::VREvent_Data_t &d, double off) {
    m_real->VendorSpecificEvent(dev, t, d, off);
}

bool ServerDriverHostWrapper::IsExiting() { return m_real->IsExiting(); }

bool ServerDriverHostWrapper::PollNextEvent(vr::VREvent_t *ev, uint32_t size) {
    return m_real->PollNextEvent(ev, size);
}

void ServerDriverHostWrapper::GetRawTrackedDevicePoses(float pred, vr::TrackedDevicePose_t *arr, uint32_t count) {
    m_real->GetRawTrackedDevicePoses(pred, arr, count);
}

void ServerDriverHostWrapper::RequestRestart(const char *a, const char *b, const char *c, const char *d) {
    m_real->RequestRestart(a, b, c, d);
}

uint32_t ServerDriverHostWrapper::GetFrameTimings(vr::Compositor_FrameTiming *t, uint32_t n) {
    return m_real->GetFrameTimings(t, n);
}

void ServerDriverHostWrapper::SetDisplayEyeToHead(uint32_t dev, const vr::HmdMatrix34_t &l, const vr::HmdMatrix34_t &r) {
    m_real->SetDisplayEyeToHead(dev, l, r);
}

void ServerDriverHostWrapper::SetDisplayProjectionRaw(uint32_t dev, const vr::HmdRect2_t &l, const vr::HmdRect2_t &r) {
    if (dev == vr::k_unTrackedDeviceIndex_Hmd) {
        DefinirProjecaoGlobal(0, l.vTopLeft.v[0], l.vBottomRight.v[0], l.vTopLeft.v[1], l.vBottomRight.v[1]);
        DefinirProjecaoGlobal(1, r.vTopLeft.v[0], r.vBottomRight.v[0], r.vTopLeft.v[1], r.vBottomRight.v[1]);
        GF_LOG("Host::SetDisplayProjectionRaw l=[%.3f %.3f %.3f %.3f] r=[%.3f %.3f %.3f %.3f]",
               l.vTopLeft.v[0], l.vBottomRight.v[0], l.vTopLeft.v[1], l.vBottomRight.v[1],
               r.vTopLeft.v[0], r.vBottomRight.v[0], r.vTopLeft.v[1], r.vBottomRight.v[1]);
    }
    m_real->SetDisplayProjectionRaw(dev, l, r);
}

void ServerDriverHostWrapper::SetRecommendedRenderTargetSize(uint32_t dev, uint32_t w, uint32_t h) {
    GF_LOG("Host::SetRecommendedRenderTargetSize dev=%u %ux%u", dev, w, h);
    m_real->SetRecommendedRenderTargetSize(dev, w, h);
}

// ===========================================================================
// IVRDriverContext
void *DriverContextWrapper::GetGenericInterface(const char *version, vr::EVRInitError *peError) {
    void *iface = m_real->GetGenericInterface(version, peError);
    GF_LOG("Context::GetGenericInterface(\"%s\") -> %s", version ? version : "(null)", iface ? "ok" : "null");
    if (!iface || !version) return iface;

    if (std::strcmp(version, vr::IVRServerDriverHost_Version) == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_host)
            m_host = std::make_unique<ServerDriverHostWrapper>(static_cast<vr::IVRServerDriverHost *>(iface));
        return m_host.get();
    }
    if (std::strncmp(version, "IVRServerDriverHost_", 20) == 0) {
        GF_LOG("  !! driver pediu %s, conhecemos %s: sem interceptação de dispositivos",
                version, vr::IVRServerDriverHost_Version);
    }
    return iface;
}

vr::DriverHandle_t DriverContextWrapper::GetDriverHandle() { return m_real->GetDriverHandle(); }

// ===========================================================================
// IServerTrackedDeviceProvider
vr::EVRInitError ProviderWrapper::Init(vr::IVRDriverContext *ctx) {
    IniciarFundo(); // relê o ghostframe.ini e loga as estatísticas do frame gen
    vr::EVRInitError ep = vr::VRInitError_None;
    g_props = ctx ? static_cast<vr::IVRProperties *>(ctx->GetGenericInterface(vr::IVRProperties_Version, &ep)) : nullptr;
    m_context = std::make_unique<DriverContextWrapper>(ctx);
    vr::EVRInitError r = m_real->Init(m_context.get());
    GF_LOG("Provider::Init -> %d", (int)r);
    if (r != vr::VRInitError_None) PararFundo(); // sem Cleanup depois: a thread não pode ficar
    return r;
}

void ProviderWrapper::Cleanup() {
    GF_LOG("Provider::Cleanup");
    PararFundo();
    m_real->Cleanup();
}

const char *const *ProviderWrapper::GetInterfaceVersions() {
    const char *const *v = m_real->GetInterfaceVersions();
    static bool logged = false;
    if (!logged && v) {
        logged = true;
        for (const char *const *p = v; *p; ++p) GF_LOG("Driver da Pico declara interface: %s", *p);
    }
    return v;
}

void ProviderWrapper::RunFrame() { m_real->RunFrame(); }
bool ProviderWrapper::ShouldBlockStandbyMode() { return m_real->ShouldBlockStandbyMode(); }
void ProviderWrapper::EnterStandby() { m_real->EnterStandby(); }
void ProviderWrapper::LeaveStandby() { m_real->LeaveStandby(); }

} // namespace gf
