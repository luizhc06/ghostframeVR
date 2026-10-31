# Plano B: engenharia reversa do driver SteamVR do Pico Connect

> **Plano atual:** não desmontar o binário. O ghostframe é um intermediário na
> interface pública do SteamVR (OpenVR): frames, poses e tempos passam por ela e
> o proxy já registra tudo no `ghostframe.log`. Este briefing só vale se o log do
> teste real mostrar algo que a interface não resolve (ex.: o driver ignora o
> conteúdo que entregamos ou limita a taxa por conta própria).


**Objetivo:** inserir frames sintéticos (frame generation) entre o compositor do
SteamVR e o encoder do Pico Connect, via uma DLL proxy (ver `src/proxy/`). Já
temos um proxy que repassa tudo e loga as chamadas OpenVR. A RE precisa
responder o que o log **não** consegue ver: o que acontece *dentro* do driver.

**Escopo:** apenas interoperabilidade, uso pessoal. Não redistribuir binários
da Pico. Não mexer em VRChat / Easy Anti-Cheat.

## Alvos

- A DLL do driver SteamVR do Pico Connect (`bin\win64\driver_*.dll`) e o `driver.vrdrivermanifest`.
- DLLs que ela importa (encoder, rede, etc.).
- O processo do app Pico Connect no PC, se o driver conversa com ele.

## Perguntas, em ordem de prioridade

### P1: Caminho de vídeo
1. O HMD implementa `IVRVirtualDisplay_002`, `IVRDriverDirectModeComponent_009`, ou ambos? Qual o SteamVR de fato usa?
2. Strings de versão exatas de todas as interfaces que o driver pede/implementa (procure strings `IVR*_0NN`). Precisamos bater 1:1 com `openvr_driver.h`.

### P2: O que acontece no `Present`
3. Ao receber a textura (`backbufferTextureHandle` ou as do `SubmitLayer`), o driver:
   - abre via `ID3D11Device::OpenSharedResource` / `OpenSharedResource1`?
   - copia para textura própria (`CopyResource`) ou encoda direto?
4. Qual encoder: NVENC (`nvEncodeAPI64.dll` / `NvEncodeAPICreateInstance`), AMF, MediaFoundation, outro?
5. O encode é síncrono dentro do `Present` ou numa thread/fila separada? Tamanho da fila?
6. Ele encoda **todo** `Present` ou limita taxa (ex.: só 72/90 por segundo)? Se mandarmos `Present` extras, o que acontece?

### P3: Pose e timing (crítico para frames sintéticos)
7. Como cada frame encodado é associado a uma pose da cabeça? (frameId? timestamp? pose embutida no pacote?) O headset faz timewarp usando essa pose: um frame sintético precisa da pose certa.
8. De onde vem o timing de vsync (`GetTimeSinceLastVsync`, `VsyncEvent`)? É sincronizado com o headset pela rede?
9. Implementa `GetFrameTiming` / lê os flags `VRCompositor_ReprojectionMotion_*` (Motion Smoothing)?

### P4: Proteções e IPC
10. O driver ou o app Pico Connect verifica hash/assinatura das DLLs do driver? O updater restaura arquivos?
11. Há IPC entre o driver e o app do PC (named pipes, shared memory, sockets locais)? O encode acontece no `vrserver.exe` ou em outro processo?
12. Há configs ocultas (registro, JSON, flags de linha de comando) relacionadas a frame rate, reprojeção, "smoothing", "ASW", "spacewarp", "interpolation"? Procure strings.

## Entregáveis esperados

- Respostas às perguntas acima, com endereços/RVAs e nomes de funções relevantes.
- Pseudocódigo do fluxo `Present` → encode → envio.
- Formato do pacote/estrutura que carrega pose + frame, se encontrado.
- Lista de qualquer verificação de integridade.

## Como o proxy ajuda

O `ghostframe.log` (Fase 1) já mostra em runtime: quais componentes são
pedidos, handles de textura, taxa de `Present`, se há depth, poses e flags de
Motion Smoothing. Use-o para confirmar o que a análise estática sugerir.
