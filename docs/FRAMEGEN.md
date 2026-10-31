# Motor do frame gen

Gera frames extras por **extrapolação** a partir dos dois últimos frames reais, dentro do
`vrserver.exe`, em D3D11 com compute shaders. O frame gerado sai com a pose do último frame
real: o timewarp do headset corrige o giro da cabeça, o motor só extrapola o movimento dos
objetos.

| Arquivo | O que é |
|---|---|
| `src/proxy/framegen.h` | Contrato usado pelo proxy |
| `src/proxy/framegen.cpp` | Implementação D3D11 + HLSL embutido (fora do Windows, tudo devolve `false`) |
| `src/proxy/framegen_ref.{h,cpp}` | O mesmo algoritmo em C++ puro: especificação executável |
| `tests/framegen_ref_test.cpp` | Testa o algoritmo com uma cena sintética (qualquer plataforma) |
| `tests/framegen_d3d_test.cpp` | Roda a GPU (WARP no CI) e compara com a referência |

## Uso pelo proxy

1. `Iniciar()` uma vez, fora do hot path: cria o device e compila os shaders (centenas de ms).
2. `DefinirProjecao(olho, ...)` com as tangentes do `SetDisplayProjectionRaw` / `GetProjectionRaw`.
3. No `Present(sync)` de um frame real: `Travar(sync)`, `NovoFrame(tex, bounds, mHmdPose, nt)`,
   `Destravar()`, e só então o `Present` do driver real.
4. Para cada frame extra: `Gerar(alfa, saida, bounds, nt)`, esperar `Concluido()` (sem bloquear)
   e entregar ao driver com a pose do último frame real.
5. `Reiniciar()` quando swap sets forem destruídos, a resolução mudar ou o frame gen desligar.

Tudo na mesma thread (a do `Present`): o contexto D3D11 não é thread-safe.

## Algoritmo

Por olho, `W×H` = região `bounds` da textura do jogo.

**No `NovoFrame`** (uma vez por frame real):

1. Copia a região para `ultimo` (família TYPELESS, bits crus, sem conversão sRGB).
2. **Luma** em 1/4 da resolução por eixo (média de 4×4 pixels, pesos Rec.709 sobre os bits).
3. **Compensação do giro:** a luma do frame anterior é reamostrada (bilinear) na vista do
   frame atual pela homografia `H`. Pontos que o frame anterior não via ficam inválidos (-1).
4. **Casamento por blocos:** blocos de 8×8 pixels de luma, busca exaustiva de ±8 pixels, custo
   = média de |Δluma| só nos pixels válidos, mais `kVies·(|dx|+|dy|)` para preferir o zero em
   área lisa. Empate fica com o primeiro candidato da ordem fixa. Bloco que o frame anterior
   não via (borda revelada pelo giro) fica sem vetor.
5. **Suavização:** mediana vetorial 3×3 (o vizinho com menor soma de distâncias L1), vezes a
   confiança do bloco: 1 até `kCustoBom`, 0 a partir de `kCustoRuim`, linear entre os dois.
   Onde o casamento é ruim (desoclusão) o vetor vai a zero e fica o último frame.

**No `Gerar`** (uma vez por frame extra):

6. `G(x) = Ultimo(x − alfa·4·v(x))`, com `v` bilinear entre os centros dos blocos e o último
   frame também amostrado em bilinear, borda repetida. Escreve numa textura própria com UAV e
   copia para a região `bounds` da saída (as texturas do driver não têm UAV).

Parâmetros em `framegen_ref.h` (os shaders recebem por macro e constant buffer):

| Constante | Valor | Papel |
|---|---|---|
| `kEscala` | 4 | luma em 1/4 por eixo |
| `kBloco` | 8 | bloco de 8×8 pixels de luma (32×32 cheios) |
| `kBusca` | 8 | ±8 pixels de luma = ±32 cheios por frame real |
| `kVies` | 1/1024 | custo por pixel de deslocamento |
| `kCustoBom` / `kCustoRuim` | 0,02 / 0,06 | faixa de confiança (|Δluma| médio, luma em 0..1) |

## Convenções

- **HMD (OpenVR):** x para a direita, y para cima, −z para a frente.
- **Pose:** `mHmdPose` é HMD → mundo (standing). Só a parte de rotação `R` (3×3) entra; a
  translação é ignorada.
