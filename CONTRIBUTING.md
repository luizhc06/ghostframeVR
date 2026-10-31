# Contribuindo com o ghostframeVR

Valeu pelo interesse! 👻

## A forma mais útil de ajudar agora: testar

Instale a [última release](https://github.com/luizhc06/ghostframevr/releases), use 2–3 minutos e
[abra um relatório de teste](https://github.com/luizhc06/ghostframevr/issues/new?template=test_report.yml)
com o `ghostframe.log`. Testes com headsets e apps de streaming diferentes são muito bem-vindos.

## Código

1. Abra uma issue antes de mudanças grandes, para alinharmos a abordagem.
2. Crie uma branch a partir da `main`.
3. Garanta que os testes passam: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
4. Abra um PR usando o template.

### Regras do proxy

- **Nunca** mude o comportamento do driver real sem uma flag explícita. Por padrão, tudo é repassado.
- Só embrulhe interfaces cuja **versão exata** conhecemos. Versão diferente → passthrough + log `!!`.
- Nada de código que rode dentro do processo do jogo.
- Log em hot path (Present, SubmitLayer) só nas primeiras chamadas + estatísticas agregadas.

### Estilo

C++17, 4 espaços, `namespace gf`. Comentários e logs em português.

## Engenharia reversa

Veja [`docs/RE_BRIEF.md`](docs/RE_BRIEF.md). Compartilhe **conclusões** (fluxos, offsets, estruturas), **nunca** binários ou código descompilado de terceiros.
