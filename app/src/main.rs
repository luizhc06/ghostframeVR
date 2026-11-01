// App de controle do ghostframeVR: instala o proxy no driver de streaming do SteamVR,
// mostra status/desempenho lendo o ghostframe.log e grava o ghostframe.ini.
// Nunca toca no processo do jogo: só lista nomes de processos.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
#![cfg_attr(not(windows), allow(dead_code))]

mod auto;
mod companion;
mod core;
mod hud;
mod vr;
mod vrchat;

#[cfg(not(windows))]
fn main() {
    eprintln!("O app do ghostframeVR roda só no Windows. A lógica é testada com `cargo test`.");
}

#[cfg(windows)]
fn main() {
    win::main()
}

#[cfg(windows)]
mod win {
    use crate::core::{self, Driver};
    use crate::{companion, hud, vr};
    use serde_json::{json, Map, Value};
    use std::os::windows::process::CommandExt;
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use std::sync::{mpsc, Arc, Mutex};
    use std::time::Duration;
    use std::{fs, io};
    use tauri::menu::{Menu, MenuItem};
    use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
    use tauri::{AppHandle, Manager, RunEvent, WebviewWindowBuilder};

    static PROXY_DLL: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/proxy.dll"));
    const SEM_JANELA: u32 = 0x0800_0000; // CREATE_NO_WINDOW

    fn local() -> PathBuf {
        PathBuf::from(std::env::var_os("LOCALAPPDATA").unwrap_or_default())
    }
    fn pasta_app() -> PathBuf {
        local().join("ghostframeVR")
    }
    fn arquivo_ini() -> PathBuf {
        pasta_app().join("ghostframe.ini")
    }
    fn openvrpaths() -> core::OpenVrPaths {
        core::ler_openvrpaths(&local().join("openvr").join("openvrpaths.vrpath"))
    }
    fn config() -> Map<String, Value> {
        core::config(&fs::read_to_string(arquivo_ini()).unwrap_or_default())
    }
    fn vrsettings(ovr: &core::OpenVrPaths) -> Option<PathBuf> {
        ovr.config.as_ref().map(|c| c.join("steamvr.vrsettings"))
    }
    fn steamvr_aberto() -> bool {
        vr::processo_rodando("vrserver.exe")
    }
    /// %LocalAppdata%Low\VRChat\VRChat\config.json (doc do VRChat). None sem LOCALAPPDATA.
    fn vrchat_config() -> Option<PathBuf> {
        let l = local();
        let pai = l.parent().filter(|_| l.is_absolute())?;
        Some(pai.join("LocalLow").join("VRChat").join("VRChat").join("config.json"))
    }
    /// Conteúdo do config.json; "" se ainda não existe. Erro de leitura nunca vira "vazio":
    /// senão o próximo gravar apagaria o arquivo do usuário.
    fn ler_vrchat_config() -> Result<String, String> {
        let p = vrchat_config().ok_or("não achei a pasta do usuário (LOCALAPPDATA)")?;
        match fs::read_to_string(p) {
            Ok(t) => Ok(t),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(String::new()),
            Err(e) => Err(format!("não consegui ler o config.json do VRChat ({e}): não mexi nele")),
        }
    }
    /// Agendamento de GPU por hardware (HwSchMode: 2 ligado, 1 desligado). Só muda com
    /// reinício do Windows: lido uma vez por execução do app, sem abrir janela.
    fn hags() -> Option<bool> {
        static HAGS: std::sync::OnceLock<Option<bool>> = std::sync::OnceLock::new();
        *HAGS.get_or_init(|| {
            let out = Command::new("reg")
                .args(["query", r"HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers", "/v", "HwSchMode"])
                .creation_flags(SEM_JANELA)
                .output()
                .ok()?;
            let txt = String::from_utf8_lossy(&out.stdout);
            let v = txt.split_whitespace().last()?.trim_start_matches("0x");
            u32::from_str_radix(v, 16).ok().map(|n| n == 2)
        })
    }
    /// Jogo da trava: o fixado pelo usuário ou o que está aberto no SteamVR agora.
    fn jogo(cfg: &Map<String, Value>, painel: &companion::Painel) -> String {
        match cfg.get("app.jogo_fixo").and_then(Value::as_str).unwrap_or("") {
            "" => painel.vr["jogo"]["chave"].as_str().unwrap_or("").to_string(),
            fixo => fixo.to_string(),
        }
    }

