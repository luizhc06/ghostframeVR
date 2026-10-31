// UI do app de controle. Toda a lógica pesada está no Rust (src/core.rs);
// aqui só desenhamos o estado que o comando `estado` devolve a cada segundo.
const { invoke } = window.__TAURI__.core;
const janela = window.__TAURI__.window.getCurrentWindow();
const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];

// ---------- fantasminha (mesmo desenho do ícone: icons/make_icon.py) ----------
const GHOST = [
  "................",
  ".....######.....",
  "...##########...",
  "..############..",
  ".##############.",
  ".#KKKKKKKKKKKK#.",
  ".#KwKKKKKKwKKK#.",
  ".#KKKKKKKKKKKK#.",
  ".##############.",
  ".##############.",
  ".##############.",
  ".##############.",
  ".##############.",
  ".##.###..###.##.",
  ".#...#....#...#.",
  "................",
];
const COR = { "#": "#00f0ff", K: "#02080c", w: "#ffffff" };
function ghostSvg(px) {
  let r = "";
  GHOST.forEach((linha, y) =>
    [...linha].forEach((c, x) => { if (COR[c]) r += `<rect x="${x}" y="${y}" width="1" height="1" fill="${COR[c]}"/>`; }));
  return `<svg width="${16 * px}" height="${16 * px}" viewBox="0 0 16 16">${r}</svg>`;
}
$$("[data-ghost]").forEach((el) => (el.innerHTML = ghostSvg(+el.dataset.ghost)));

