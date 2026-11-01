// UI do app de controle. A lógica fica no Rust; aqui só desenhamos o `estado`,
// que é lido a cada segundo enquanto a janela está visível.
const { invoke } = window.__TAURI__.core;
const janela = window.__TAURI__.window.getCurrentWindow();
const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];

// ---------- fantasminha (mesmo desenho do ícone: icons/make_icon.py) ----------
const GHOST = [
  "................................",
  "................................",
  "..............SSSS..............",
  "............##SSSSss............",
  "..........####SSSS##ss..........",
  "........######SSSS####ss........",
  ".......#######SSSS#####ss.......",
  "......########SSSS######ss......",
  ".....#########SSSS#######ss.....",
  ".....#########SSSS#######ss.....",
  "...kkkkkkkkkkkkkkkkkkkkkkkkkk...",
  "..kKwwwKKKKKKKKKKKKKKKKKKKKKKk..",
  "..kKKKKKKKKKKKKKKKKKKKKKKKKKKk..",
  "SSkKKKKKcccccKKKKKKcccccKKKKKkSS",
  "SSkKKKKKcccccKKKKKKcccccKKKKKkSS",
  "..kKKKKKKKKKKKKKKKKKKKKKKKKKKk..",
  "..kKKKKKKKKKKKKKKKKKKKKKKKKKKk..",
  "...kkkkkkkkkkkkkkkkkkkkkkkkkk...",
  "....######################ss....",
  "....######################ss....",
  "....######################ss....",
  "....######################ss....",
  "....######################ss....",
  "....######################ss....",
  "....######################ss....",
  "....######################ss....",
  "....ssssssssssssssssssssssss....",
  ".....ssssss..ssssss..ssssss.....",
  "......ssss....ssss....ssss......",
  ".......ss......ss......ss.......",
  "................................",
  "................................",
];
const COR = { "#": "#00f0ff", s: "#00a0b0", K: "#060e14", k: "#3c606e", S: "#223a46", c: "#00f0ff", w: "#dcfcff" };
// `px` = tamanho de cada pixel do desenho de 16 (o grid tem 32: meio pixel cada).
function ghostSvg(px) {
  let r = "";
  GHOST.forEach((linha, y) =>
    [...linha].forEach((c, x) => { if (COR[c]) r += `<rect x="${x}" y="${y}" width="1.02" height="1.02" fill="${COR[c]}"/>`; }));
  return `<svg width="${16 * px}" height="${16 * px}" viewBox="0 0 32 32">${r}</svg>`;
}
$$("[data-ghost]").forEach((el) => (el.innerHTML = ghostSvg(+el.dataset.ghost)));

// ---------- DOM só muda quando o valor muda ----------
const texto = (el, s) => { if (el.textContent !== s) el.textContent = s; };
const html = (el, s) => { if (el._html !== s) el.innerHTML = el._html = s; };
const classe = (el, c) => { if (el.className !== c) el.className = c; };
const fmt = (v, d = 0) => (v == null ? "--" : v.toFixed(d).replace(".", ","));
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const tag = (txt, c) => `<span class="tag ${c}">${esc(txt)}</span>`;
const abaAtiva = (aba) => $("#aba-" + aba).classList.contains("ativa");

// ---------- janela e abas ----------
$("#btn-min").onclick = () => janela.minimize();
$("#btn-max").onclick = () => janela.toggleMaximize();
$("#btn-fechar").onclick = () => janela.close();
$$(".abas button").forEach((b) => (b.onclick = () => {
  $$(".abas button").forEach((x) => x.classList.toggle("ativa", x === b));
  $$(".aba").forEach((a) => a.classList.toggle("ativa", a.id === "aba-" + b.dataset.aba));
  if (b.dataset.aba === "hud") previa();
  if (ultimo) render(ultimo);
}));

function toast(msg, erro = false) {
  const t = document.createElement("div");
  t.className = "toast" + (erro ? " erro" : "");
  t.textContent = msg;
  $("#toasts").append(t);
  setTimeout(() => t.remove(), erro ? 9000 : 4500);
}

async function chamar(cmd, args, botao) {
  if (botao) { botao.disabled = true; botao.classList.add("ocupado"); }
  try {
    const r = await invoke(cmd, args);
    if (typeof r === "string") toast(r);
    return r;
  } catch (e) {
    toast(String(e), true);
  } finally {
    if (botao) { botao.disabled = false; botao.classList.remove("ocupado"); }
    atualizar();
  }
}

// ---------- configurações (ghostframe.ini) ----------
// Depois da primeira leitura, o `config` local é a fonte da verdade.
let config = {};
let editadoEm = 0;
function salvar(valores) {
  editadoEm = Date.now();
  Object.assign(config, valores);
  preencherConfig();
  const r = chamar("salvar_config", { valores });
  if (Object.keys(valores).some((k) => k.startsWith("hud."))) r.then(previa);
  return r;
}
// Sliders salvam 150 ms depois do último movimento.
const depois = (el, f, ms = 150) => { clearTimeout(el._t); el._t = setTimeout(f, ms); };
const saida = (el) => (el.nextElementSibling.textContent = el.value + el.dataset.unidade);

$$("input[type=checkbox][data-chave]").forEach((el) =>
  (el.onchange = () => salvar({ [el.dataset.chave]: el.checked ? "1" : "0" })));
$$("input[type=number][data-chave]").forEach((el) =>
  (el.onchange = () => salvar({ [el.dataset.chave]: el.value })));