    struct Companion {
        painel: Arc<Mutex<companion::Painel>>,
        pedidos: Mutex<mpsc::Sender<companion::Gravar>>,
    }

    // Comandos async: rodam fora da thread da janela (leem arquivos, esperam o companion).
    #[tauri::command]
    async fn estado(c: tauri::State<'_, Companion>) -> Result<Value, String> {
        let ovr = openvrpaths();
        let cfg = config();
        let alvo = core::alvo(&cfg, &ovr);
        let log = alvo.as_ref().and_then(core::log_do_alvo);
        let log = log.map(|(p, idade)| {
            let dados = core::ler_fim(&p, 512 * 1024).map(|t| core::ler_log(&t)).unwrap_or(Value::Null);
            json!({ "caminho": p, "idade": idade, "dados": dados })
        });
        let jogo = jogo(&cfg, &c.painel.lock().unwrap_or_else(|e| e.into_inner()));
        let vrs = vrsettings(&ovr).and_then(|p| core::ler_json(&p));
        let steamapps = ovr.config.as_ref().and_then(|c| c.parent()).map(|s| s.join("steamapps"));
        let jogos = vrs.as_ref().map(|v| core::jogos(v, steamapps.as_deref())).unwrap_or_default();
        let steamvr = vrs.map(|v| core::ler_steamvr(&v, &jogo));
        // Só headset por streaming (trackers e utilitários como o Standable ficam de fora),
        // mais o escolhido à mão, mesmo que não tenha sido detectado: tem de aparecer marcado.
        let mut drivers: Vec<Value> = ovr
            .drivers
            .iter()
            .filter(|d| core::eh_streaming(d) || alvo.as_ref().is_some_and(|a| a.raiz == d.raiz))
            .map(Driver::json)
            .collect();
        if let Some(a) = alvo.as_ref().filter(|a| !ovr.drivers.iter().any(|d| d.raiz == a.raiz)) {
            drivers.push(a.json());
        }
        let mut r = json!({
            "versao": env!("CARGO_PKG_VERSION"),
            "dllEmbutida": !PROXY_DLL.is_empty(),
            // O proxy instalado já gera frames (instalado mas antigo: atualizar).
            "framegenDisponivel": alvo.as_ref().is_some_and(|d| core::proxy_com_framegen(&d.dll)),
            "processos": { "steamvr": steamvr_aberto() },
            "drivers": drivers,
            "alvoAuto": cfg.get("app.alvo").and_then(Value::as_str).unwrap_or("").is_empty(),
            "alvo": alvo.map(|d| { let mut j = d.json(); j["estado"] = core::estado(&d.dll).codigo().into(); j }),
            "log": log,
            "config": cfg,
            "steamvr": steamvr,
            "jogos": jogos,
            "pastaApp": pasta_app(),
        });
        let limitador = ler_vrchat_config().and_then(|t| core::vrc_limitador(&t));
        let nomes = vrchat::reg::nomes();
        let contas = vrchat::contas(&nomes);
        let ler = |n: &str| vrchat::reg::ler(n).and_then(|(t, d)| vrchat::Valor::de(t, &d));
        let ajustes: Map<String, Value> =
            vrchat::AJUSTES.iter().map(|a| (a.id.to_string(), json!(vrchat::valor_atual(a, &contas, &ler)))).collect();
        r["vrchat"] = json!({
            "aberto": vr::processo_rodando("VRChat.exe"),
            "limitador": limitador.as_ref().ok(),
            "erroConfig": limitador.as_ref().err(),
            "amd": core::cpu_amd(),
            "threads": std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0),
            "hags": hags(),
            "energia": plano_energia().map(|(g, n)| json!({ "alto": g == PLANO_ALTO, "nome": n })),
            // Opções do jogo no registro (vazio = o VRChat ainda não rodou neste usuário).
            "jogo": { "rodou": !nomes.is_empty(), "contas": contas.len(), "ajustes": ajustes, "temBackup": backup_vrchat().exists() },
        });
        let p = c.painel.lock().unwrap_or_else(|e| e.into_inner());
        r["vr"] = p.vr.clone();
        r["auto"] = p.auto.clone();
        r["hud"] = json!({ "conectado": p.hud_conectado, "erro": p.erro });
        Ok(r)
    }

    #[tauri::command]
    fn salvar_config(valores: Map<String, Value>) -> Result<(), String> {
        let mut cfg = config();
        cfg.extend(valores);
        fs::create_dir_all(pasta_app()).map_err(|e| e.to_string())?;
        core::gravar_atomico(&arquivo_ini(), &core::config_para_ini(&cfg)).map_err(|e| e.to_string())
    }

    /// Escolha do driver do headset na janela ("" = automático): valida antes de salvar.
    #[tauri::command]
    fn escolher_alvo(caminho: String) -> Result<Value, String> {
        let raiz = Some(caminho.trim()).filter(|c| !c.is_empty()).map(core::normalizar_raiz);
        let driver = match &raiz {
            Some(r) => {
                let d = core::driver_em(r);
                if !d.dll.exists() {
                    return Err(format!(
                        "Essa pasta não tem um driver do SteamVR (falta bin\\win64\\driver_*.dll): {}. Use a pasta que tem o driver.vrdrivermanifest.",
                        r.display()
                    ));
                }
                Some(d)
            }
            None => core::alvo_padrao(&openvrpaths().drivers).cloned(),
        };
        let valor = raiz.map(|r| r.display().to_string()).unwrap_or_default();
        let mut v = Map::new();
        v.insert("app.alvo".into(), json!(valor));
        salvar_config(v)?;
        let mensagem = match &driver {
            Some(d) => format!("Driver do headset: {} ({})", core::tipo(&d.nome, &d.raiz).0, d.raiz.display()),
            None => "Automático: nenhum driver de headset encontrado ainda".into(),
        };
        Ok(json!({ "alvo": valor, "mensagem": mensagem }))
    }

    fn executar(op: &str, dll: &Path) -> io::Result<String> {
        match op {
            "instalar" => core::instalar(dll, PROXY_DLL, &pasta_app().join("backup")),
            _ => core::desinstalar(dll),
        }
    }

    /// Roda a mesma operação numa cópia elevada deste .exe (UAC) e espera.
    fn elevado(op: &str, dll: &Path) -> Result<String, String> {
        let exe = std::env::current_exe().map_err(|e| e.to_string())?;
        let saida = std::env::temp_dir().join("ghostframe-elevado.txt");
        let _ = fs::remove_file(&saida);
        let ps = |s: &Path| s.display().to_string().replace('\'', "''");
        // Aspas duplas dentro das simples: o Start-Process do PowerShell 5 não cita argumentos com espaço.
        let cmd = format!(
            "$p = Start-Process -FilePath '{}' -ArgumentList '--elevado','{}','\"{}\"','\"{}\"' -Verb RunAs -Wait -PassThru; exit $p.ExitCode",
            ps(&exe), op, ps(dll), ps(&saida)
        );
        let st = Command::new("powershell")
            .args(["-NoProfile", "-NonInteractive", "-Command", &cmd])
            .creation_flags(SEM_JANELA)
            .status()
            .map_err(|e| e.to_string())?;
        let msg = fs::read_to_string(&saida).unwrap_or_else(|_| "permissão de administrador negada".into());
        if st.success() { Ok(msg) } else { Err(msg) }
    }

    fn operar(op: &str) -> Result<String, String> {
        let d = core::alvo(&config(), &openvrpaths()).ok_or("nenhum driver alvo encontrado")?;
        if steamvr_aberto() {
            return Err("feche o SteamVR antes: a DLL do driver fica travada enquanto ele roda".into());
        }
        if !d.dll.exists() {
            return Err(format!("não achei {}", d.dll.display()));
        }
        if core::pode_escrever(&d.dll) {
            executar(op, &d.dll).map_err(|e| e.to_string())
        } else {
            elevado(op, &d.dll)
        }
    }

    #[tauri::command]
    async fn instalar() -> Result<String, String> {
        operar("instalar")
    }

    #[tauri::command]
    async fn desinstalar() -> Result<String, String> {
        operar("desinstalar")
    }

    /// Ajuste do SteamVR vindo da janela: ao vivo pelo companion; com o SteamVR
    /// fechado, direto no steamvr.vrsettings (aberto, ele regravaria o arquivo ao sair).
    #[tauri::command]
    async fn vr_set(chave: String, valor: Value, c: tauri::State<'_, Companion>) -> Result<(), String> {
        let jogo = jogo(&config(), &c.painel.lock().unwrap_or_else(|e| e.into_inner()));
        let num = || valor.as_f64().ok_or("valor inválido");
        let sim = || valor.as_bool().ok_or("valor inválido");
        let s = |sec: &str, k: &str, v| (sec.to_string(), k.to_string(), v);
        let valores = match chave.as_str() {
            "supersample" => vec![s("steamvr", "supersampleScale", vr::Valor::Float(num()?.clamp(0.2, 5.0) as f32))],
            "supersampleManual" => vec![s("steamvr", "supersampleManualOverride", vr::Valor::Bool(sim()?))],
            "motionSmoothing" => vec![s("steamvr", "motionSmoothing", vr::Valor::Bool(sim()?))],
            "home" => vec![s("steamvr", "enableHomeApp", vr::Valor::Bool(sim()?))],
            "trava" => {
                if jogo.is_empty() {
                    return Err("Nenhum jogo aberto no SteamVR agora: abra o jogo ou escolha um na lista".into());
                }
                let n = vr::Valor::Int(num()?.clamp(0.0, 2.0) as i32);
                vec![s(&jogo, "framesToThrottle", n), s(&jogo, "additionalFramesToPredict", n)]
            }
            _ => return Err(format!("ajuste desconhecido: {chave}")),
        };
        let (tx, rx) = mpsc::channel();
        let pedido = companion::Gravar { valores: valores.clone(), resposta: tx };
        if c.pedidos.lock().unwrap_or_else(|e| e.into_inner()).send(pedido).is_ok() {
            match rx.recv_timeout(Duration::from_secs(2)) {
                Ok(Ok(())) => return Ok(()),
                Ok(Err(e)) if e != companion::SEM_SESSAO => return Err(e),
                _ => {}
            }
        }
        if steamvr_aberto() {
            return Err("conectando ao SteamVR, tente de novo em alguns segundos".into());
        }
        let p = vrsettings(&openvrpaths()).ok_or("não achei a config do SteamVR")?;
        let lista: Vec<_> = valores
            .iter()
            .map(|(sec, k, v)| {
                let j = match *v {
                    vr::Valor::Bool(b) => json!(b),
                    vr::Valor::Int(i) => json!(i),
                    vr::Valor::Float(f) => json!(f),
                };
                (sec.as_str(), k.as_str(), Some(j))
            })
            .collect();
        core::gravar_steamvr(&p, &lista).map_err(|e| e.to_string())
    }

    /// A imagem exata que o headset vai mostrar (a janela não reimplementa o visual).
    #[tauri::command]
    async fn hud_previa(c: tauri::State<'_, Companion>) -> Result<Value, String> {
        let mut m = c.painel.lock().unwrap_or_else(|e| e.into_inner()).metricas.clone();
        if m.modo.is_empty() {
            m = companion::exemplo();
        }
        let mut hud = hud::Hud::default();
        let (px, w, h) = hud.desenhar(&m, &companion::itens(&config()));
        Ok(json!({ "largura": w, "altura": h, "rgba": core::base64(px) }))
    }

    /// Junta log, vrserver.txt e configs numa pasta para anexar no GitHub.
    /// A pasta do driver da Pico NÃO entra aqui (binário de terceiros); ela fica no backup.
    #[tauri::command]
    async fn relatorio() -> Result<String, String> {
        let ovr = openvrpaths();
        let dir = pasta_app().join(format!("relatorio-{}", core::agora()));
        fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let copiar = |de: PathBuf| {
            if let Some(nome) = de.file_name() {
                let _ = fs::copy(&de, dir.join(nome));
            }
        };
        copiar(local().join("openvr").join("openvrpaths.vrpath"));
        copiar(std::env::temp_dir().join("ghostframe.log"));
        copiar(arquivo_ini());
        if let Some(l) = &ovr.log {
            copiar(l.join("vrserver.txt"));
        }
        if let Some(d) = core::alvo(&config(), &ovr) {
            copiar(d.dll.with_file_name("ghostframe.log"));
            copiar(d.raiz.join("driver.vrdrivermanifest"));
        }
        let _ = Command::new("explorer").arg(&dir).spawn();
        Ok(dir.display().to_string())
    }

    /// Liga/desliga o limitador de partículas no config.json do VRChat (backup na 1ª vez).
    #[tauri::command(async)]
    fn vrchat_particulas(ligar: bool) -> Result<String, String> {
        let p = vrchat_config().ok_or("não achei a pasta do usuário (LOCALAPPDATA)")?;
        let texto = ler_vrchat_config()?;
        let novo = core::vrc_com_limitador(&texto, ligar)?;
        let pasta = p.parent().ok_or("caminho do VRChat inválido")?;
        if !pasta.is_dir() {
            return Err("não achei a pasta do VRChat: abra o VRChat uma vez antes".into());
        }
        let backup = p.with_extension("json.ghostframe-backup");
        if !texto.is_empty() && !backup.exists() {
            fs::write(&backup, &texto).map_err(|e| format!("backup do config.json: {e}"))?;
        }
        core::gravar_atomico(&p, &novo).map_err(|e| format!("config.json do VRChat: {e}"))?;
        let quando = if vr::processo_rodando("VRChat.exe") { " Vale quando o VRChat abrir de novo." } else { "" };
        Ok(format!("Limitador de partículas {}.{quando}", if ligar { "ligado" } else { "desligado" }))
    }

    // Planos de energia padrão do Windows (powercfg /list). O VRChat pede um plano "para
    // uso pesado" em vez de economia; o app só troca quando o usuário clica.
    const PLANO_ALTO: &str = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    const PLANO_EQUILIBRADO: &str = "381b4222-f694-41f0-9685-ff5bb260df2e";

    /// (GUID, nome) do plano de energia ativo. O estado roda a cada 1 s: o powercfg só a cada 10 s.
    static PLANO: Mutex<Option<(std::time::Instant, Option<(String, String)>)>> = Mutex::new(None);
    fn plano_energia() -> Option<(String, String)> {
        let mut c = PLANO.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((_, v)) = c.as_ref().filter(|(t, _)| t.elapsed() < Duration::from_secs(10)) {
            return v.clone();
        }
        let v = ler_plano_energia();
        *c = Some((std::time::Instant::now(), v.clone()));
        v
    }
    fn ler_plano_energia() -> Option<(String, String)> {
        let out = Command::new("powercfg").arg("/getactivescheme").creation_flags(SEM_JANELA).output().ok()?;
        let txt = String::from_utf8_lossy(&out.stdout);
        let guid = txt.split_whitespace().find(|p| p.len() == 36 && p.matches('-').count() == 4)?.to_lowercase();
        let nome = txt.split('(').nth(1).and_then(|r| r.split(')').next()).unwrap_or("").trim().to_string();
        Some((guid, nome))
    }

    #[tauri::command(async)]
    fn plano_energia_usar(alto: bool) -> Result<String, String> {
        let guid = if alto { PLANO_ALTO } else { PLANO_EQUILIBRADO };
        let out = Command::new("powercfg").args(["/setactive", guid]).creation_flags(SEM_JANELA).output().map_err(|e| e.to_string())?;
        *PLANO.lock().unwrap_or_else(|e| e.into_inner()) = None; // o próximo estado lê de novo
        if !out.status.success() {
            return Err(if alto {
                "este PC não tem o plano Alto desempenho (comum em notebooks): escolha um plano de desempenho nas Configurações de energia".into()
            } else {
                "não consegui voltar ao plano Equilibrado".into()
            });
        }
        Ok(format!("Plano de energia: {}.", if alto { "Alto desempenho" } else { "Equilibrado" }))
    }

    fn backup_vrchat() -> PathBuf {
        pasta_app().join("vrchat-backup.json")
    }
    const VRCHAT_ABERTO: &str = "feche o VRChat antes: ele regrava essas opções quando fecha";

    /// Grava opções do VRChat no registro, com o jogo fechado. Antes da 1ª mudança de cada
    /// opção, guarda o valor de antes em vrchat-backup.json (Restaurar volta a ele).
    #[tauri::command(async)]
    fn vrchat_ajustar(valores: Map<String, Value>) -> Result<String, String> {
        if vr::processo_rodando("VRChat.exe") {
            return Err(VRCHAT_ABERTO.into());
        }
        let nomes = vrchat::reg::nomes();
        if nomes.is_empty() {
            return Err("abra o VRChat uma vez neste PC antes".into());
        }
        let contas = vrchat::contas(&nomes);
        let mut mudar = Vec::new();
        for (id, v) in &valores {
            let a = vrchat::ajuste(id).ok_or(format!("opção desconhecida: {id}"))?;
            let v = v.as_i64().and_then(|x| i32::try_from(x).ok()).filter(|x| vrchat::valido(id, *x));
            mudar.push((a, v.ok_or(format!("valor inválido para {id}"))?));
        }
        let todos: Vec<String> = mudar.iter().flat_map(|(a, _)| vrchat::nomes_do_ajuste(a, &contas)).collect();
        let mut backup = core::ler_json(&backup_vrchat()).and_then(|v| v.as_object().cloned()).unwrap_or_default();
        vrchat::completar_backup(&mut backup, &todos, &vrchat::reg::ler);
        fs::create_dir_all(pasta_app()).map_err(|e| e.to_string())?;
        core::gravar_atomico(&backup_vrchat(), &Value::Object(backup).to_string()).map_err(|e| format!("backup: {e}"))?;
        for (a, v) in &mudar {
            let (tipo, dados) = vrchat::Valor::Int(*v).para();
            for n in vrchat::nomes_do_ajuste(a, &contas) {
                vrchat::reg::gravar(&n, tipo, &dados)?;
            }
        }
        Ok("VRChat ajustado: vale quando ele abrir.".into())
    }

    /// Volta as opções do VRChat ao que eram antes do app mexer.
    #[tauri::command(async)]
    fn vrchat_restaurar() -> Result<String, String> {
        if vr::processo_rodando("VRChat.exe") {
            return Err(VRCHAT_ABERTO.into());
        }
        let backup = core::ler_json(&backup_vrchat()).and_then(|v| v.as_object().cloned()).ok_or("nada para restaurar")?;
        for (n, antes) in vrchat::restauracao(&backup) {
            match antes {
                Some((tipo, dados)) => vrchat::reg::gravar(&n, tipo, &dados)?,
                None => vrchat::reg::apagar(&n)?,
            }
        }
        fs::remove_file(backup_vrchat()).map_err(|e| e.to_string())?;
        Ok("Opções do VRChat de volta como eram.".into())
    }

    /// Abre Configurações → Sistema → Tela → Gráficos do Windows (o app não muda sozinho).
    #[tauri::command]
    fn abrir_graficos_windows() {
        // Windows 11: "Alterar configurações gráficas padrão", onde fica o HAGS.
        let _ = Command::new("explorer").arg("ms-settings:display-advancedgraphics-default").spawn();
    }

    #[tauri::command]
    fn abrir_pasta(caminho: String) {
        let p = PathBuf::from(caminho);
        let p = if p.is_file() { p.parent().map(Path::to_path_buf).unwrap_or(p) } else { p };
        let _ = Command::new("explorer").arg(p).spawn();
    }

    pub fn main() {
        let a: Vec<String> = std::env::args().collect();
        if a.len() == 5 && a[1] == "--elevado" {
            let (codigo, msg) = match executar(&a[2], Path::new(&a[3])) {
                Ok(m) => (0, m),
                Err(e) => (1, e.to_string()),
            };
            let _ = fs::write(&a[4], msg);
            std::process::exit(codigo);
        }
        // --bandeja: aberto pelo SteamVR (autostart); fica só no ícone, sem WebView.
        let bandeja = a.iter().any(|x| x == "--bandeja");
        tauri::Builder::default()
            // Uma cópia só: o autostart do SteamVR com o app já aberto só avisa a que está rodando.
            .plugin(tauri_plugin_single_instance::init(|app, args, _| {
                if !args.iter().any(|x| x == "--bandeja") {
                    abrir_janela(app);
                }
            }))
            .invoke_handler(tauri::generate_handler![
                estado, salvar_config, escolher_alvo, instalar, desinstalar, vr_set, hud_previa, relatorio, abrir_pasta,
                vrchat_particulas, abrir_graficos_windows, vrchat_ajustar, vrchat_restaurar, plano_energia_usar
            ])
            .setup(move |app| {
                let painel = Arc::new(Mutex::new(companion::Painel::default()));
                let caminhos = companion::Caminhos {
                    app: pasta_app(),
                    openvrpaths: local().join("openvr").join("openvrpaths.vrpath"),
                };
                // Aberto pelo SteamVR e sem janela: sai junto com ele, como os outros overlays.
                let h = app.handle().clone();
                let ao_fechar = Box::new(move || {
                    if bandeja && h.get_webview_window("main").is_none() {
                        h.exit(0);
                    }
                });
                let pedidos = Mutex::new(companion::iniciar(caminhos, painel.clone(), ao_fechar));
                app.manage(Companion { painel, pedidos });
                let abrir = MenuItem::with_id(app, "abrir", "Abrir", true, None::<&str>)?;
                let fg = MenuItem::with_id(app, "framegen", "Ligar/desligar frame gen", true, None::<&str>)?;
                let sair = MenuItem::with_id(app, "sair", "Sair", true, None::<&str>)?;
                TrayIconBuilder::new()
                    .icon(app.default_window_icon().cloned().ok_or("sem ícone")?)
                    .tooltip("ghostframeVR")
                    .menu(&Menu::with_items(app, &[&abrir, &fg, &sair])?)
                    .show_menu_on_left_click(false)
                    .on_menu_event(|app, ev| match ev.id.as_ref() {
                        "abrir" => abrir_janela(app),
                        // Sem abrir a janela: o proxy relê o ini a cada segundo.
                        "framegen" => {
                            let ligado = config().get("framegen.ativo").and_then(Value::as_str) == Some("1");
                            let mut v = Map::new();
                            v.insert("framegen.ativo".into(), json!(if ligado { "0" } else { "1" }));
                            let _ = salvar_config(v);
                        }
                        "sair" => app.exit(0),
                        _ => {}
                    })
                    .on_tray_icon_event(|icone, ev| {
                        if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = ev {
                            abrir_janela(icone.app_handle());
                        }
                    })
                    .build(app)?;
                if !bandeja {
                    abrir_janela(app.handle());
                }
                Ok(())
            })
            .build(tauri::generate_context!())
            .expect("falha ao iniciar o ghostframeVR")
            .run(|_, ev| {
                // Fechar a janela destrói o WebView (é ele que pesa na RAM); o app segue na bandeja.
                if let RunEvent::ExitRequested { api, code: None, .. } = ev {
                    api.prevent_exit();
                }
            });
    }

    fn abrir_janela(app: &AppHandle) {
        if let Some(j) = app.get_webview_window("main") {
            let _ = j.unminimize();
            let _ = j.set_focus();
            return;
        }
        if let Some(cfg) = app.config().app.windows.first() {
            let _ = WebviewWindowBuilder::from_config(app, cfg).and_then(|b| b.build());
        }
    }
}
