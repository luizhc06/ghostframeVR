# Changelog

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões seguem [SemVer](https://semver.org/lang/pt-BR/).

## [Não lançado]

## [0.2.7-alpha] - 2026-11-01

### Adicionado
- Desempenho: perfis de um clique (Qualidade, Equilibrado, Desempenho, Máximo fps) que ajustam frame gen, resolução e motion smoothing juntos. Mexeu em algo à mão, o perfil vira "personalizado".
- Ajustes em linhas com nome e explicação curta, como num app de configurações.

## [0.2.6-alpha] - 2026-11-01

### Corrigido
- **Frame gen nunca ligava com o Pico Connect.** O driver da Pico usa a versão 008 do DirectMode (`IVRDriverDirectModeComponent_008`, SDK antigo) e o proxy só conhecia a 009: repassava sem embrulhar, então nenhum frame passava pelo frame gen. Agora a 008 é embrulhada, com a definição exata copiada do SDK oficial da Valve, e passa pelo mesmo frame gen. A explicação da 0.2.4 (plugin do compositor) estava errada: o SteamVR nunca pede o plugin ao Pico.
- Projeção dos olhos lida também do `IVRDisplayComponent_002` (Pico).
- Quadro repetido pelo compositor é reconhecido pela pose de render, mesmo que venha em outra textura.

### Mudado
- Desempenho: chave grande de ligar/desligar o frame gen. Auto/Manual só aparece com ele ligado e fica lembrado ao desligar.

## [0.2.5-alpha] - 2026-10-31

### Mudado
- O frame gen tem um lugar só: o card da aba Desempenho (Desligado / Auto / Manual), que agora mostra ao vivo o estado e quantos frames reais ele está pegando e quantos está gerando por segundo, além do custo de GPU. A barra repetida do Painel saiu; a opção da bandeja continua.

## [0.2.4-alpha] - 2026-10-31

### Corrigido
- Frame gen nunca ligava com o Pico Connect porque o vídeo não passava pelo proxy: drivers como esse entregam os frames por um **plugin carregado dentro do vrcompositor.exe** (`IVRCompositorPluginProvider`), e o proxy só repassava esse pedido. Agora o DirectMode do plugin também é embrulhado, o proxy roda o frame gen dentro do compositor e calcula o frustum dos olhos pela matriz de projeção de cada frame.
- O app junta no log os dois processos do SteamVR (vrserver e vrcompositor), então fps, frame time, frames gerados e o resumo de componentes aparecem de qualquer um deles.
- Estatísticas `FRAMEGEN` só saem do processo que vê os frames; o aviso de "sem frames" diz de qual processo está falando.
- O HUD piscava a cada atualização: agora usa duas texturas de GPU alternadas (em vez de `SetOverlayRaw`) e só reenvia quando a imagem muda.

### Adicionado
- Painel: chave para ligar e desligar o frame gen ao vivo, com quantos frames reais ele está pegando e quantos está gerando por segundo, e o custo de GPU.
- Menu do ícone da bandeja: "Ligar/desligar frame gen", sem abrir a janela.
- Estatísticas do frame gen no log a cada segundo.

## [0.2.3-alpha] - 2026-10-31

Correções do primeiro teste real.

### Corrigido
- O app não conectava ao SteamVR ("não encontrou o vrclient"): o `vrclient_x64.dll` fica em `SteamVR\bin`, não em `bin\win64`. Isso deixava o HUD, os ajustes ao vivo e o fps do jogo sem funcionar.
- O proxy agora embrulha também o dispositivo de vídeo à parte (DisplayRedirect / `IVRVirtualDisplay`), usado por alguns drivers de streaming.
- Lista do driver do headset: o driver em uso aparece marcado com "em uso" (também no Automático), comparando caminhos na mesma forma.

### Adicionado
- A trava de fps vale para o jogo aberto no SteamVR (qualquer jogo), a não ser que você fixe um na lista.
- Diagnóstico: com o headset ativo e nenhum frame passando pelo proxy por 15 s, o log avisa; o app mostra "sem frames" e a aba Log resume componentes de vídeo e interfaces do driver.

## [0.2.2-alpha] - 2026-10-31

### Corrigido
- Escolher a pasta do driver salva de verdade e mostra o que ficou em uso: o app aceita o caminho entre aspas ("Copiar como caminho" do Explorer), a pasta `bin\win64` ou o próprio `.dll`, valida antes de salvar e mantém o driver escolhido na lista mesmo que ele não tenha sido detectado.
- O Automático mostra qual driver encontrou.
- Gravar o `ghostframe.ini` tenta de novo quando o proxy está lendo o arquivo naquele instante.

### Adicionado
- Painel: frames gerados por segundo (número e linha no gráfico de FPS).

### Removido
- Opções que ainda não faziam nada: gravação de frames, qualidade e correção por pose (esta sempre está ligada no motor).

## [0.2.1-alpha] - 2026-10-31

### Corrigido
- "DLL não encontrada": o app acha a DLL que está de fato na pasta do driver, aceita manifesto com comentários (como o SteamVR) e também descobre drivers pelo `vrserver.txt` do SteamVR.
- A lista de drivers mostra só headsets por streaming (Pico Connect, ALVR, Virtual Desktop, Steam Link, Meta Link); trackers e utilitários como o Standable não aparecem nem são escolhidos no automático.

### Mudado
- Ícone novo: fantasma com headset VR.

## [0.2.0-alpha] - 2026-10-31

Primeira versão com frame gen (experimental, desligado por padrão).

### Adicionado
- Frame gen por extrapolação na GPU, dentro do driver (`docs/FRAMEGEN.md`):
  - luma em 1/4 da resolução, frame anterior reprojetado pela pose (o giro da cabeça fica com o timewarp do headset), fluxo por blocos e deformação do último frame real;
  - com a trava do SteamVR (½ ou ⅓), o proxy entrega um frame gerado em cada vsync entre os reais; sem trava, troca os frames que o compositor repetiria;
  - frames gerados com antecedência na GPU (o Present nunca espera); qualquer falha volta a só repassar e fica no log.
- `ghostframe.ini` relido em segundo plano pelo proxy; estatísticas `FRAMEGEN` no log a cada 2 s.
- App: liga o frame gen em Auto (2× ou 3× pelo que o jogo aguenta) ou Manual, mostra estado, frames gerados e custo de GPU; só trava o jogo com o frame gen funcionando e desfaz a trava que pôs quando ele para.
- HUD mostra frames reais, gerados e o custo de GPU.
- Barra lateral de volta na interface.
- Testes: referência do algoritmo em CPU (Linux e Windows) e motor D3D11 rodando em GPU por software na CI do Windows.

## [0.1.3-alpha] - 2026-10-31

### Adicionado
- Companion do SteamVR dentro do app (processo à parte, nunca no jogo):
  - HUD no headset estilo relógio: aparece ao virar o pulso para o rosto e some ao abaixar o braço. Âncora no pulso, na visão ou no mundo, com tamanho, opacidade, posição e itens ajustáveis, e prévia real na janela;
  - ajustes do SteamVR ao vivo, com o jogo aberto: resolução, motion smoothing e trava de fps por jogo;
  - lê do SteamVR o modelo e os Hz do headset e o fps real do jogo;
  - modo Auto/Manual do frame gen. O Auto recomenda 2× ou 3× pelo que o jogo aguenta e nunca trava o jogo enquanto o frame gen não existir na DLL;
  - opção de iniciar junto com o SteamVR.
- Ícone na bandeja: fechar a janela libera a RAM da interface e o app segue leve em segundo plano. Só uma cópia do app roda por vez.

### Mudado
- Interface mais enxuta: abas Painel, Desempenho, HUD, Instalação e Log, sem textos repetidos e sem efeitos pesados.
- Fase 2 do roadmap: validar pelo log o que chega ao driver, em vez de desmontar o binário da Pico.

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

Primeira versão pública: **proxy de diagnóstico** (não gera frames ainda).

### Adicionado
- Driver proxy OpenVR: carrega `<nome>_real.dll` e repassa todas as chamadas.
- Wrappers de `IServerTrackedDeviceProvider`, `IVRDriverContext`, `IVRServerDriverHost`, `ITrackedDeviceServerDriver` (HMD), `IVRVirtualDisplay` e `IVRDriverDirectModeComponent`.
- Log `ghostframe.log`: interfaces usadas, taxa de `Present`/vsync/poses, texturas, depth, flags de Motion Smoothing.
- Passthrough automático para versões de interface desconhecidas.
- Testes com SteamVR e driver de streaming simulados.
- Briefing de engenharia reversa (`docs/RE_BRIEF.md`).