// ---------- janela e abas ----------
$("#btn-min").onclick = () => janela.minimize();
$("#btn-max").onclick = () => janela.toggleMaximize();
$("#btn-fechar").onclick = () => janela.close();
$$(".menu button[data-aba]").forEach((b) => (b.onclick = () => {
  $$(".menu button[data-aba]").forEach((x) => x.classList.toggle("ativa", x === b));
  $$(".aba").forEach((a) => a.classList.toggle("ativa", a.id === "aba-" + b.dataset.aba));
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
let config = {};
const salvar = (valores) => {
  Object.assign(config, valores);
  return chamar("salvar_config", { valores });
};

$$("input[type=checkbox][data-chave]").forEach((el) =>
  (el.onchange = () => salvar({ [el.dataset.chave]: el.checked ? "1" : "0" })));
$$("input[type=number][data-chave]").forEach((el) =>
  (el.onchange = () => salvar({ [el.dataset.chave]: String(el.value) })));
$$(".seg[data-chave] button").forEach((b) => (b.onclick = () => {
  salvar({ [b.parentElement.dataset.chave]: b.dataset.valor });
  preencherConfig();
}));

function preencherConfig() {
  $$("input[type=checkbox][data-chave]").forEach((el) => (el.checked = config[el.dataset.chave] === "1"));
  $$("input[type=number][data-chave]").forEach((el) => { if (document.activeElement !== el) el.value = config[el.dataset.chave]; });
  $$(".seg[data-chave]").forEach((g) =>
    [...g.children].forEach((b) => b.classList.toggle("ativo", b.dataset.valor === config[g.dataset.chave])));
  cadencia();
}

// Cadência: R = frame do jogo, S = sintético. Anima numa velocidade de leitura, não na real.
let passo = 0;
function cadencia() {
  const n = +config["framegen.multiplicador"] || 2;
  const ligado = config["framegen.ativo"] === "1";
  const el = $("#cadencia");
  if (el.children.length !== 24) el.innerHTML = "<i></i>".repeat(24);
  [...el.children].forEach((c, i) => {
    const real = !ligado || i % n === 0;
    c.className = (real ? "r" : "s") + (i === passo % 24 ? " agora" : "");
  });
  $("#cadencia-texto").textContent = ligado
    ? n === 2 ? "72 Hz = 36 reais + 36 sintéticos" : "90 Hz = 30 reais + 60 sintéticos"
    : "desligado: só frames reais";
}
setInterval(() => { passo++; if ($("#aba-framegen").classList.contains("ativa")) cadencia(); }, 180);

// ---------- otimização ----------
$$("#trava button").forEach((b) => (b.onclick = () => chamar("trava_vrchat", { frames: +b.dataset.valor })));

// ---------- instalação ----------
$("#btn-instalar").onclick = (e) => chamar("instalar", {}, e.currentTarget);
$("#btn-desinstalar").onclick = (e) => chamar("desinstalar", {}, e.currentTarget);
$("#btn-relatorio").onclick = (e) => chamar("relatorio", {}, e.currentTarget);
$("#btn-alvo").onclick = () => { const v = $("#alvo-manual").value.trim(); if (v) salvar({ "app.alvo": v }); };
$("#btn-pasta-driver").onclick = () => ultimo?.alvo && invoke("abrir_pasta", { caminho: ultimo.alvo.dll });
$("#btn-pasta-app").onclick = () => ultimo && invoke("abrir_pasta", { caminho: ultimo.pastaApp });
$("#btn-pasta-log").onclick = () => ultimo?.log && invoke("abrir_pasta", { caminho: ultimo.log.caminho });

// ---------- gráficos ----------
function grafico(canvas, series, { min = 0, max, guia } = {}) {
  const dpr = devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== w * dpr) { canvas.width = w * dpr; canvas.height = h * dpr; }
  const g = canvas.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  const pts = series.flatMap((s) => s.pts);
  const esq = 30;
  // grade
  g.font = "10px Consolas, monospace";
  g.fillStyle = "#3d5c66";
  g.strokeStyle = "rgba(0,240,255,.08)";
  g.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = 6 + ((h - 12) * i) / 4;
    g.beginPath(); g.moveTo(esq, y); g.lineTo(w, y); g.stroke();
    g.fillText(Math.round(max - ((max - min) * i) / 4), 0, y + 3);
  }
  if (!pts.length) {
    g.fillStyle = "#5b7c86";
    g.fillText("sem dados: instale o proxy e abra o SteamVR", esq + 12, h / 2);
    return;
  }
  const t0 = Math.min(...pts.map((p) => p[0])), t1 = Math.max(...pts.map((p) => p[0]), t0 + 1);
  const X = (t) => esq + ((t - t0) / (t1 - t0)) * (w - esq - 4);
  const Y = (v) => 6 + (1 - (Math.min(Math.max(v, min), max) - min) / (max - min)) * (h - 12);
  if (guia != null) {
    g.setLineDash([4, 4]); g.strokeStyle = "#ffd23f"; g.globalAlpha = .7;
    g.beginPath(); g.moveTo(esq, Y(guia)); g.lineTo(w, Y(guia)); g.stroke();
    g.setLineDash([]); g.globalAlpha = 1;
  }
  for (const s of series) {
    if (s.banda) {
      g.fillStyle = "rgba(0,240,255,.16)";
      g.beginPath();
      s.banda.forEach(([t, , hi], i) => (i ? g.lineTo(X(t), Y(hi)) : g.moveTo(X(t), Y(hi))));
      [...s.banda].reverse().forEach(([t, lo]) => g.lineTo(X(t), Y(lo)));
      g.fill();
    }
    if (!s.pts.length) continue;
    g.strokeStyle = s.cor; g.lineWidth = 2; g.shadowColor = s.cor; g.shadowBlur = s.brilho ? 10 : 0;
    g.beginPath();
    s.pts.forEach(([t, v], i) => (i ? g.lineTo(X(t), Y(v)) : g.moveTo(X(t), Y(v))));
    g.stroke();
    g.shadowBlur = 0;
    if (s.area) {
      const grad = g.createLinearGradient(0, 0, 0, h);
      grad.addColorStop(0, "rgba(0,240,255,.22)"); grad.addColorStop(1, "rgba(0,240,255,0)");
      g.lineTo(X(s.pts.at(-1)[0]), h); g.lineTo(X(s.pts[0][0]), h); g.closePath();
      g.fillStyle = grad; g.fill();
    }
  }
}

// ---------- render ----------
let ultimo = null;
const ultimoValor = (serie, i = 1) => (serie && serie.length ? serie.at(-1)[i] : null);
const fmt = (v, d = 0) => (v == null ? "--" : v.toFixed(d));
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

function card(id, classe, texto) {
  const el = $("#st-" + id);
  el.className = "card " + classe;
  el.querySelector("p").textContent = texto;
}

const PROXY = {
  instalado: ["ok", "instalado"],
  original: ["", "não instalado"],
  sobrescrito: ["alerta", "removido por atualização"],
  quebrado: ["erro", "falta a DLL _real"],
  sem_dll: ["erro", "DLL do driver não encontrada"],
};

function render(e) {
  $("#versao").textContent = "v" + e.versao;
  const vivo = e.log && e.log.idade < 6 && e.log.dados ? e.log.dados : null;
  const stats = vivo?.stats || {};
  const present = stats["DirectMode::Present"] || stats["VirtualDisplay::Present"] || [];
  const poses = stats["HMD TrackedDevicePoseUpdated"] || [];
  const fps = ultimoValor(present), hz = ultimoValor(poses), ft = ultimoValor(present, 2);
  const estado = e.alvo?.estado;

  // status
  card("steamvr", e.processos.steamvr ? "ok" : "", e.processos.steamvr ? "rodando" : "fechado");
  card("pico", e.processos.pico ? "ok" : "", e.processos.pico ? "aberto" : "fechado");
  if (!e.processos.steamvr) card("headset", "", "aguardando o SteamVR");
  else if (estado !== "instalado") card("headset", "alerta", "instale o proxy para detectar");
  else if (vivo?.hmd && hz > 0) card("headset", "ok", `conectado · ${fmt(hz)} Hz tracking`);
  else if (vivo?.hmd) card("headset", "alerta", "registrado, sem tracking");
  else card("headset", "alerta", "não detectado");
  if (!e.alvo) card("proxy", "erro", "alvo não encontrado");
  else card("proxy", ...(PROXY[estado] || ["", estado]));

  const tudoOk = e.processos.steamvr && vivo?.hmd && estado === "instalado";
  $("#led-sys").className = "led " + (tudoOk ? "ok" : e.alvo ? "alerta" : "erro");
  $("#sys-texto").textContent = tudoOk ? "SYS//STREAMING" : e.processos.steamvr ? "SYS//STANDBY" : "SYS//IDLE";
  $("#fala").textContent =
    !e.alvo ? "não achei o Pico Connect... me mostra onde ele mora? (aba instalação)"
    : estado === "sobrescrito" ? "o Pico Connect se atualizou e me expulsou. reinstala?"
    : estado !== "instalado" ? "boo. me instala no driver pra eu ver os frames."
    : tudoOk && fps ? `${fmt(fps)} frames/s passando por mim. boo!`
    : e.processos.steamvr ? "SteamVR aberto. cadê o headset?"
    : "tudo calmo. abre o Pico Connect e o SteamVR.";

  // métricas e gráficos (só desenha a aba visível)
  $("#m-fps").textContent = fmt(fps);
  $("#m-pose").textContent = fmt(hz);
  $("#m-ft").textContent = fmt(ft, 1);
  $("#m-ft-sub").textContent = ft == null ? "ms · média" : `ms · média (mín ${fmt(ultimoValor(present, 3), 1)} / máx ${fmt(ultimoValor(present, 4), 1)})`;
  if ($("#aba-painel").classList.contains("ativa")) {
    grafico($("#graf-fps"), [
      { pts: poses.map((p) => [p[0], p[1] / 10]), cor: "#2a7fff" },
      { pts: present.map((p) => [p[0], p[1]]), cor: "#00f0ff", brilho: true, area: true },
    ], { max: 120, guia: 72 });
    grafico($("#graf-ft"), [
      { pts: present.map((p) => [p[0], p[2]]), banda: present.map((p) => [p[0], p[3], p[4]]), cor: "#00f0ff", brilho: true },
    ], { max: 50, guia: 1000 / 72 });
  }

  const tag = (txt, c) => `<span class="tag ${c}">${esc(txt)}</span>`;
  const d = e.log?.dados;
  $("#sessao").innerHTML = `
    <dt>caminho de vídeo</dt><dd>${d?.video ? esc(d.video) : "—"}</dd>
    <dt>depth do jogo</dt><dd>${d?.depth == null ? "—" : d.depth ? "sim" + tag("frame gen melhor", "ok") : "não" + tag("só cor", "alerta")}</dd>
    <dt>motion smoothing</dt><dd>${d?.motionSmoothing ? esc(d.motionSmoothing) : "—"}</dd>
    <dt>resolução por olho</dt><dd>${d?.render ? esc(d.render) : "—"}</dd>
    <dt>VRChat</dt><dd>${e.processos.vrchat ? "aberto" : "fechado"}</dd>
    <dt>avisos</dt><dd>${d?.avisos?.length ? tag(d.avisos.length + " no log", "alerta") : "nenhum"}</dd>`;

  // otimização
  const s = e.steamvr;
  $$("#trava button").forEach((b) => {
    b.classList.toggle("ativo", s && String(s.trava) === b.dataset.valor);
    b.disabled = !s || e.processos.steamvr;
  });
  $("#trava-nota").innerHTML = !s ? "Não achei o <code>steamvr.vrsettings</code>."
    : e.processos.steamvr ? "<b>Feche o SteamVR para mudar</b>: ele regrava o arquivo ao sair."
    : "Backup em <code>steamvr.vrsettings.ghostframe.bak</code> antes de cada mudança.";
  $("#steamvr-info").innerHTML = s ? `
    <dt>motion smoothing</dt><dd>${s.motionSmoothing == null ? "padrão" : s.motionSmoothing ? "ligado" : "desligado"}</dd>
    <dt>supersampling</dt><dd>${s.supersampleManual ? Math.round((s.supersample ?? 1) * 100) + "% (manual)" : "automático"}</dd>
    <dt>trava VRChat</dt><dd>${["sem trava", "1/2 da taxa", "1/3 da taxa"][s.trava] ?? s.trava}</dd>`
    : "<dt>steamvr</dt><dd>config não encontrada</dd>";

  // instalação
  const lista = JSON.stringify([e.drivers, config["app.alvo"]]);
  if ($("#drivers").dataset.lista !== lista) {
    $("#drivers").dataset.lista = lista;
    const auto = !config["app.alvo"];
    $("#drivers").innerHTML =
      `<label><input type="radio" name="alvo" value="" ${auto ? "checked" : ""}><b>AUTO</b><small>primeiro driver com "pico" no nome</small></label>` +
      e.drivers.map((dr) => `<label><input type="radio" name="alvo" value="${esc(dr.raiz)}" ${config["app.alvo"] === dr.raiz ? "checked" : ""}>
        <b>${esc(dr.nome)}</b><small>${esc(dr.raiz)}</small></label>`).join("") +
      (e.drivers.length ? "" : `<small class="nota">Nenhum driver externo em openvrpaths.vrpath.</small>`);
    $$("#drivers input").forEach((r) => (r.onchange = () => salvar({ "app.alvo": r.value })));
  }
  $("#alvo-info").innerHTML = e.alvo ? `
    <dt>driver</dt><dd>${esc(e.alvo.nome)}</dd>
    <dt>dll</dt><dd>${esc(e.alvo.dll)}</dd>
    <dt>estado</dt><dd>${tag((PROXY[estado] || ["", estado])[1], (PROXY[estado] || ["erro"])[0] || "alerta")}</dd>` : "";
  $("#proxy-texto").textContent = !e.alvo ? "Escolha o driver alvo acima."
    : e.processos.steamvr ? "Feche o SteamVR para instalar ou desinstalar: a DLL fica travada enquanto ele roda."
    : !e.dllEmbutida ? "Este build não tem a DLL do proxy embutida. Use o .exe da release."
    : { instalado: "Instalado. INSTALAR de novo atualiza a DLL para a versão deste app.",
        sobrescrito: "O Pico Connect se atualizou e restaurou a DLL original. Reinstale.",
        original: "Driver original. O proxy ainda não está instalado.",
      }[estado] || "Não dá para instalar neste estado.";
  $("#btn-instalar").textContent = estado === "instalado" ? "ATUALIZAR" : estado === "sobrescrito" ? "REINSTALAR" : "INSTALAR";
  $("#btn-instalar").disabled = e.processos.steamvr || !e.dllEmbutida || !["original", "instalado", "sobrescrito"].includes(estado);
  $("#btn-desinstalar").disabled = e.processos.steamvr || !["instalado", "sobrescrito"].includes(estado);

  // log
  $("#log-caminho").textContent = e.log ? e.log.caminho : "";
  $("#avisos").innerHTML = (d?.avisos || []).map((a) => `<li>${esc(a)}</li>`).join("");
  if ($("#aba-log").classList.contains("ativa") && d?.linhas) {
    const t = $("#terminal");
    const noFim = t.scrollTop + t.clientHeight >= t.scrollHeight - 20;
    t.innerHTML = d.linhas.map((l) => {
      const c = l.includes("!!") ? "av" : l.includes("ERRO") ? "er" : l.includes("STATS") ? "st" : "";
      return c ? `<span class="${c}">${esc(l)}</span>` : esc(l);
    }).join("\n");
    if (noFim) t.scrollTop = t.scrollHeight;
  }
}

let primeira = true;
async function atualizar() {
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
atualizar();

// ---------- rodapé ----------
$("#ticker").textContent = [
  "GHOSTFRAME//VR", "extrapolação, não interpolação: zero frame de atraso", "nunca toca no processo do jogo",
  "tudo roda dentro do vrserver.exe", "36 → 72 fps", "boo", "frames reais + frames fantasmas",
].join("   ✦   ");
setInterval(() => ($("#relogio").textContent = new Date().toLocaleTimeString("pt-BR")), 1000);
