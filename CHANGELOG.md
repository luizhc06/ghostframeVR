# Changelog

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões seguem [SemVer](https://semver.org/lang/pt-BR/).

## [Não lançado]

## [0.1.2-alpha] - 2026-10-31

### Adicionado
- App de controle `ghostframeVR.exe` (Tauri 2, tema Y2K preto e ciano):
  - detecta drivers de streaming (externos e os embutidos do SteamVR: Pico Connect, ALVR, Steam Link, Meta Link) e instala, atualiza ou desinstala o proxy com backup e UAC quando precisa;
  - avisa quando uma atualização do driver removeu o proxy;
  - mostra o status do SteamVR, do headset, do driver e do proxy, com gráficos de fps e frame time lidos do `ghostframe.log`;
  - guarda as opções de frame gen em `%LOCALAPPDATA%\ghostframeVR\ghostframe.ini` (a DLL ainda não lê);
  - trava experimental de fps por jogo via throttling por app do SteamVR (qualquer jogo que o SteamVR conhece);
  - gera relatório de teste.

## [0.1.1-alpha] - 2026-10-31

### Corrigido
- Log cai para `%TEMP%\ghostframe.log` quando a pasta do driver não é gravável (ex.: `Program Files`).

## [0.1.0-alpha] - 2026-10-31

Primeira versão pública — **proxy de diagnóstico** (não gera frames ainda).

### Adicionado
- Driver proxy OpenVR: carrega `<nome>_real.dll` e repassa todas as chamadas.
- Wrappers de `IServerTrackedDeviceProvider`, `IVRDriverContext`, `IVRServerDriverHost`, `ITrackedDeviceServerDriver` (HMD), `IVRVirtualDisplay` e `IVRDriverDirectModeComponent`.
- Log `ghostframe.log`: interfaces usadas, taxa de `Present`/vsync/poses, texturas, depth, flags de Motion Smoothing.
- Passthrough automático para versões de interface desconhecidas.
- Testes com SteamVR e driver de streaming simulados.
- Briefing de engenharia reversa (`docs/RE_BRIEF.md`).
