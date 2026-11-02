# Changelog

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões seguem [SemVer](https://semver.org/lang/pt-BR/).

## [0.2.16-alpha] - 2026-11-01

### Corrigido
- **"Atualizar agora" dava "Acesso negado (os error 5)"**, mesmo com a permissão do Windows e o SteamVR fechado. Algum programa ainda estava com a DLL do driver carregada, e o Windows não deixa escrever por cima de uma DLL carregada, nem como administrador. Mas deixa renomear: agora a DLL velha sai do caminho e a nova entra no lugar (vale também para desinstalar). Se a DLL abre para escrita mas a pasta não deixa criar o arquivo novo, o app pede a permissão do Windows em vez de falhar.

## [0.2.15-alpha] - 2026-11-01

Tudo o que mudou desde o último teste real (relatório da v0.2.14 com o Pico 4 e o VRChat).

### O que o teste real mostrou
- **O proxy instalado era o da 0.2.12.** O app da 0.2.14 abriu, mas o driver do Pico continuou com a DLL antiga: o relógio próprio e o Manual 2x/3x da 0.2.14 nunca rodaram. Atualizar a DLL precisa do SteamVR fechado, e o aviso de versão diferente era só um toast que passava despercebido.
- **A trava do SteamVR chega ao Pico, às vezes.** Com ela o 2x funcionou de verdade: o log mediu metade dos quadros reais e metade gerados, somando os Hz do headset.
- **E logo parava (o liga e desliga que você viu).** O compositor do SteamVR já segura o syncTexture do próximo quadro enquanto o PostPresent roda. O proxy precisava dele para entregar o gerado, não conseguia (1091 falhas "syncTexture ocupado no PostPresent" em 44 s) e desistia. O jogo ficava travado na metade sem os gerados, o fps caía, o app desfazia a trava, o fps subia e o ciclo recomeçava.
- **As estatísticas do SteamVR não contam os gerados.** O frame timing e o fps do SteamVR (e o contador do VRChat) medem o compositor, que só vê os quadros do jogo. Os gerados saem do proxy direto para o driver do Pico. O total aparece no ghostframe: Pegando (reais), Gerando e o fps do Headset no Painel.

### Corrigido
- **Gerados com syncTexture próprio.** O motor cria um keyed mutex 1x1 compartilhado, sempre livre, e todo quadro gerado é entregue com ele. O syncTexture do compositor só é usado para reenviar o quadro real, e só quando está livre (sem esperar). É a correção do liga e desliga.
- **O frame gen não fica mais parado de vez.** Antes, depois de 60 falhas seguidas, 120 gerados atrasados ou o motor sem iniciar, ele desistia até você desligar e religar. Agora o proxy repassa tudo por 10 s e tenta de novo sozinho. A proteção do app (que desfaz a trava quando nenhum gerado é entregue) também volta sozinha depois de 30 s.
- **Relógio próprio travado por um Present lento.** O pior caso do Present só era medido ao entregar um gerado: um Present lento uma única vez fazia o relógio achar que nunca havia tempo e parar de gerar para sempre. Agora cada quadro real mede o driver e o pior caso vai esquecendo.
- **Proxy de outra versão.** Uma faixa amarela no topo de todas as abas avisa que o frame gen desta versão não está rodando e atualiza o proxy num clique (com o SteamVR fechado).

### Testes
O teste ponta a ponta roda o proxy de verdade entre um SteamVR e um Pico falsos, numa GPU D3D11. Ele roda na CI a cada mudança e no seu PC pelo autoteste (aba Desempenho, "Testar agora"). O compositor falso agora segura o syncTexture durante o PostPresent, como o do SteamVR real, que é a situação que quebrava o 2x no Pico. Resultado na CI desta versão:

| Cenário | O que confere | Resultado |
|---|---|---|
| a | frame gen desligado: tudo passa intacto | 20 de 20 |
| b | quadro repetido pelo compositor vira gerado | 59 de 60 (98%) |
| c | trava do SteamVR: um gerado no vsync seguinte | 58 de 60 (97%) |
| d | como o Pico: sem trava e sem repetido, o Auto gera no vsync vazio | 60 de 60 (100%) |
| e | Auto com o jogo a 2/3 dos Hz: um gerado a cada 2 reais, nunca mais Presents que vsyncs | 35 gerados em 70 quadros, 105 Presents em 105 vsyncs |
| f | driver com PostPresent lento (20 ms): o proxy nunca segura o compositor até o próximo quadro | 28 gerados em 50; compositor preso: mediana 52 ms, 90% até 81 ms, próximo quadro em 90 ms |
| g | Manual 2x: o proxy trava o jogo na metade | 50 de 50 (100%), 2 vsyncs por quadro real |
| h | Manual 3x: o proxy trava o jogo no terço | 40 de 40 com 2 gerados (100%), 3 vsyncs por quadro real |

Também passaram: os testes em Linux com o SteamVR simulado (DirectMode 008 do Pico byte a byte, trava ligada e desligada com o jogo aberto, PostPresent sem trava), os mesmos testes com ASan e UBSan, a lógica do app e o build do Windows.

### Para testar
1. Feche o SteamVR, abra o ghostframe e clique em **Atualizar agora** se a faixa amarela aparecer.
2. Desligue a suavização do próprio sistema (Motion Smoothing do SteamVR e, se o Pico Connect tiver, a dele): ela segura o jogo na metade por conta própria e briga com o frame gen.
3. Jogue alguns minutos no Auto e depois no Manual 2x, e mande o relatório (aba Log, Gerar relatório).

### Ainda não provado
- Se o headset do Pico mostra os quadros gerados. O teste prova que o proxy entrega ao driver no tempo certo, não o que o Pico faz depois.
- Quanto o PostPresent do Pico demora de verdade (o log da sessão mostra em `pp_ms`) e a qualidade da imagem gerada em movimento.

## [0.2.14-alpha] - 2026-11-01

### Mudado
- **Manual 2x/3x agora vale sempre**: sem a trava do SteamVR (o Pico Connect não recebe), o proxy trava o jogo ele mesmo na metade ou no terço dos Hz e entrega os quadros gerados nos vsyncs do meio. Só com o motor pronto; se o frame gen falhar, o jogo volta ao ritmo normal. O Auto completa até os Hz quando o jogo fica abaixo.

### Corrigido
- **O frame gen não gerava nada no Pico Connect em jogo de verdade.** O log real mostrou por quê: com o Pico, o SteamVR só chama o driver quando o jogo tem quadro novo (presents/s = quadros do jogo/s, nenhum repetido) e a trava por jogo não chega. Agora o proxy tem relógio próprio, como o Lossless Scaling: sabe os Hz do headset, mede o ritmo do jogo e, abaixo dos Hz, entrega o quadro gerado no vsync que ficaria vazio (1 gerado a 60 ou 45 fps em 90 Hz, 2 a 30 fps). Custo medido na RTX 3060 do teste real: 1,3 ms por quadro gerado em 2416x2416.

## [0.2.13-alpha] - 2026-11-01

### Corrigido
- Autoteste: na RTX 3060 do primeiro usuário ele pulava na sonda inicial ("a textura compartilhada chega com outro conteúdo"). A sonda escrevia de um jeito que GPU de verdade não garante (conteúdo na criação da textura). Agora escreve como o compositor (UpdateSubresource + espera a GPU), relê se chegar atrasado e mostra o resultado de cada jeito. Conferido numa GPU real via Proton/DXVK.
- Shader do frame gen compila também em compiladores mais restritos ([numthreads] com valor pronto).

## [0.2.12-alpha] - 2026-11-01

### Adicionado
- **Autoteste na sua GPU** (aba Desempenho, "Testar agora"): roda no seu PC o mesmo teste ponta a ponta da CI (proxy de verdade, Pico e SteamVR falsos, textura D3D11 na placa padrão) numa pasta temporária, sem abrir o SteamVR nem mexer nas suas configurações. Mostra a GPU usada, o custo em ms por frame gerado e quantos quadros foram trocados por gerados, com e sem trava.

## [0.2.11-alpha] - 2026-11-01

### Adicionado
- **Otimizar tudo** (aba VRChat): num clique, o jogo leve, perfil Desempenho, plano de energia Alto, SteamVR Home desligado e as opções de inicialização; depois confere item por item pelo que ficou gravado e mostra o que faltou e por quê.
- Opções de inicialização gravadas direto no VRChat da sua conta Steam (localconfig.vdf, com a Steam fechada e backup), mantendo as opções que você já tinha. Só a linha do VRChat muda no arquivo.
- O app atualiza o proxy do driver sozinho quando você abre uma versão nova (ou quando o programa do headset restaura a DLL dele), com o SteamVR fechado.

## [0.2.10-alpha] - 2026-11-01

### Adicionado
- Aba VRChat: "Usar impostor" (avatar bloqueado por malha, skinned meshes, materiais ou ossos aparece na versão leve que o VRChat gera) e "Esconder avatares longe" (Hide Avatar Beyond, 5 a 50 m). Explicação do que o rank mínimo faz: malha pesada vira impostor; PhysBones, luzes e partículas pesadas só são removidos. O Modo leve liga o impostor.

## [0.2.9-alpha] - 2026-11-01

### Adicionado
- Aba VRChat, sem tocar no processo do jogo (EAC):
  - **Modo VRChat leve** (um clique, com o VRChat fechado): bloqueio de Poor e Very Poor (o "Medium" que o VRChat sugere), até 15 avatares, partículas limitadas, espelho Quarter, sombras e detalhe Low, sem anti-aliasing e o perfil Desempenho do ghostframe. **Restaurar** volta as opções do jogo como eram.
  - Cada opção também tem controle próprio. Elas ficam no registro do VRChat (PlayerPrefs da Unity). Espelho, sombras, detalhe e anti-aliasing foram conferidos num VRChat real (menu x valor gravado); o bloqueio por rank e o limite de avatares por conta vêm de dumps públicos do registro. O app guarda o valor de antes da 1ª mudança.
  - Opções de inicialização documentadas (prioridade, thread principal, afinidade só em AMD) para copiar na Steam; limitador de partículas no config.json com backup.
  - Windows e SteamVR, como recomenda a ajuda de desempenho do VRChat: plano de energia (Equilibrado/Alto), agendamento de GPU por hardware (o VRChat recomenda desligar) e SteamVR Home.

## [0.2.8-alpha] - 2026-11-01

### Corrigido
- O SteamVR só manda a trava por jogo a headsets que declaram `Prop_Hmd_SupportsAppThrottling_Bool`. Com o frame gen ligado, o proxy declara no headset (o log mostra o valor que o driver tinha) e segura o vsync ele mesmo, como o Lossless Scaling faz no monitor: o jogo roda numa fração dos Hz e o proxy gera o resto. Ligar o frame gen com o SteamVR aberto também vale; desligar devolve o valor do driver.
- Com a trava declarada pelo proxy, o driver nunca recebe trava > 0: sem frame gerado (aquecendo, desligado, quadro repetido), o proxy segura os vsyncs sozinho, como o ALVR, em vez de deixar o jogo previsto à frente com o headset a taxa cheia.
- O ritmo dos frames gerados vem dos Hz do headset (`Prop_DisplayFrequency_Float`); o Pico não manda `VsyncEvent`.

### Adicionado
- Log e aba Desempenho dizem por que o frame gen não está gerando: quadros por segundo chegando, quantos repetidos e se há trava.
- O log diz em qual GPU o motor do frame gen abriu.

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
