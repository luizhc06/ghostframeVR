<p align="center">
  <img src="docs/assets/banner.svg" alt="ghostframeVR" width="100%">
</p>

<p align="center">
  <a href="https://github.com/luizhc06/ghostframevr/actions/workflows/ci.yml"><img alt="CI" src="https://img.shields.io/github/actions/workflow/status/luizhc06/ghostframevr/ci.yml?branch=main&style=for-the-badge&label=build"></a>
  <a href="https://github.com/luizhc06/ghostframevr/releases"><img alt="Release" src="https://img.shields.io/github/v/release/luizhc06/ghostframevr?include_prereleases&style=for-the-badge&color=7c5cff"></a>
  <img alt="Plataforma" src="https://img.shields.io/badge/plataforma-Windows%20x64-22d3ee?style=for-the-badge">
  <img alt="SteamVR" src="https://img.shields.io/badge/SteamVR-driver%20proxy-1b2838?style=for-the-badge">
  <a href="LICENSE"><img alt="Licença" src="https://img.shields.io/github/license/luizhc06/ghostframevr?style=for-the-badge"></a>
</p>

<p align="center">
  <b>Frame generation para headsets VR que fazem streaming pelo SteamVR — sem tocar no jogo.</b><br>
  Seu PC renderiza 36 fps. Seu headset mostra 72.
</p>

<p align="center">
  <a href="#-como-funciona">Como funciona</a> •
  <a href="#-roadmap">Roadmap</a> •
  <a href="#-instalação">Instalação</a> •
  <a href="#-lendo-o-log">Lendo o log</a> •
  <a href="#-faq">FAQ</a> •
  <a href="CONTRIBUTING.md">Contribuir</a>
</p>

---

> [!WARNING]
> **Projeto em fase alpha / experimental.** A versão atual (`v0.1`) é um **proxy de diagnóstico**: ela ainda **não gera frames**, só observa e registra como o vídeo passa pelo driver. Use por sua conta e risco e sempre faça backup.

## 👻 Por que

Jogos sociais em VR (mundos cheios, dezenas de avatares) derrubam o fps para 20–40 — quase sempre por **gargalo de CPU**. Upscalers como DLSS/FSR não ajudam (aliviam a GPU, não a CPU) e mods dentro do jogo são bloqueados por anti-cheat.

Headsets nativos resolvem isso com reprojeção (ASW, Motion Smoothing). Em **streaming via Wi-Fi**, essa camada é limitada ou inexistente. O ghostframeVR quer preencher esse buraco **fora do processo do jogo**.

## ⚙️ Como funciona

```mermaid
flowchart LR
    A[🎮 Jogo<br/>36 fps] --> B[Compositor<br/>SteamVR]
    B --> C{{👻 ghostframeVR<br/>driver proxy}}
    C -->|frames reais + sintéticos| D[Driver de streaming<br/>ex.: Pico Connect]
    D --> E[Encoder] --> F[📶 Wi-Fi] --> G[🥽 Headset<br/>72 fps]
    style C fill:#7c5cff,stroke:#22d3ee,color:#fff
```

1. O SteamVR carrega drivers como DLLs. O ghostframeVR se coloca **no lugar da DLL do driver de streaming** e carrega a original por trás.
2. Todas as chamadas são repassadas — o driver original continua funcionando normalmente.
3. Como cada frame passa por nós, podemos (fases futuras) **gerar frames intermediários por extrapolação** — optical flow acelerado por hardware + correção pela pose atual da cabeça — quando o jogo atrasa.

Tudo roda dentro do `vrserver.exe` do SteamVR. **O processo do jogo nunca é tocado.**

> Por que extrapolar e não interpolar como o Lossless Scaling? Interpolação precisa esperar o próximo frame → +latência → enjoo em VR. Extrapolação usa só frames passados + a pose mais recente.

## 🗺️ Roadmap

| | Fase | Descrição |
|:-:|---|---|
| ✅ | **1 — Diagnóstico** | Proxy transparente que loga caminho de vídeo, taxa de frames, texturas, depth, poses e flags de Motion Smoothing |
| 🔄 | **2 — Engenharia reversa** | Entender o `Present` → encode → envio do driver ([briefing](docs/RE_BRIEF.md)) |
| ⏳ | **3 — Captura** | Capturar frames D3D11 + poses e gravar para testes offline |
| ⏳ | **4 — Frame gen offline** | Protótipo de extrapolação com NVIDIA Optical Flow em frames gravados |
| ⏳ | **5 — Injeção** | Inserir frames sintéticos ao vivo quando o jogo perde o vsync |

