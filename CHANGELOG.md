# Changelog

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões seguem [SemVer](https://semver.org/lang/pt-BR/).

## [Não lançado]

## [0.1.0-alpha] - 2026-10-31

Primeira versão pública — **proxy de diagnóstico** (não gera frames ainda).

### Adicionado
- Driver proxy OpenVR: carrega `<nome>_real.dll` e repassa todas as chamadas.
- Wrappers de `IServerTrackedDeviceProvider`, `IVRDriverContext`, `IVRServerDriverHost`, `ITrackedDeviceServerDriver` (HMD), `IVRVirtualDisplay` e `IVRDriverDirectModeComponent`.
- Log `ghostframe.log`: interfaces usadas, taxa de `Present`/vsync/poses, texturas, depth, flags de Motion Smoothing.
- Passthrough automático para versões de interface desconhecidas.
- Testes com SteamVR e driver de streaming simulados.
- Briefing de engenharia reversa (`docs/RE_BRIEF.md`).
