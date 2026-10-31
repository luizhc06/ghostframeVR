# ghostframeVR

Frame generation para VR via **Pico Connect**, sem tocar no VRChat (o anti-cheat não é afetado).

## Como funciona

```
VRChat → compositor SteamVR → [ NOSSO PROXY ] → driver real do Pico Connect → encoder → Wi-Fi → Pico 4
```

O SteamVR carrega drivers como DLLs. Nós renomeamos a DLL do Pico Connect para
`<nome>_real.dll` e colocamos a nossa DLL com o nome original. A nossa DLL carrega
a real e repassa **todas** as chamadas — por isso tudo continua funcionando — mas
enxerga cada frame que vai para o headset. É ali que o frame gen será inserido.

Tudo roda dentro do `vrserver.exe` (processo do SteamVR), nunca dentro do VRChat.

## Fases

| Fase | O que | Status |
|---|---|---|
| 1 | **Proxy de diagnóstico**: repassa tudo e registra em log quais interfaces o Pico Connect usa, taxa de frames, texturas, poses, se há depth | ✅ pronto para testar |
| 2 | Engenharia reversa do driver (ver [`docs/RE_BRIEF.md`](docs/RE_BRIEF.md)) | ⏳ |
| 3 | Captura dos frames (D3D11, texturas compartilhadas) + gravação para teste offline | ⏳ |
| 4 | Frame gen por extrapolação: optical flow (NVIDIA Optical Flow, RTX 3060) + correção por pose | ⏳ |
| 5 | Injetar frames sintéticos quando o jogo atrasa | ⏳ |

## Fase 1 — instalar o proxy de diagnóstico

### 1. Achar o driver do Pico Connect

Abra `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` no Bloco de Notas. Na lista
`external_drivers` aparece a pasta do driver da Pico. Dentro dela haverá
`driver.vrdrivermanifest` e uma DLL em `bin\win64\driver_<nome>.dll`.

(Alternativa: `Steam\logs\vrserver.txt` mostra o caminho de cada driver carregado.)

### 2. Compilar com o nome certo

Visual Studio 2022 (com "Desenvolvimento para desktop com C++") + CMake:

```
cmake -S . -B build -A x64 -DGF_DRIVER_NAME=driver_<nome>
cmake --build build --config Release
```

Ou do Linux com MinGW: `cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DGF_DRIVER_NAME=driver_<nome>`.

### 3. Instalar (com o SteamVR e o Pico Connect FECHADOS)

1. **Faça backup** da pasta `bin\win64` inteira.
2. Renomeie `driver_<nome>.dll` → `driver_<nome>_real.dll`.
3. Copie a nossa `driver_<nome>.dll` para a mesma pasta.
4. Abra o Pico Connect, conecte, abra o SteamVR e jogue VRChat uns 2–3 minutos
   (de preferência num mundo cheio, pra pegar fps baixo).
5. Feche tudo e pegue o `ghostframe.log` gerado em `bin\win64`.

### Desinstalar / se algo der errado

Apague a nossa DLL e renomeie `driver_<nome>_real.dll` de volta. Pronto.
Atualizações do Pico Connect provavelmente desfazem a instalação sozinhas.

## O que o log responde

- `GetComponent("IVRVirtualDisplay_002") -> IMPLEMENTADO` ou `IVRDriverDirectModeComponent_009` → qual caminho de vídeo o Pico usa (define onde o frame gen entra).
- `STATS ... Present  X/s` → quantos frames por segundo saem para o headset.
- `SubmitLayer ... depth=0x...` → se o VRChat manda **profundidade** (depth ≠ 0 melhora muito a qualidade do frame gen).
- `GetFrameTiming ... motionSmoothing=on/off` → se o SteamVR acha que o Motion Smoothing está ativo.
- Linhas `!!` → versões de interface diferentes das conhecidas (o proxy passa a só repassar).

## Testes

```
cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)
```

Simulam o SteamVR e um driver Pico falso e verificam que todas as chamadas atravessam o proxy.

## Avisos

- Projeto pessoal e experimental. Não redistribua DLLs da Pico.
- Não modifica o VRChat nem nenhum processo dele.
- `third_party/openvr/openvr_driver.h` é da Valve (licença BSD, ver `third_party/openvr/LICENSE`).