$$("input[type=range][data-chave]").forEach((el) => (el.oninput = () => {
  config[el.dataset.chave] = el.value;
  saida(el);
  depois(el, () => salvar({ [el.dataset.chave]: el.value }));
}));
$$(".seg[data-chave] button").forEach((b) =>
  (b.onclick = () => salvar({ [b.parentElement.dataset.chave]: b.dataset.valor })));
$$("[data-item]").forEach((el) => (el.onchange = () =>
  salvar({ "hud.itens": $$("[data-item]:checked").map((x) => x.dataset.item).join(",") })));

let autoInstalou = false;
let framegenDisponivel = null; // null até o primeiro estado: força aplicar o estado inicial
const modoFrameGen = () => (config["framegen.ativo"] === "1" ? config["framegen.modo"] : "off");

function preencherConfig() {
  $$("input[type=checkbox][data-chave]").forEach((el) => (el.checked = config[el.dataset.chave] === "1"));
  $$("input[type=number][data-chave]").forEach((el) => (el.value = config[el.dataset.chave]));
  $$("input[type=range][data-chave]").forEach((el) => { el.value = config[el.dataset.chave]; saida(el); });
  $$(".seg[data-chave]").forEach((g) =>
    [...g.children].forEach((b) => b.classList.toggle("ativo", b.dataset.valor === config[g.dataset.chave])));
  const itens = (config["hud.itens"] || "").split(",");
  $$("[data-item]").forEach((el) => (el.checked = itens.includes(el.dataset.item)));
  const modo = modoFrameGen();
  // Desligado, o modo continua salvo: religar volta para o último Auto/Manual.
  $("#fg-modo-linha").hidden = $("#fg-vivo").hidden = modo === "off" || !framegenDisponivel;
  // Sem frame gen na DLL: a recomendação continua visível, mas não dá para ligar.
  $("#fg-auto").hidden = modo !== "auto" && framegenDisponivel;
  $("#fg-manual").hidden = modo !== "manual" || !framegenDisponivel;
  $(".previa").classList.toggle("off", config["hud.ativo"] !== "1");
}

// ---------- SteamVR ao vivo (ou steamvr.vrsettings com ele fechado) ----------
const vrSet = (chave, valor) => chamar("vr_set", { chave, valor });
$("#jogo").onchange = (ev) => salvar({ "app.jogo_fixo": ev.target.value });
$$("#trava button").forEach((b) => (b.onclick = () => vrSet("trava", +b.dataset.valor)));
$("#ss-manual").onchange = (ev) => vrSet("supersampleManual", ev.target.checked);
$("#ms").onchange = (ev) => vrSet("motionSmoothing", ev.target.checked);
$("#ss").oninput = (ev) => {
  const el = ev.target;
  el._mexido = Date.now();
  texto($("#ss-valor"), el.value + "%");
  depois(el, () => vrSet("supersample", el.value / 100), 200);
};

// ---------- perfis: um clique ajusta frame gen + SteamVR ----------
// Só mexem em ajustes reais; a ordem do vr importa (escala antes de ligar o manual).
const PERFIS = {
  qualidade: { cfg: { "framegen.ativo": "0" }, vr: { supersampleManual: false } },
  equilibrado: { cfg: { "framegen.ativo": "1", "framegen.modo": "auto" }, vr: { supersampleManual: false, motionSmoothing: false } },
  desempenho: {
    cfg: { "framegen.ativo": "1", "framegen.modo": "manual", "framegen.multiplicador": "2" },
    vr: { supersample: 0.8, supersampleManual: true, motionSmoothing: false },
  },
  maximo: {
    cfg: { "framegen.ativo": "1", "framegen.modo": "manual", "framegen.multiplicador": "3" },
    vr: { supersample: 0.7, supersampleManual: true, motionSmoothing: false },
  },
};
// Valor efetivo no SteamVR (ausente no arquivo = padrão dele).
const vrValor = (s, k) => k === "motionSmoothing" ? s?.motionSmoothing ?? true
  : k === "supersampleManual" ? !!s?.supersampleManual : s?.supersample ?? 1;
const perfilAtual = (s) => Object.keys(PERFIS).find((n) =>
  Object.entries(PERFIS[n].cfg).every(([k, v]) => config[k] === v) &&
  (!s || Object.entries(PERFIS[n].vr).every(([k, v]) =>
    typeof v === "number" ? Math.abs(vrValor(s, k) - v) < 0.011 : vrValor(s, k) === v)));
// Aplica um perfil; erro do SteamVR sobe para quem chamou.
async function aplicarPerfil(nome) {
  const p = PERFIS[nome];
  salvar(p.cfg);
  for (const [chave, valor] of Object.entries(p.vr)) await invoke("vr_set", { chave, valor });
}
$$("#perfis button").forEach((b) => (b.onclick = async () => {
  b.classList.add("ocupado");
  try {
    await aplicarPerfil(b.dataset.perfil);
    toast(`Perfil ${b.querySelector("b").textContent} aplicado`);
  } catch (e) {
    toast(String(e), true);
  } finally {
    b.classList.remove("ocupado");
    atualizar();
  }
}));

