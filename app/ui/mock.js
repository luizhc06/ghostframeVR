// Backend falso para abrir a UI num navegador comum (sem Tauri) e ajustar o visual.
// Dentro do app o __TAURI__ já existe e este arquivo não faz nada.
// Variações: index.html?estado=original | sobrescrito | offline | sem-alvo
if (!window.__TAURI__) {
  const modo = new URLSearchParams(location.search).get("estado") || "instalado";
  const t0 = Date.now() / 1000;
  let cfg = {
    "framegen.ativo": "0", "framegen.multiplicador": "2", "framegen.qualidade": "1", "framegen.correcao_pose": "1",
    "hud.ativo": "0", "gravacao.ativo": "0", "gravacao.segundos": "5", "app.alvo": "", "app.jogo": "steam.app.438100",
  };
  let trava = 0;
  const raiz = "C:\\Program Files\\PICO Connect\\resources\\driver\\pico";
  const steamvr = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\drivers";
  const serie = (f) => {
    const agora = Date.now() / 1000 - t0 + 300;
    const pts = [];
    for (let t = Math.max(0, agora - 300); t <= agora; t += 2) pts.push(f(t));
    return pts;
  };
  const queda = (t) => (Math.sin(t / 23) > 0.93 ? 18 : 0);
  window.__TAURI__ = {
    window: { getCurrentWindow: () => ({ minimize() {}, toggleMaximize() {}, close() {} }) },
    core: {
      invoke: async (cmd, a) => {
        if (cmd === "salvar_config") { Object.assign(cfg, a.valores); return; }
        if (cmd === "trava_fps") { trava = a.frames; return; }
        if (cmd === "instalar") return "proxy instalado. Backup em C:\\Users\\voce\\AppData\\Local\\ghostframeVR\\backup\\win64-1793476800";
        if (cmd === "desinstalar") throw "feche o SteamVR antes: a DLL do driver fica travada enquanto ele roda";
        if (cmd === "relatorio") return "C:\\Users\\voce\\AppData\\Local\\ghostframeVR\\relatorio-1793476800";
        if (cmd !== "estado") return;
        const on = modo === "instalado";
        const present = serie((t) => {
          const fps = 72 - queda(t) - Math.random() * 1.5;
          return [t, fps, 1000 / fps, 11 + Math.random(), 1000 / fps + 4 + Math.random() * 6 + queda(t)];
        });
        const poses = serie((t) => [t, 500 + Math.random() * 4]);
        return {
          versao: "0.1.2-alpha", dllEmbutida: true, pastaApp: "C:\\Users\\voce\\AppData\\Local\\ghostframeVR",
          processos: { steamvr: modo !== "offline" },
          drivers: modo === "sem-alvo" ? [] : [
            { nome: "pico", rotulo: "Pico Connect", suporte: "alvo", embutido: false, raiz, dll: raiz + "\\bin\\win64\\driver_pico.dll" },
            { nome: "alvr_server", rotulo: "ALVR", suporte: "streaming", embutido: false, raiz: "C:\\ALVR", dll: "C:\\ALVR\\bin\\win64\\driver_alvr_server.dll" },
            { nome: "lighthouse", rotulo: "Index / Vive", suporte: "nativo", embutido: true, raiz: steamvr + "\\lighthouse", dll: "" },
            { nome: "vrlink", rotulo: "Steam Link", suporte: "streaming", embutido: true, raiz: steamvr + "\\vrlink", dll: "" },
          ],
          alvo: modo === "sem-alvo" ? null : { nome: "pico", rotulo: "Pico Connect", suporte: "alvo", embutido: false, raiz, dll: raiz + "\\bin\\win64\\driver_pico.dll", estado: on || modo === "offline" ? "instalado" : modo },
          log: on ? {
            caminho: "C:\\Users\\voce\\AppData\\Local\\Temp\\ghostframe.log", idade: 0.8,
            dados: {
              pid: "8812", hmd: true, video: "DirectMode", depth: true, motionSmoothing: "off", render: "2160x2160",
              avisos: ["  !! versão diferente da que conhecemos (IVRVirtualDisplay_002 / IVRDriverDirectModeComponent_009)"],
              stats: { "DirectMode::Present": present, "HMD TrackedDevicePoseUpdated": poses },
              linhas: [
                "    0.002  ghostframe proxy iniciado. DLL própria: C:\\...\\driver_pico.dll",
                "    0.004  Carregando driver real: C:\\...\\driver_pico_real.dll",
                "    0.210  Provider::Init -> 0",
                "    3.118  Host::TrackedDeviceAdded serial=PA7L10MGH class=HMD",
                "    3.120  Device(HMD)::GetComponent(\"IVRDriverDirectModeComponent_009\") -> IMPLEMENTADO",
                "    3.120    -> embrulhando IVRDriverDirectModeComponent",
                "    3.121    !! versão diferente da que conhecemos (IVRVirtualDisplay_002 / IVRDriverDirectModeComponent_009)",
                "  300.400  STATS DirectMode::Present            71.8/s  dt(ms) avg=13.93 min=11.20 max=21.40",
                "  300.410  STATS HMD TrackedDevicePoseUpdated  501.2/s  dt(ms) avg=2.00 min=1.10 max=3.80",
              ],
            },
          } : null,
          config: { ...cfg },
          steamvr: { trava, motionSmoothing: false, supersample: 1.0, supersampleManual: false },
          jogos: [{ chave: "steam.app.438100", nome: "VRChat" }, { chave: "steam.app.620980", nome: "Beat Saber" }, { chave: "system.generated.jogo.exe", nome: "jogo.exe" }],
        };
      },
    },
  };
}