- **Câmera:** x para a direita, y para baixo, z para a frente. `c = F·d`, `F = diag(1, −1, −1)`.
- **Tangentes (`ProjecaoOlho`):** com y para baixo, como `HmdRect2_t` (`vTopLeft`, `vBottomRight`):
  o ponto `(u, v)` da região, `u, v ∈ [0, 1]` e `v` crescendo para baixo, vê a direção de câmera
  `(esq + u·(dir − esq), cima + v·(baixo − cima), 1)`. `cima` negativo = olhando para cima.
- **Pixels:** coordenadas contínuas, centro do pixel `i` em `i + 0.5`.
- **K da luma:** `fx = (W/4)/(dir − esq)`, `cx = −esq·fx`, `fy = (H/4)/(baixo − cima)`, `cy = −cima·fy`.
- **Homografia:** `H = K · F · (R_antᵀ · R_atual) · F · K⁻¹` leva um ponto da luma do frame
  **atual** ao ponto da luma do frame **anterior** que via a mesma direção do mundo (amostragem
  para trás). `qz ≤ 0` (atrás da câmera anterior) é inválido. Cabeça virando para a esquerda: o
  centro atual cai à esquerda do centro no frame anterior (testado).
- **Vetores:** `v(p)` em pixels de luma, na vista do frame atual, com `Atual(p) ≈ AntRot(p − v(p))`:
  aponta no sentido do movimento. Em pixels cheios, `4·v`.
- **alfa:** fração do intervalo entre os dois últimos frames reais, para a frente, em `(0, 1]`.
  2× usa 0,5; 3× usa 1/3 e 2/3. O vetor é lido no próprio `x` (aproximação usual).
- **Bounds:** `[uMin, uMax] × [vMin, vMax]` da textura, arredondados para pixels. Invertidos
  (`min > max`) são recusados.

## Orçamento esperado (RTX 3060, dois olhos de 2000×2000)

Estimativa por banda de memória (~360 GB/s) e conta de ALU; meça com `MsGpu()`.

| Etapa | Custo | Por quê |
|---|---|---|
| `NovoFrame` | ~0,4 a 0,6 ms | cópia (64 MB) + luma (32 MB lidos) + 2×3969 grupos de casamento (~1,3 G instruções de thread) |
| `Gerar` | ~0,4 ms | por olho: lê 16 MB, escreve 16 MB, copia 16 MB |
| **Por frame gerado** | **~1 ms em 2×, ~0,7 ms em 3×** | a análise é dividida entre os frames extras |

VRAM: ~35 MB por olho. `MsGpu()` devolve o último `Gerar` mais o último `NovoFrame` (pior caso
do 2×), lidos por timestamp queries sem esperar a GPU.

## Limites conhecidos

- **Vetores inteiros na luma:** passo de 4 px cheios; o erro da extrapolação chega a ~2·alfa px.
  Próximo passo: refinamento subpixel por parábola nos custos vizinhos.
- **Movimento rápido:** acima de ±32 px cheios por frame real a busca satura e o bloco perde
  confiança (fica o último frame).
- **Desoclusão e bordas de objetos:** o vetor é interpolado entre blocos de 32 px, então a
  borda de um objeto em movimento estica um pouco; a área revelada fica com o último frame.
- **Translação da cabeça (paralaxe):** não é compensada; vira "movimento" e é extrapolada.
- **Rotação olho → cabeça** (displays inclinados) ignorada. O Pico 4 não tem.
- **Convenção vertical das tangentes:** se o headset tiver FOV vertical assimétrico e o driver
  usar a convenção oposta, o centro óptico vertical fica errado (efeito pequeno em giros pequenos).
- **Pose invertida:** se o `mHmdPose` vier mundo → HMD, a compensação dobra o giro em vez de
  cancelar. Sintoma: tremedeira forte só ao girar a cabeça. Correção: inverter as poses em `Homografia`.
- **Formatos:** só RGBA8 e BGRA8 (UNORM, UNORM_SRGB, TYPELESS), sem MSAA, fatia 0. BGRA8 depende
  de UAV tipado B8G8R8A8 no adaptador. Saída precisa ser da mesma família da entrada.
- **Keyed mutex:** só o `syncTexture` é travado. Se o driver criar as texturas do swap set com
  keyed mutex próprio, as cópias sem `AcquireSync` são descartadas (o log mostra `misc=0x100`).
- **Adaptador:** o device usa o adaptador padrão. Com o compositor em outra GPU (notebook
  híbrido) os handles não abrem (fica no log e o proxy repassa o frame original).