// ---------- VRChat: opções de inicialização (só montadas para copiar) e config.json ----------
let vrcInfo = null;
function argsVrchat() {
  const a = [];
  if (config["vrchat.prioridade"] !== "0") a.push(`--process-priority=${config["vrchat.prioridade"]}`);
  if (config["vrchat.thread"] !== "0") a.push(`--main-thread-priority=${config["vrchat.thread"]}`);
  // Primeira metade das threads: o primeiro CCX num Ryzen de dois (a doc pede o primeiro CCX).
  const metade = Math.floor((vrcInfo?.threads || 0) / 2);
  if (vrcInfo?.amd && config["vrchat.afinidade"] === "1" && metade > 0)
    a.push(`--affinity=${((1n << BigInt(metade)) - 1n).toString(16).toUpperCase()}`);
  return a.join(" ");
}
$("#btn-vrc-steam").onclick = (ev) => chamar("vrchat_opcoes_steam", { nossas: argsVrchat() }, ev.currentTarget);
$("#btn-vrc-copiar").onclick = async () => {
  try {
    await navigator.clipboard.writeText($("#vrc-args").value);
    toast("Copiado. Cole nas opções de inicialização do VRChat na Steam.");
  } catch (e) {
    toast("Não deu para copiar: selecione o texto e copie à mão.", true);
  }
};
// Gravações em andamento: o estado de 1 s não desfaz o clique enquanto elas não acabam.
let vrcGravando = 0;
const gravandoVrc = async (f) => { vrcGravando++; try { return await f(); } finally { vrcGravando--; } };
$("#vrc-particulas").onchange = (ev) => gravandoVrc(() => chamar("vrchat_particulas", { ligar: ev.target.checked }));
$("#btn-graficos").onclick = () => chamar("abrir_graficos_windows");
$$("#vrc-plano button").forEach((b) => (b.onclick = () => chamar("plano_energia_usar", { alto: b.dataset.alto === "1" }, b)));
$("#vr-home").onchange = (ev) => vrSet("home", ev.target.checked);

// Opções do jogo no registro (o backend recusa com o VRChat aberto e guarda o backup).
const vrcAjustar = (valores) => gravandoVrc(() => chamar("vrchat_ajustar", { valores }));
$$("#vrc-rank button").forEach((b) => (b.onclick = () => vrcAjustar({ rank: +b.dataset.valor })));
$$("[data-vrc]").forEach((el) => (el.onchange = () =>
  vrcAjustar({ [el.dataset.vrc]: +(el.checked ? el.dataset.sim : el.dataset.nao) })));
$("#vrc-max-ligado").onchange = (ev) => vrcAjustar({ maxLigado: ev.target.checked ? 1 : 0 });
$("#vrc-dist-ligada").onchange = (ev) =>
  vrcAjustar(ev.target.checked ? { distLigada: 1, distancia: +$("#vrc-dist").value } : { distLigada: 0 });
$("#vrc-dist").oninput = (ev) => {
  const el = ev.target;
  el._mexido = Date.now();
  texto($("#vrc-dist-valor"), el.value + " m");
  depois(el, () => vrcAjustar({ distancia: +el.value }), 300);
};
$("#vrc-max").oninput = (ev) => {
  const el = ev.target;
  el._mexido = Date.now();
  texto($("#vrc-max-valor"), el.value);
  depois(el, () => vrcAjustar({ maxAvatares: +el.value }), 300);
};
$("#btn-vrc-restaurar").onclick = (ev) => chamar("vrchat_restaurar", {}, ev.target);
// Modo leve: o que o VRChat recomenda para desempenho + o perfil Desempenho do ghostframe.
// O jogo primeiro (se o VRChat estiver aberto, nada muda); depois partículas e perfil,
// cada um por si: um erro num não esconde o que deu certo no outro.
let vrPode = false; // SteamVR ao vivo ou arquivo disponível (o perfil mexe nele)
const LEVE = { rank: 3, impostor: 1, maxLigado: 1, maxAvatares: 15, espelho: 2, sombras: 0, lod: 0, aa: 1 };
// Cada passo roda por si; no fim, o estado é lido de novo e cada item é conferido pelo que
// ficou gravado de verdade, não pelo que foi pedido.
$("#btn-vrc-leve").onclick = (ev) => gravandoVrc(async () => {
  const b = ev.currentTarget, erro = {};
  b.disabled = true;
  const passo = async (id, f) => { try { await f(); } catch (e) { erro[id] = String(e); } };
  try {
    await passo("jogo", () => invoke("vrchat_ajustar", { valores: LEVE }));
    await passo("particulas", () => invoke("vrchat_particulas", { ligar: true }));
    if (framegenDisponivel && vrPode) await passo("perfil", () => aplicarPerfil("desempenho"));
    else erro.perfil = framegenDisponivel ? "SteamVR indisponível agora" : "o proxy instalado não tem frame gen";
    await passo("energia", () => invoke("plano_energia_usar", { alto: true }));
    if (vrPode) await passo("home", () => invoke("vr_set", { chave: "home", valor: false }));
    else erro.home = "SteamVR indisponível agora";
    const args = argsVrchat();
    if (args) await passo("steam", () => invoke("vrchat_opcoes_steam", { nossas: args }));
    const e = await invoke("estado");
    const aj = e.vrchat?.jogo?.ajustes || {}, s = e.vr?.config ?? e.steamvr, c = e.config || {};
    const itens = [
      ["jogo", "Opções do jogo", Object.entries(LEVE).every(([k, v]) => aj[k] === v)],
      ["particulas", "Limitador de partículas", e.vrchat?.limitador === true],
      ["perfil", "Perfil Desempenho", c["framegen.ativo"] === "1" && c["framegen.modo"] === "manual" && c["framegen.multiplicador"] === "2"],
      ["energia", "Plano de energia Alto", e.vrchat?.energia?.alto === true],
      ["home", "SteamVR Home desligado", s?.home === false],
      ["steam", "Opções de inicialização na Steam", !args || (e.vrchat?.steam?.opcoes || "").split(" ").filter(Boolean).join(" ").endsWith(args)],
    ];
    html($("#vrc-conferencia"), itens.map(([id, nome, ok]) =>
      `<li class="${ok ? "ok" : "falta"}"><b>${ok ? "ok" : "falta"}</b> ${esc(nome)}${ok ? "" : erro[id] ? `: ${esc(erro[id])}` : ": não ficou gravado"}</li>`).join(""));
    $("#vrc-conferencia").hidden = false;
    const faltam = itens.filter((i) => !i[2]).length;
    toast(faltam ? `Otimizado, faltam ${faltam} itens (veja a lista)` : "Tudo otimizado e conferido.", faltam > 0);
  } finally {
    b.disabled = false;
    atualizar();
  }
});

