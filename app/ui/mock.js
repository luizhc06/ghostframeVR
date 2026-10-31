// Backend falso para abrir a UI num navegador comum (sem Tauri) e ajustar o visual.
// Dentro do app o __TAURI__ já existe e este arquivo não faz nada.
// Variações: index.html?estado=original | sobrescrito | offline | sem-alvo
if (!window.__TAURI__) {
  const modo = new URLSearchParams(location.search).get("estado") || "instalado";
  const t0 = Date.now() / 1000;
  const cfg = {
    "framegen.ativo": "1", "framegen.modo": "auto", "framegen.multiplicador": "2",
    
    "hud.ativo": "1", "hud.ancora": "pe", "hud.tamanho": "12", "hud.opacidade": "80",
    "hud.x": "0", "hud.y": "4", "hud.z": "0", "hud.itens": "fps,frames,ft,graf",
    "app.autostart": "0", "hud.olhar": "1", "app.alvo": "", "app.jogo": "steam.app.438100",
  };
  const vrCfg = { supersample: 1.0, supersampleManual: false, motionSmoothing: false, trava: 0 };
  const aberto = modo !== "offline";
  const conectado = aberto && modo !== "sem-alvo";
  const raiz = "C:\\Program Files\\PICO Connect\\resources\\driver\\pico";
  const steamvr = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\drivers";
  const relogio = () => Date.now() / 1000 - t0 + 300;
  const serie = (f) => {
    const agora = relogio(), pts = [];
    for (let t = Math.max(0, agora - 300); t <= agora; t += 2) pts.push(f(t));
    return pts;
  };
  const queda = (t) => (Math.sin(t / 23) > 0.93 ? 22 : 0);

  // Imagem RGBA do HUD, do jeito que o backend devolve (base64 dos pixels crus).
  function hudPrevia() {
    const itens = cfg["hud.itens"].split(",").filter(Boolean);
    const linhas = itens.filter((i) => i !== "graf");
    const W = 360, L = 44, G = itens.includes("graf") ? 84 : 0;
    const H = Math.max(40, 16 + linhas.length * L + G + 8);
    const c = document.createElement("canvas");
    c.width = W; c.height = H;
    const g = c.getContext("2d");
    const a = +cfg["hud.opacidade"] / 100;
    g.fillStyle = `rgba(3,12,16,${0.85 * a})`;
    g.strokeStyle = `rgba(0,240,255,${a})`;
    g.lineWidth = 2;
    g.beginPath(); g.roundRect(1, 1, W - 2, H - 2, 10); g.fill(); g.stroke();
    const fps = 90 - Math.random() * 2;
    const valores = {
      fps: ["FPS", fps.toFixed(0)], frames: ["FRAMES", "45 + 45"], ft: ["FRAME TIME", (1000 / fps).toFixed(1) + " ms"],
      lat: ["LATÊNCIA", "38 ms"], gpu: ["GPU", "9.3 ms"],
    };
    g.textBaseline = "middle";
    linhas.forEach((k, i) => {
      const y = 16 + i * L + L / 2;
      g.font = "600 15px monospace"; g.fillStyle = `rgba(120,170,180,${a})`; g.fillText(valores[k][0], 20, y);
      g.font = "700 24px monospace"; g.fillStyle = `rgba(255,255,255,${a})`;
      g.fillText(valores[k][1], W - 20 - g.measureText(valores[k][1]).width, y);
    });
    if (G) {
      const y0 = 16 + linhas.length * L + 8;
      g.strokeStyle = `rgba(0,240,255,${a})`;
      g.beginPath();
      for (let x = 0; x <= W - 40; x += 8) g.lineTo(20 + x, y0 + 20 + Math.random() * 10 + (x > 200 && x < 240 ? 30 : 0));
      g.stroke();
    }
    const d = g.getImageData(0, 0, W, H).data;
    let s = "";
    for (let i = 0; i < d.length; i += 0x8000) s += String.fromCharCode(...d.subarray(i, i + 0x8000));
    return { largura: W, altura: H, rgba: btoa(s) };
  }

  window.__TAURI__ = {
    window: { getCurrentWindow: () => ({ minimize() {}, toggleMaximize() {}, close() {} }) },
    core: {
      invoke: async (cmd, a) => {
        if (cmd === "salvar_config") { Object.assign(cfg, a.valores); return; }
        if (cmd === "escolher_alvo") {
          // Como o backend: tira aspas e sobe do .dll / bin\\win64 até a pasta do driver.
          const c = (a.caminho || "").trim().replace(/^"|"$/g, "").replace(/[\\/]+$/, "")
            .replace(/[\\/]driver_[^\\/]*\.dll$/i, "").replace(/[\\/]bin[\\/]win64$/i, "");
          if (c && !/pico|alvr/i.test(c)) throw `Essa pasta não tem um driver do SteamVR (falta bin\\win64\\driver_*.dll): ${c}`;
          cfg["app.alvo"] = c;
          return { alvo: c, mensagem: `Driver do headset: ${/alvr/i.test(c) ? "ALVR" : "Pico Connect"} (${c || raiz})` };
        }
        if (cmd === "vr_set") {
          if (aberto && !conectado) throw "SteamVR aberto, mas ainda não conectou";
          vrCfg[a.chave] = a.valor;
          return;
        }
        if (cmd === "hud_previa") return hudPrevia();
        if (cmd === "instalar") return "proxy instalado. Backup em C:\\Users\\voce\\AppData\\Local\\ghostframeVR\\backup\\win64-1793476800";
        if (cmd === "desinstalar") throw "feche o SteamVR antes: a DLL do driver fica travada enquanto ele roda";
        if (cmd === "relatorio") return "C:\\Users\\voce\\AppData\\Local\\ghostframeVR\\relatorio-1793476800";
        if (cmd !== "estado") return;
        const on = modo === "instalado";
        const fgOn = on && cfg["framegen.ativo"] === "1";
        const present = serie((t) => {
          const fps = 90 - queda(t) - Math.random() * 1.5;
          return [t, fps, 1000 / fps, 9.5 + Math.random(), 1000 / fps + 3 + Math.random() * 5 + queda(t) / 2];
        });
        const poses = serie((t) => [t, 500 + Math.random() * 4]);
        const escala = Math.sqrt(vrCfg.supersampleManual ? vrCfg.supersample : 1);
        return {
          framegenDisponivel: on, alvoAuto: !cfg["app.alvo"], versao: "0.2.0-alpha", dllEmbutida: true, pastaApp: "C:\\Users\\voce\\AppData\\Local\\ghostframeVR",
          processos: { steamvr: aberto },
          drivers: modo === "sem-alvo" ? [] : (cfg["app.alvo"] && !/PICO Connect\\resources/.test(cfg["app.alvo"]) ? [{ nome: "pico", rotulo: "Pico Connect", suporte: "alvo", embutido: false, raiz: cfg["app.alvo"], dll: "" }] : []).concat([
            { nome: "pico", rotulo: "Pico Connect", suporte: "alvo", embutido: false, raiz, dll: raiz + "\\bin\\win64\\driver_pico.dll" },
            { nome: "alvr_server", rotulo: "ALVR", suporte: "streaming", embutido: false, raiz: "C:\\ALVR", dll: "C:\\ALVR\\bin\\win64\\driver_alvr_server.dll" },
            { nome: "vrlink", rotulo: "Steam Link", suporte: "streaming", embutido: true, raiz: steamvr + "\\vrlink", dll: "" },
          ]),
          alvo: modo === "sem-alvo" ? null : { nome: "pico", rotulo: "Pico Connect", suporte: "alvo", embutido: false, raiz: cfg["app.alvo"] || raiz, dll: (cfg["app.alvo"] || raiz) + "\\bin\\win64\\driver_pico.dll", estado: on || modo === "offline" ? "instalado" : modo },
          log: on ? {
            caminho: "C:\\Users\\voce\\AppData\\Local\\Temp\\ghostframe.log", idade: 0.8,
            dados: {
              pid: "8812", t: relogio(), hmd: true, video: "DirectMode", depth: true, motionSmoothing: "off", render: "2160x2160",
              avisos: ["  !! versão diferente da que conhecemos (IVRVirtualDisplay_002 / IVRDriverDirectModeComponent_009)"],
              stats: { "DirectMode::Present": fgOn ? present.map((p) => [p[0], p[1] / 2, ...p.slice(2)]) : present,
                "HMD TrackedDevicePoseUpdated": poses, ...(fgOn ? { FRAMEGEN: serie((t) => [t, 44 + Math.random(), 1]) } : {}) },
              framegen: fgOn ? { estado: "pronto", reais: 45, gerados: 44.6, falhas: 0, trava: 1, gpuMs: 1.4 } : null,
              linhas: [
                "    0.002  ghostframe proxy iniciado. DLL própria: C:\\...\\driver_pico.dll",
                "    0.004  Carregando driver real: C:\\...\\driver_pico_real.dll",
                "    0.210  Provider::Init -> 0",
                "    3.118  Host::TrackedDeviceAdded serial=PA7L10MGH class=HMD",
                "    3.120  Device(HMD)::GetComponent(\"IVRDriverDirectModeComponent_009\") -> IMPLEMENTADO",
                "    3.120    -> embrulhando IVRDriverDirectModeComponent",
                "    3.121    !! versão diferente da que conhecemos (IVRVirtualDisplay_002 / IVRDriverDirectModeComponent_009)",
                "  300.400  STATS DirectMode::Present            89.8/s  dt(ms) avg=11.14 min=9.80 max=17.40",
                "  300.410  STATS HMD TrackedDevicePoseUpdated  501.2/s  dt(ms) avg=2.00 min=1.10 max=3.80",
              ],
            },
          } : null,
          config: { ...cfg },
          steamvr: modo === "sem-alvo" ? null : { ...vrCfg },
          jogos: modo === "sem-alvo" ? [] : [{ chave: "steam.app.438100", nome: "VRChat" }, { chave: "steam.app.620980", nome: "Beat Saber" }, { chave: "system.generated.jogo.exe", nome: "jogo.exe" }],
          vr: conectado ? {
            hz: 90, render: [Math.round(2160 * escala), Math.round(2160 * escala)], modelo: "PICO 4", fabricante: "PICO",
            timing: modo === "sobrescrito" ? null : {
              fpsJogo: 44 + Math.random() * 1.6, msCpu: 12.1 + Math.random(), msGpu: 9.3 + Math.random() * 0.6,
              msTrabalho: 21.0, reprojetados: Math.floor(Math.random() * 4), perdidos: 0,
            },
            config: { ...vrCfg },
          } : null,
          auto: conectado
            ? { recomendado: 2, multiplicador: 1, trava: 0, motivo: "frame gen indisponível" }
            : { recomendado: 1, multiplicador: 1, trava: 0, motivo: aberto ? "headset não conectado" : "SteamVR fechado" },
          hud: { conectado: conectado && cfg["hud.ativo"] === "1" },
        };
      },
    },
  };
}
