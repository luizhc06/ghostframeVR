// App de controle do ghostframeVR: instala o proxy no driver de streaming do SteamVR,
// mostra status/desempenho lendo o ghostframe.log e grava o ghostframe.ini.
// Nunca toca no processo do jogo: só lista nomes de processos.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
#![cfg_attr(not(windows), allow(dead_code))]

mod auto;
mod core;
mod hud;

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
    use serde_json::{json, Map, Value};
    use std::os::windows::process::CommandExt;
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use std::sync::Mutex;
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
        core::processos(&mut sysinfo::System::new())["steamvr"] == true
    }

    #[tauri::command]
    fn estado(sys: tauri::State<Mutex<sysinfo::System>>) -> Value {
        let ovr = openvrpaths();
        let cfg = config();
        let alvo = core::alvo(&cfg, &ovr);
        let log = alvo.as_ref().and_then(core::log_do_alvo);
        let log = log.map(|(p, idade)| {
            let dados = core::ler_fim(&p, 512 * 1024).map(|t| core::ler_log(&t)).unwrap_or(Value::Null);
            json!({ "caminho": p, "idade": idade, "dados": dados })
        });
        let jogo = cfg.get("app.jogo").and_then(Value::as_str).unwrap_or(core::VRCHAT).to_string();
        let vrs = vrsettings(&ovr).and_then(|p| core::ler_json(&p));
        let steamapps = ovr.config.as_ref().and_then(|c| c.parent()).map(|s| s.join("steamapps"));
        let jogos = vrs.as_ref().map(|v| core::jogos(v, steamapps.as_deref())).unwrap_or_default();
        let steamvr = vrs.map(|v| core::ler_steamvr(&v, &jogo));
        json!({
            "versao": env!("CARGO_PKG_VERSION"),
            "dllEmbutida": !PROXY_DLL.is_empty(),
            "processos": core::processos(&mut sys.lock().unwrap()),
            "drivers": ovr.drivers.iter().map(Driver::json).collect::<Vec<_>>(),
            "alvo": alvo.map(|d| { let mut j = d.json(); j["estado"] = core::estado(&d.dll).codigo().into(); j }),
            "log": log,
            "config": cfg,
            "steamvr": steamvr,
            "jogos": jogos,
            "pastaApp": pasta_app(),
        })
    }

    #[tauri::command]
    fn salvar_config(valores: Map<String, Value>) -> Result<(), String> {
        let mut cfg = config();
        cfg.extend(valores);
        fs::create_dir_all(pasta_app()).map_err(|e| e.to_string())?;
        fs::write(arquivo_ini(), core::config_para_ini(&cfg)).map_err(|e| e.to_string())
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

    #[tauri::command]
    fn trava_fps(frames: i64) -> Result<(), String> {
        if steamvr_aberto() {
            return Err("feche o SteamVR antes: ele regrava o steamvr.vrsettings ao sair".into());
        }
        let p = vrsettings(&openvrpaths()).ok_or("não achei a pasta de config do SteamVR")?;
        let cfg = config();
        let jogo = cfg.get("app.jogo").and_then(Value::as_str).unwrap_or(core::VRCHAT);
        core::gravar_trava(&p, jogo, frames).map_err(|e| e.to_string())
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
            .manage(Mutex::new(sysinfo::System::new()))
            .invoke_handler(tauri::generate_handler![
                estado, salvar_config, instalar, desinstalar, trava_fps, relatorio, abrir_pasta
            ])
            .setup(move |app| {
                let abrir = MenuItem::with_id(app, "abrir", "Abrir", true, None::<&str>)?;
                let sair = MenuItem::with_id(app, "sair", "Sair", true, None::<&str>)?;
                TrayIconBuilder::new()
                    .icon(app.default_window_icon().cloned().ok_or("sem ícone")?)
                    .tooltip("ghostframeVR")
                    .menu(&Menu::with_items(app, &[&abrir, &sair])?)
                    .show_menu_on_left_click(false)
                    .on_menu_event(|app, ev| match ev.id.as_ref() {
                        "abrir" => abrir_janela(app),
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