// ---------- prévia do HUD: no máximo um pedido em voo, mais um na fila ----------
let previaOcupada = false, previaDeNovo = false;
async function previa() {
  if (previaOcupada) { previaDeNovo = true; return; }
  previaOcupada = true;
  try {
    const p = await invoke("hud_previa");
    const bin = atob(p.rgba), px = new Uint8ClampedArray(bin.length);
    for (let i = 0; i < bin.length; i++) px[i] = bin.charCodeAt(i);
    const c = $("#hud-previa");
    if (c.width !== p.largura || c.height !== p.altura) { c.width = p.largura; c.height = p.altura; }
    c.getContext("2d").putImageData(new ImageData(px, p.largura, p.altura), 0, 0);
  } catch (e) {
    console.error(e);
  }
  previaOcupada = false;
  if (previaDeNovo) { previaDeNovo = false; previa(); }
}

// ---------- instalação e log ----------
$("#btn-instalar").onclick = (e) => chamar("instalar", {}, e.currentTarget);
$("#btn-desinstalar").onclick = (e) => chamar("desinstalar", {}, e.currentTarget);
$("#btn-relatorio").onclick = (e) => chamar("relatorio", {}, e.currentTarget);
// O backend valida a pasta (e ajeita aspas, bin\\win64 ou o .dll colado) antes de salvar.
async function escolherAlvo(caminho, botao) {
  if (botao) botao.disabled = true;
  try {
    const r = await invoke("escolher_alvo", { caminho });
    config["app.alvo"] = r.alvo;
    $("#alvo-manual").value = "";
    toast(r.mensagem);
  } catch (e) {
    toast(String(e), true);
  } finally {
    if (botao) botao.disabled = false;
    $("#drivers")._html = null; // redesenha: o rádio clicado pode não ter valido
    atualizar();
  }
}
$("#drivers").onchange = (ev) => escolherAlvo(ev.target.value);
$("#btn-alvo").onclick = (ev) => { const v = $("#alvo-manual").value.trim(); if (v) escolherAlvo(v, ev.currentTarget); };
$("#alvo-manual").onkeydown = (ev) => { if (ev.key === "Enter") $("#btn-alvo").click(); };
$("#btn-pasta-driver").onclick = () => ultimo?.alvo && invoke("abrir_pasta", { caminho: ultimo.alvo.dll });
$("#btn-pasta-app").onclick = () => ultimo && invoke("abrir_pasta", { caminho: ultimo.pastaApp });
$("#btn-pasta-log").onclick = () => ultimo?.log && invoke("abrir_pasta", { caminho: ultimo.log.caminho });

// ---------- gráficos ----------
const CIANO = "#00f0ff", VIOLETA = "#a08fff", GERADO = "#7dffcf";
// Redesenha só quando o tamanho ou o último ponto mudam.
function grafico(canvas, series, { min = 0, max, guia } = {}) {
  const w = canvas.clientWidth, h = canvas.clientHeight;
  const chave = [w, h, guia, ...series.map((s) => s.pts.length + ":" + s.pts.at(-1)?.[0])].join();
  if (canvas._chave === chave) return;
  canvas._chave = chave;
  const dpr = devicePixelRatio || 1;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const g = canvas.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const esq = 26;
  g.font = "10px Consolas, monospace";
  g.fillStyle = "#4a6670";
  g.strokeStyle = "rgba(0,240,255,.07)";
  g.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = Math.round(6 + ((h - 12) * i) / 4) + 0.5;
    g.beginPath(); g.moveTo(esq, y); g.lineTo(w, y); g.stroke();
    g.fillText(Math.round(max - ((max - min) * i) / 4), 0, y + 3);
  }
  const Y = (v) => 6 + (1 - (Math.min(Math.max(v, min), max) - min) / (max - min)) * (h - 12);
  if (guia) {
    g.setLineDash([3, 3]); g.strokeStyle = "rgba(255,255,255,.3)";
    g.beginPath(); g.moveTo(esq, Y(guia)); g.lineTo(w, Y(guia)); g.stroke();
    g.setLineDash([]);
  }
  const pts = series.flatMap((s) => s.pts);
  if (!pts.length) {
    g.fillText("sem dados", esq + 12, h / 2);
    return;
  }
  const t0 = Math.min(...pts.map((p) => p[0])), t1 = Math.max(...pts.map((p) => p[0]), t0 + 1);
  const X = (t) => esq + ((t - t0) / (t1 - t0)) * (w - esq - 2);
  for (const s of series) {
    if (s.banda) {
      g.fillStyle = "rgba(0,240,255,.14)";
      g.beginPath();
      s.banda.forEach(([t, , hi], i) => (i ? g.lineTo(X(t), Y(hi)) : g.moveTo(X(t), Y(hi))));
      [...s.banda].reverse().forEach(([t, lo]) => g.lineTo(X(t), Y(lo)));
      g.fill();
    }
    if (!s.pts.length) continue;
    g.strokeStyle = s.cor; g.lineWidth = 1.5;
    g.beginPath();
    s.pts.forEach(([t, v], i) => (i ? g.lineTo(X(t), Y(v)) : g.moveTo(X(t), Y(v))));
    g.stroke();
  }
}

