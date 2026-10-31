# Prompt para o agente de engenharia reversa / implementação

Copie tudo abaixo da linha e cole no outro agente.

---

Você vai continuar o projeto **ghostframeVR**: https://github.com/luizhc06/ghostframevr

## Objetivo final

Frame generation para o **Pico 4 via Pico Connect**: o VRChat renderiza ~36 fps e o headset recebe ~72 fps estáveis, com frames sintéticos gerados por **extrapolação** (não interpolação, que adiciona latência e dá enjoo em VR).

## Contexto (leia antes de tudo)

1. Clone o repo e leia `README.md`, `docs/RE_BRIEF.md`, `CONTRIBUTING.md` e todo o `src/proxy/`.
2. Já existe (v0.1.0-alpha) um **driver proxy OpenVR**: o SteamVR carrega a nossa DLL no lugar da DLL do driver do Pico Connect; ela carrega a original (`<nome>_real.dll`) e repassa todas as chamadas, logando em `ghostframe.log`. Os pontos de inserção estão marcados com `>>>` em `src/proxy/wrappers.cpp`.
3. Máquina-alvo: Windows, Ryzen 5 5500, RTX 3060 12 GB, Pico 4 com Pico Connect via Wi-Fi. O usuário roda os testes e te devolve logs.
4. As issues #2–#7 e os milestones v0.1–v0.3 são o roadmap. Atualize as issues conforme avança.

## Regras inegociáveis

- **Nunca** injete, leia ou altere nada no processo do jogo (VRChat usa Easy Anti-Cheat). Tudo roda dentro do `vrserver.exe`/driver.
- Não commite binários, código descompilado ou trechos de assembly da Pico. Documente só **conclusões**: fluxos, estruturas, RVAs, nomes de funções.
- Só embrulhe interfaces OpenVR cuja **versão exata** está em `third_party/openvr/openvr_driver.h`. Versão diferente → passthrough + log `!!`.
- Toda funcionalidade nova fica **desligada por padrão** e é ligada por config (ex.: `ghostframe.ini` ao lado da DLL). Com tudo desligado, o proxy precisa se comportar exatamente como hoje.
- Qualquer falha (D3D, optical flow, timing) → volta para passthrough e loga. Nunca derrube o SteamVR.
- Hot path (Present/SubmitLayer) sem alocação, sem I/O síncrono e sem bloquear. Orçamento: **< 2 ms por frame gerado** na RTX 3060.
- Mantenha `ctest` passando e a CI verde. Trabalhe em branches e abra PR por fase; não faça push direto na `main`.

## Fases e critérios de pronto

### Fase 2: engenharia reversa (issue #4)

Responda às perguntas P1–P4 de `docs/RE_BRIEF.md`. O que mais importa:
- Qual componente o SteamVR usa com o Pico Connect: `IVRVirtualDisplay` ou `IVRDriverDirectModeComponent`.
- O que o driver faz no `Present`: abre a textura compartilhada, copia, qual encoder usa (NVENC?), em que thread e fila, e se limita a taxa.
- **Como cada frame é associado à pose da cabeça** (frameId, timestamp, pose embutida no pacote). O headset faz timewarp com essa pose, então o frame sintético precisa levar a pose certa.
- Se há verificação de integridade das DLLs ou um updater que restaura arquivos.

**Pronto quando:** existir `docs/RE_FINDINGS.md` com as respostas e o pseudocódigo do fluxo `Present → encode → envio`, confirmado com o `ghostframe.log` de um teste real.

### Fase 3: captura (issue #5)

- Device D3D11 próprio no proxy; abrir as texturas compartilhadas (`OpenSharedResource`/`OpenSharedResource1`, respeitando `VRSwapTextureFlag_Shared_NTHandle`).
- Identificar quando um frame é **novo do jogo** e quando é **repetido/reprojetado pelo compositor**. Dicas: `IVRServerDriverHost::GetFrameTimings` (`Compositor_FrameTiming`, que tem flags de reprojeção e contagem de presents por frame), `nFrameId`, pose em `SubmitLayer`.
- Modo gravação (flag): salvar ~5 s de cor (+ depth se houver) + pose + timestamps em disco, de forma assíncrona.

**Pronto quando:** o usuário gravar uma sessão sem queda de fps perceptível, e as gravações abrirem numa ferramenta de visualização simples (`tools/`).

### Fase 4: frame gen offline (issue #6)

- Ferramenta em `tools/` que lê as gravações e gera o frame t+1 a partir de t-1 e t:
  1. Optical flow com **NVIDIA Optical Flow SDK** (NVOFA, Ampere) entre t-1 e t.
  2. Extrapolação dos vetores para t+1.
  3. Correção pela diferença de pose da cabeça (reprojeção rotacional + translacional; use depth se existir).
  4. Preenchimento de buracos/disocclusion simples.
- Comparar o frame gerado com o t+1 real (PSNR/SSIM + imagens lado a lado).

**Pronto quando:** houver métricas e imagens em `docs/` mostrando o resultado melhor que a reprojeção só rotacional, com tempo de GPU < 2 ms em 2 × ~2000×2000.

### Fase 5: injeção ao vivo (issue #7)

- Quando o jogo perder o frame (detecção da fase 3), gerar o sintético e entregá-lo ao driver da Pico no lugar do frame repetido, com a pose certa (fase 2).
- Toggle em runtime (tecla ou arquivo de config relido) e HUD/log de frames reais vs sintéticos.
- Pacing: deixar o VRChat estável em metade da taxa (36 @ 72 Hz) e gerar exatamente um sintético entre cada par de reais.

**Pronto quando:** o usuário confirmar, no headset, 72 fps percebidos com o VRChat a ~36, sem enjoo, e o log mostrar a proporção 1:1 de real/sintético. Aí sai a release v0.3.0-alpha.

## Como trabalhar comigo (o usuário)

- Sempre que precisar de teste real, me dê **instruções numeradas e curtas**: o que instalar, o que fazer no jogo, quanto tempo, que arquivo devolver.
- Antes de cada teste, compile a DLL pela CI ou pela release e diga qual versão usar.
- Se uma decisão mudar a arquitetura (ex.: o driver da Pico não aceitar frames extras), pare e me explique as opções antes de seguir.
- Seja honesto sobre o que não funcionou. Prefiro "isso não dá por causa X" do que algo que finge funcionar.

Comece pela fase 2: leia o código, analise o driver do Pico Connect instalado neste PC e me diga o que precisa de mim para o primeiro teste do proxy.