Acompanhe em [Issues](https://github.com/luizhc06/ghostframevr/issues) e [Milestones](https://github.com/luizhc06/ghostframevr/milestones).

### Compatibilidade

| Driver de streaming | Status |
|---|---|
| Pico Connect (Pico 4) | 🎯 alvo principal — em teste |
| Outros drivers SteamVR de streaming | 🤷 talvez funcione (o proxy é genérico) — [mande um relatório](https://github.com/luizhc06/ghostframevr/issues/new?template=test_report.yml) |

## 📦 Instalação

> [!IMPORTANT]
> Feche o **SteamVR** e o app de streaming antes de começar.

1. Baixe o `.zip` da [última release](https://github.com/luizhc06/ghostframevr/releases) e extraia.
2. **Ache a pasta do driver.** Abra `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` no Bloco de Notas; em `external_drivers` está a pasta do driver de streaming. A DLL fica em `bin\win64\driver_<nome>.dll`.
   <sub>Alternativa: `Steam\logs\vrserver.txt` lista o caminho de cada driver carregado.</sub>
3. **Faça backup** da pasta `bin\win64` inteira.
4. Renomeie a DLL original: `driver_<nome>.dll` → `driver_<nome>_real.dll`.
5. Renomeie a `driver_ghostframe.dll` da release para `driver_<nome>.dll` e copie para a mesma pasta.
6. Abra o app de streaming, conecte e abra o SteamVR. Jogue 2–3 minutos.
7. O log fica em `bin\win64\ghostframe.log` (se a pasta não for gravável, ex.: `Program Files`, vai para `%TEMP%\ghostframe.log`).

### Desinstalar

Apague a nossa DLL e renomeie `driver_<nome>_real.dll` de volta para o nome original. Atualizações do app de streaming normalmente desfazem a instalação sozinhas.

## 🔍 Lendo o log

| Linha | Significado |
|---|---|
| `GetComponent("IVRVirtualDisplay_002") -> IMPLEMENTADO` | O driver usa o caminho *VirtualDisplay* (típico de streaming) |
| `GetComponent("IVRDriverDirectModeComponent_009") -> IMPLEMENTADO` | O driver usa o caminho *DirectMode* |
| `STATS ... Present  72.0/s` | Frames por segundo saindo para o headset |
| `SubmitLayer ... depth=0x...` | Depth ≠ `0x0` → o jogo envia profundidade (frame gen bem melhor) |
| `GetFrameTiming ... motionSmoothing=on` | SteamVR acha que o Motion Smoothing está ativo |
| `!! versão diferente` | Interface desconhecida — o proxy entra em modo passthrough puro |

Achou algo? [Abra um relatório de teste](https://github.com/luizhc06/ghostframevr/issues/new?template=test_report.yml) com o log anexado. 🙏

## 🛠️ Compilando

**Windows (Visual Studio 2022 + CMake):**

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

**Linux → Windows (MinGW-w64):**

```bash
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

**Testes** (Linux — simulam SteamVR + driver falso):

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

<details>
<summary><b>Estrutura do projeto</b></summary>

```
src/proxy/       driver proxy (HmdDriverFactory + wrappers das interfaces OpenVR)
tests/           SteamVR e driver de streaming simulados
docs/            briefing de engenharia reversa, assets
third_party/     openvr_driver.h (Valve, BSD)
cmake/           toolchain MinGW
```
</details>

## ❓ FAQ

<details>
<summary><b>Posso tomar ban em jogo com anti-cheat?</b></summary>

O ghostframeVR não injeta nada no jogo, não lê nem altera sua memória e roda num processo separado (o do SteamVR), do mesmo jeito que qualquer driver de VR. Dito isso, ninguém pode dar garantia absoluta — use com consciência.
</details>

<details>
<summary><b>Já gera frames?</b></summary>

Ainda não. A `v0.1` é só diagnóstico. A geração de frames vem nas fases 4–5 do roadmap.
</details>

<details>
<summary><b>Por que não usar o Motion Smoothing do SteamVR?</b></summary>

Use, se ele funcionar com o seu headset! Em muitos drivers de streaming ele não ativa. O próprio log do ghostframeVR mostra se está ativo.
</details>

<details>
<summary><b>Funciona com placas AMD?</b></summary>

O proxy (fase 1) funciona com qualquer GPU. O frame gen planejado usa NVIDIA Optical Flow (RTX 20+); um caminho alternativo para AMD/Intel está em aberto.
</details>

## 🤝 Contribuindo

Relatórios de teste com log são a contribuição mais valiosa agora. Veja o [CONTRIBUTING.md](CONTRIBUTING.md).

## ⚖️ Aviso legal

Projeto independente, sem afiliação com Pico/ByteDance, Valve ou qualquer desenvolvedora de jogos. Nomes e marcas pertencem aos seus donos e são citados só para indicar compatibilidade. Não distribua binários de terceiros. A engenharia reversa feita aqui tem fim exclusivo de interoperabilidade.

## 📄 Licença

[MIT](LICENSE) © Luiz Castro. `third_party/openvr/openvr_driver.h` © Valve Corporation, licença BSD-3.