// fps do jogo vem só do valor atual (vr.timing): guardamos até 5 min aqui,
// no mesmo relógio do log quando ele existe, para o gráfico alinhar com o headset.
let serieJogo = [];
function guardarJogo(e, vivo) {
  const t = e.vr?.timing;
  if (!t) return;
  const agora = vivo ? vivo.t + e.log.idade : Date.now() / 1000;
  const ult = serieJogo.at(-1);
  if (ult && (agora < ult[0] || agora - ult[0] > 30)) serieJogo = []; // relógio mudou ou janela ficou oculta
  serieJogo.push([agora, t.fpsJogo]);
  if (serieJogo.length > 300) serieJogo.shift();
}

// ---------- render ----------
let ultimo = null;

const PROXY = {
  instalado: ["ok", "instalado"],
  original: ["", "não instalado"],
  sobrescrito: ["alerta", "removido por atualização"],
  quebrado: ["erro", "falta a DLL _real"],
  sem_dll: ["erro", "DLL não encontrada"],
};
// suporte do driver (src/core.rs, CONHECIDOS) -> [classe, rótulo]
const SUPORTE = {
  alvo: ["ok", "alvo principal"],
  streaming: ["alerta", "não testado"],
  nativo: ["alerta", "sem ganho"],
  desconhecido: ["alerta", "desconhecido"],
};
const suporte = (s) => SUPORTE[s] || SUPORTE.desconhecido;

function status(id, c, valor) {
  const el = $("#st-" + id);
  classe(el, c);
  texto(el.querySelector("p"), valor);
}

function render(e) {
  texto($("#versao"), "v" + e.versao);
  if (framegenDisponivel !== !!e.framegenDisponivel) {
    framegenDisponivel = !!e.framegenDisponivel;
    $("#fg-ativo").disabled = !framegenDisponivel;
    preencherConfig();
  }
  // Frame gen depende do proxy instalado ser uma versão que gera frames.
  $("#fg-aviso").hidden = framegenDisponivel;
  texto($("#fg-aviso"), e.alvo?.estado === "instalado"
    ? "O proxy instalado é de uma versão sem frame gen. Atualize na aba Instalação."
    : "Instale o proxy na aba Instalação para usar o frame gen.");
  classe($("#fg-tag"), "tag " + (framegenDisponivel ? "alerta" : ""));
  texto($("#fg-tag"), framegenDisponivel ? "experimental" : "indisponível");
  const vivo = e.log && e.log.idade < 6 ? e.log.dados : null;
  const stats = vivo?.stats || {};
  const presentComp = stats["DirectMode::Present"] || stats["VirtualDisplay::Present"] || [];
  // Com a trava, o proxy entrega os gerados sozinho (fora dos Present do compositor): soma.
  const serieFG = config["framegen.ativo"] === "1" ? stats["FRAMEGEN"] || [] : [];
  const extrasEm = (t) => {
    let x = 0;
    for (const [tf, gerados, trava] of serieFG) if (tf <= t + 1) x = trava > 0 ? gerados : 0;
    return x;
  };
  const present = presentComp.map((p) => [p[0], p[1] + extrasEm(p[0]), ...p.slice(2)]);
  const poses = stats["HMD TrackedDevicePoseUpdated"] || [];
  const estado = e.alvo?.estado;
  const vr = e.vr, timing = vr?.timing, hz = vr?.hz;
  guardarJogo(e, vivo);
  $("#logo").classList.toggle("vivo", estado === "instalado" && !!vivo?.hmd);

  // ----- painel -----
  const aberto = e.processos.steamvr;
  // Abriu um app mais novo (ou o Pico restaurou a DLL dele): atualiza o proxy sozinho,
  // uma vez por execução, com o SteamVR fechado (o Windows pode pedir permissão).
  if (!autoInstalou && !aberto && e.dllEmbutida && (e.alvo?.desatualizado || e.alvo?.estado === "sobrescrito")) {
    autoInstalou = true;
    toast(e.alvo.desatualizado ? "Atualizando o proxy do driver para esta versão..." : "O programa do headset restaurou a DLL dele: reinstalando o proxy...");
    chamar("instalar");
  }
  status("steamvr", aberto ? "ok" : "", aberto ? "rodando" : "fechado");
  if (vr) status("headset", "ok", `${vr.modelo} · ${fmt(vr.hz)} Hz`);
  else if (!aberto) status("headset", "", "--");
  else if (vivo?.hmd) status("headset", poses.length ? "ok" : "alerta", poses.length ? "conectado" : "sem tracking");
  else status("headset", "alerta", "não detectado");
  if (!e.alvo) status("driver", "erro", "nenhum");
  else {
    const [c, rotulo] = suporte(e.alvo.suporte);
    status("driver", c, e.alvo.suporte === "alvo" ? e.alvo.rotulo : `${e.alvo.rotulo} · ${rotulo}`);
  }
  // Instalado mas sem nenhum frame passando: o driver usa outro caminho de vídeo.
  const semFrames = estado === "instalado" && !!vivo?.semFrames && !presentComp.length;
  status("proxy", ...(!e.alvo ? ["erro", "sem alvo"] : semFrames ? ["alerta", "instalado, sem frames"] : PROXY[estado] || ["alerta", estado]));
  // Ligado/desligado pela bandeja: a janela acompanha (sem brigar com um clique recente).
  if (e.config?.["framegen.ativo"] !== config["framegen.ativo"] && Date.now() - editadoEm > 2500) {
    config["framegen.ativo"] = e.config["framegen.ativo"];
    preencherConfig();
  }
  const fpsAgora = present.at(-1)?.[1];
  texto($("#status-curto"), !aberto ? "SteamVR fechado"
    : fpsAgora != null && vivo ? `${fmt(fpsAgora)} fps no headset`
    : vr ? `${vr.modelo} conectado` : "SteamVR aberto");

  texto($("#m-fps"), fmt(present.at(-1)?.[1]));
  texto($("#m-jogo"), fmt(timing?.fpsJogo));
  texto($("#leg-gerados"), serieFG.length ? "gerados" : "");
  texto($("#m-ft"), fmt(present.at(-1)?.[2], 1));
  texto($("#m-cpugpu"), timing ? `${fmt(timing.msCpu, 1)} / ${fmt(timing.msGpu, 1)}` : "--");
  texto($("#m-reproj"), timing ? `${timing.reprojetados} / ${timing.perdidos}` : "--");
  texto($("#leg-jogo"), serieJogo.length ? "jogo" : "");
  texto($("#leg-fps-guia"), hz ? `${fmt(hz)} Hz` : "");
  texto($("#leg-ft-guia"), hz ? `${fmt(1000 / hz, 1)} ms` : "");
  if (abaAtiva("painel")) {
    grafico($("#graf-fps"), [
      { pts: present.map((p) => [p[0], p[1]]), cor: CIANO },
      { pts: serieJogo, cor: VIOLETA },
      { pts: serieFG.map((p) => [p[0], p[1]]), cor: GERADO },
    ], { max: Math.max(120, (hz || 0) + 20), guia: hz });
    grafico($("#graf-ft"), [
      { pts: present.map((p) => [p[0], p[2]]), banda: present.map((p) => [p[0], p[3], p[4]]), cor: CIANO },
    ], { max: 40, guia: hz && 1000 / hz });
  }

  // ----- desempenho -----
  const a = e.auto;
  texto($("#fg-alvo"), hz ? `${fmt(hz)} Hz` : "--");
  texto($("#fg-rec"), a ? a.recomendado + "×" : "--");
  texto($("#fg-motivo"), framegenDisponivel ? a?.motivo || "" : ""); // sem frame gen o aviso já explica
  const fgv = vivo?.framegen;
  // Ao vivo (o proxy atualiza a cada segundo).
  texto($("#fg-estado"), semFrames ? "sem frames" : !fgv ? "esperando" : { pronto: "gerando", esperando: "aquecendo", falhou: "falhou", desligado: "esperando" }[fgv.estado] || fgv.estado);
  texto($("#fg-reais"), fgv?.reais != null ? fmt(fgv.reais) : "--");
  texto($("#fg-motivo"), semFrames ? "O driver do headset não está passando frames pelo proxy. Veja o resumo na aba Log e mande o relatório." : $("#fg-motivo").textContent);
  texto($("#fg-gerados"), fgv?.gerados != null ? fmt(fgv.gerados) : "--");
  texto($("#fg-gpu"), fgv?.gpuMs >= 0 ? fmt(fgv.gpuMs, 1) : "--");
  // Por que não está gerando, com os números que o proxy manda a cada segundo.
  texto($("#fg-diag"), !fgv || fgv.gerados > 0 ? ""
    : fgv.estado === "falhou" ? "O motor desistiu. O motivo está na aba Log."
    : fgv.presents > 0 && !fgv.repetidos && !fgv.trava
      ? `Nada para gerar: ${fmt(fgv.presents)} quadros/s chegando, nenhum repetido e sem trava do SteamVR.`
    : fgv.repetidos > 0 ? `${fmt(fgv.repetidos)} quadros/s para trocar, nenhum gerado ainda (${fgv.estado}).` : "");

  // Ao vivo com o SteamVR conectado; pelo arquivo só com ele fechado (ele regrava ao sair).
  const s = vr?.config ?? e.steamvr;
  const pode = !!vr || (!aberto && !!e.steamvr);
  const fonte = vr ? ["ok", "ao vivo"] : pode ? ["", "arquivo"] : aberto ? ["alerta", "aguardando"] : ["erro", "sem config"];
  classe($("#vr-fonte"), "tag " + fonte[0]);
  texto($("#vr-fonte"), fonte[1]);

  // Primeira opção: o jogo aberto agora (padrão); as outras fixam um jogo.
  const aberto_ = vr?.jogo;
  html($("#jogo"),
    `<option value=""${config["app.jogo_fixo"] ? "" : " selected"}>Jogo aberto: ${aberto_ ? esc(aberto_.nome || aberto_.chave) : "nenhum agora"}</option>` +
    (e.jogos || []).map((j) =>
      `<option value="${esc(j.chave)}"${j.chave === config["app.jogo_fixo"] ? " selected" : ""}>${esc(j.nome)}</option>`).join(""));
  $("#jogo").disabled = false;

  // Com o frame gen ligado, a trava é dele (Auto decide; Manual segue o multiplicador).
  const doFG = framegenDisponivel && config["framegen.ativo"] === "1";
  $("#trava-auto").hidden = !doFG;
  $$("#trava button").forEach((b) => {
    b.classList.toggle("ativo", String(s?.trava) === b.dataset.valor);
    b.disabled = !pode || doFG;
    if (b.dataset.valor !== "0") texto(b.lastChild, hz ? `${fmt(hz / (+b.dataset.valor + 1))} fps` : "");
  });

  const ss = $("#ss"), manual = !!s?.supersampleManual;
  $("#ss-manual").checked = manual;
  $("#ss-manual").disabled = !pode;
  ss.disabled = !pode || !manual;
  if (!(Date.now() - (ss._mexido || 0) < 1500)) ss.value = Math.round((s?.supersample ?? 1) * 100);
  texto($("#ss-valor"), (manual ? ss.value + "%" : "auto") + (vr?.render ? ` · ${vr.render.join("×")}` : ""));
  $("#ms").checked = !!s && (s.motionSmoothing ?? true); // ausente no arquivo = padrão do SteamVR (ligado)
  $("#ms").disabled = !pode;

  const atual = perfilAtual(s);
  texto($("#perfil-tag"), atual ? "" : "personalizado");
  $$("#perfis button").forEach((b) => {
    b.classList.toggle("ativo", b.dataset.perfil === atual);
    b.disabled = !pode || (PERFIS[b.dataset.perfil].cfg["framegen.ativo"] === "1" && !framegenDisponivel);
  });

  // ----- VRChat -----
  vrcInfo = e.vrchat || null;
  $("#vrc-afinidade-linha").hidden = !vrcInfo?.amd;
  const args = argsVrchat();
  if ($("#vrc-args").value !== args) $("#vrc-args").value = args;
  $("#btn-vrc-copiar").disabled = !args;
  const st = vrcInfo?.steam;
  $("#btn-vrc-steam").disabled = !st || st.aberta || st.opcoes == null;
  texto($("#vrc-steam-agora"), !st || st.opcoes == null ? "Não achei a configuração da sua conta Steam."
    : (st.aberta ? "Feche a Steam para gravar. " : "") + `Na Steam agora: ${st.opcoes || "nenhuma opção"}`);
  const lim = vrcInfo?.limitador;
  if (!vrcGravando) $("#vrc-particulas").checked = !!lim;
  $("#vrc-particulas").disabled = !vrcInfo || lim == null;
  // HAGS: o VRChat recomenda desligado.
  const hags = vrcInfo?.hags;
  classe($("#vrc-hags"), "tag " + (hags ? "alerta" : hags === false ? "ok" : ""));
  texto($("#vrc-hags"), hags ? "ligado" : hags === false ? "desligado" : "");
  const en = vrcInfo?.energia;
  classe($("#vrc-energia"), "tag " + (en ? (en.alto ? "ok" : "") : ""));
  texto($("#vrc-energia"), en?.nome || "");
  $$("#vrc-plano button").forEach((b) => b.classList.toggle("ativo", !!en && en.alto === (b.dataset.alto === "1")));
  $("#vr-home").checked = !!s && (s.home ?? true); // ausente = padrão do SteamVR (ligado)
  $("#vr-home").disabled = !pode;
  vrPode = pode;

  // Opções do jogo (registro): só com o VRChat fechado.
  const jogo = vrcInfo?.jogo, aj = jogo?.ajustes || {};
  const fechado = !!jogo?.rodou && !vrcInfo.aberto;
  $("#vrc-aberto").hidden = !vrcInfo?.aberto;
  texto($("#vrc-jogo-tag"), !vrcInfo ? "" : !jogo?.rodou ? "abra o VRChat uma vez" : "");
  texto($("#vrc-erro"), vrcInfo?.erroConfig || "");
  $("#btn-vrc-leve").disabled = !fechado;
  $("#btn-vrc-restaurar").hidden = !jogo?.temBackup;
  $("#btn-vrc-restaurar").disabled = !fechado;
  $$("#vrc-rank button").forEach((b) => {
    b.classList.toggle("ativo", String(aj.rank) === b.dataset.valor);
    b.disabled = !fechado;
  });
  if (!vrcGravando) {
    $$("[data-vrc]").forEach((el) => (el.checked = String(aj[el.dataset.vrc]) === el.dataset.sim));
    $("#vrc-max-ligado").checked = aj.maxLigado === 1;
    $("#vrc-dist-ligada").checked = aj.distLigada === 1;
  }
  $$("[data-vrc], #vrc-max-ligado, #vrc-dist-ligada").forEach((el) => (el.disabled = !fechado));
  const dist = $("#vrc-dist");
  dist.disabled = !fechado || aj.distLigada !== 1;
  // Fora da faixa da interface (um VRChat novo grava 100): o slider começa no máximo dela.
  if (!(Date.now() - (dist._mexido || 0) < 1500) && aj.distancia != null) dist.value = Math.min(aj.distancia, 50);
  texto($("#vrc-dist-valor"), aj.distLigada === 1 ? dist.value + " m" : "");
  const mx = $("#vrc-max");
  mx.disabled = !fechado || aj.maxLigado !== 1;
  if (!(Date.now() - (mx._mexido || 0) < 1500) && aj.maxAvatares != null) mx.value = aj.maxAvatares;
  texto($("#vrc-max-valor"), aj.maxLigado === 1 ? String(mx.value) : "");

  // ----- HUD -----
  const erro = e.hud?.erro;
  const hudTag = config["hud.ativo"] !== "1" ? ["", ""] : erro ? ["erro", "erro"] : e.hud?.conectado ? ["ok", "no SteamVR"] : ["alerta", "aguardando SteamVR"];
  classe($("#hud-tag"), "tag " + hudTag[0]);
  texto($("#hud-tag"), hudTag[1]);
  $("#hud-tag").title = erro || "";
  texto($("#hud-erro"), erro || "");

  // ----- instalação -----
  // Marcado = o que o backend diz que está em uso (não o texto do ini), e "em uso" aparece
  // também no modo automático, para saber qual driver ele achou.
  const mesmo = (a, b) => !!a && !!b && a.replace(/[\\/]+$/, "").toLowerCase() === b.replace(/[\\/]+$/, "").toLowerCase();
  html($("#drivers"),
    `<label><input type="radio" name="alvo" value=""${e.alvoAuto ? " checked" : ""}><b>Automático</b>` +
    `<small>${e.alvoAuto ? (e.alvo ? `usando ${esc(e.alvo.rotulo)}` : "nenhum driver de headset encontrado") : ""}</small></label>` +
    e.drivers.map((dr) => {
      const emUso = mesmo(dr.raiz, e.alvo?.raiz);
      return `<label><input type="radio" name="alvo" value="${esc(dr.raiz)}"${emUso && !e.alvoAuto ? " checked" : ""}>
      <b>${esc(dr.rotulo)}</b>${tag(suporte(dr.suporte)[1], suporte(dr.suporte)[0])}${dr.embutido ? tag("SteamVR", "") : ""}${emUso ? tag("em uso", "ok") : ""}
      <small>${esc(dr.raiz)}</small></label>`;
    }).join("") +
    (e.drivers.length ? "" : `<p class="nota">Nenhum driver de headset encontrado. Abra o programa do headset e o SteamVR uma vez, ou cole a pasta do driver abaixo.</p>`));

  texto($("#alvo-uso"), e.alvo ? `Em uso: ${e.alvo.rotulo} · ${e.alvo.dll}` : "");
  const [pc, pr] = e.alvo ? PROXY[estado] || ["alerta", estado] : ["", ""];
  classe($("#proxy-tag"), "tag " + pc);
  texto($("#proxy-tag"), pr);
  texto($("#proxy-texto"), !e.alvo ? "Escolha o driver do headset."
    : estado === "sem_dll" ? `Não achei a DLL do driver em ${e.alvo.dll}. Confira a pasta ou escolha outro driver.`
    : aberto ? "Feche o SteamVR para instalar ou remover."
    : !e.dllEmbutida ? "Build sem a DLL embutida: use o .exe da release."
    : e.alvo.embutido && estado !== "instalado" ? "Driver do SteamVR: atualizações do SteamVR desfazem a instalação."
    : "");
  texto($("#btn-instalar"), estado === "instalado" ? "Atualizar" : estado === "sobrescrito" ? "Reinstalar" : "Instalar");
  $("#btn-instalar").disabled = aberto || !e.dllEmbutida || !["original", "instalado", "sobrescrito"].includes(estado);
  $("#btn-desinstalar").disabled = aberto || !["instalado", "sobrescrito"].includes(estado);
  $("#btn-pasta-driver").disabled = !e.alvo;

  // ----- log -----
  const d = e.log?.dados;
  texto($("#log-caminho"), e.log?.caminho || "");
  $("#btn-pasta-log").disabled = !e.log;
  const item = (k, v) => `<div><dt>${k}</dt><dd>${esc(v ?? "--")}</dd></div>`;
  html($("#sessao"), d ? item("Vídeo", d.video || (d.semFrames ? "nenhum frame passa pelo proxy" : null))
    + item("Componentes", d.componentes?.length ? d.componentes.join(", ") : null)
    + item("Interfaces", d.interfaces?.length ? d.interfaces.join(", ") : null)
    + item("Depth", d.depth == null ? null : d.depth ? "sim" : "não")
    + item("Motion smoothing", d.motionSmoothing) : "");
  html($("#avisos"), (d?.avisos || []).map((x) => `<li>${esc(x.trim())}</li>`).join(""));
  if (abaAtiva("log") && d?.linhas) {
    const t = $("#terminal");
    const noFim = t.scrollTop + t.clientHeight >= t.scrollHeight - 20;
    html(t, d.linhas.map((l) => {
      const c = l.includes("!!") ? "av" : l.includes("ERRO") ? "er" : l.includes("STATS") ? "st" : "";
      return c ? `<span class="${c}">${esc(l)}</span>` : esc(l);
    }).join("\n"));
    if (noFim) t.scrollTop = t.scrollHeight;
  }
}

// ---------- ciclo: 1 s, parado com a janela oculta/minimizada ----------
let primeira = true;
async function atualizar() {
  if (document.hidden) return;
  try {
    const e = await invoke("estado");
    if (primeira) { config = e.config; preencherConfig(); primeira = false; }
    ultimo = e;
    render(e);
  } catch (err) {
    console.error(err);
  }
}
setInterval(atualizar, 1000);
document.addEventListener("visibilitychange", atualizar);
addEventListener("resize", () => ultimo && render(ultimo));
atualizar();
