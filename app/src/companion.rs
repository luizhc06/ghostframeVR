//! Thread do companion: conecta no SteamVR quando ele abre e cuida do HUD no headset,
//! do modo auto e das configurações ao vivo. A sessão OpenVR só é usada nesta thread.
//! Roda com ou sem janela aberta (a janela pesa na RAM; isto aqui, quase nada).
use crate::{auto, core, hud, vr};
use serde_json::{json, Map, Value};
use std::path::PathBuf;
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};

const CHAVE_HUD: &str = "io.github.luizhc06.ghostframevr.hud";
pub const SEM_SESSAO: &str = "SteamVR desconectado";

pub struct Caminhos {
    pub app: PathBuf,
    pub openvrpaths: PathBuf,
}

/// O que a janela lê no comando `estado` e o que a prévia do HUD desenha.
#[derive(Default)]
pub struct Painel {
    pub vr: Value,
    pub auto: Value,
    pub hud_conectado: bool,
    pub metricas: hud::Metricas,
    /// Último problema com o SteamVR ou o HUD, para a janela mostrar o motivo.
    pub erro: Option<String>,
}

/// Pedido da janela para gravar no SteamVR ao vivo. Erro = sem sessão (o chamador cai no arquivo).
pub struct Gravar {
    pub valores: Vec<(String, String, vr::Valor)>,
    pub resposta: mpsc::Sender<Result<(), String>>,
}

/// `ao_fechar_steamvr` roda nesta thread quando o SteamVR sai (o app aberto pelo
/// autostart usa isso para sair junto).
pub fn iniciar(c: Caminhos, painel: Arc<Mutex<Painel>>, ao_fechar_steamvr: Box<dyn Fn() + Send>) -> mpsc::Sender<Gravar> {
    let (tx, rx) = mpsc::channel();
    let _ = std::thread::Builder::new().name("companion".into()).spawn(move || laco(c, painel, rx, ao_fechar_steamvr));
    tx
}

pub fn itens(cfg: &Map<String, Value>) -> hud::Itens {
    let l = texto(cfg, "hud.itens");
    let tem = |k: &str| l.split(',').any(|x| x.trim() == k);
    hud::Itens { fps: tem("fps"), frames: tem("frames"), frametime: tem("ft"), latencia: tem("lat"), gpu: tem("gpu"), grafico: tem("graf") }
}

/// Prévia do HUD sem SteamVR: o visual com "--", sem números inventados.
pub fn exemplo() -> hud::Metricas {
    hud::Metricas { modo: "PREVIA".into(), ..Default::default() }
}

fn texto<'a>(cfg: &'a Map<String, Value>, chave: &str) -> &'a str {
    cfg.get(chave).and_then(Value::as_str).unwrap_or("")
}

fn num(cfg: &Map<String, Value>, chave: &str, padrao: f32) -> f32 {
    texto(cfg, chave).parse().unwrap_or(padrao)
}

struct Sessao {
    vr: vr::Vr,
    hud: Option<vr::Overlay>,
    /// Assinatura da última posição aplicada; só reposiciona quando muda.
    pos_hud: String,
    autostart: Option<bool>,
    /// Gesto do relógio ligado (HUD no pulso que só aparece ao olhar).
    gesto: Option<Gesto>,
    olhando: bool,
    /// 0 = escondido, 1 = na opacidade escolhida.
    fade: f32,
    /// O pulso segue o índice do controle de quando foi posicionado: reaplica de tempos
    /// em tempos para pegar um controle que reconectou com outro índice.
    posicionado: Instant,
    /// Desde quando o frame gen está ligado (o "esperando" só vale no começo).
    ligado_desde: Option<Instant>,
    /// Com a nossa trava, desde quando nenhum frame gerado chega ao driver.
    sem_entrega: Option<Instant>,
    /// Travou sem entregar frames: não trava de novo até o frame gen ser religado.
    fg_bloqueado: bool,
    /// Resumo da última imagem enviada ao HUD: só reenvia quando muda.
    hud_hash: u64,
}

/// Trava que o app pôs para o frame gen, gravada em disco: o SteamVR persiste a trava
/// no steamvr.vrsettings, então o app tem de lembrar dela mesmo depois de reiniciar.
fn trava_nossa(app: &std::path::Path) -> Option<(String, i64)> {
    let v = core::ler_json(&app.join("trava.json"))?;
    Some((v["jogo"].as_str()?.to_string(), v["valor"].as_i64()?))
}

fn gravar_trava_nossa(app: &std::path::Path, t: &Option<(String, i64)>) {
    let arquivo = app.join("trava.json");
    let _ = match t {
        Some((jogo, valor)) => std::fs::create_dir_all(app)
            .and_then(|_| core::gravar_atomico(&arquivo, &json!({ "jogo": jogo, "valor": valor }).to_string())),
        None => std::fs::remove_file(&arquivo),
    };
}

struct Gesto {
    ancora: vr::Ancora,
    rel: [f32; 12],
    opacidade: f32,
}

/// Roda ~30×/s com o gesto ligado: só lê duas poses e mexe no alfa quando muda.
fn gesto(s: &mut Sessao) {
    let (Some(g), Some(o)) = (&s.gesto, &s.hud) else { return };
    s.olhando = s.vr.poses(g.ancora).is_some_and(|(h, c)| vr::olhando_pulso(&h, &c, &g.rel, s.olhando));
    let alvo = if s.olhando { 1.0 } else { 0.0 };
    let fade = s.fade + (alvo - s.fade).clamp(-0.25, 0.25); // ~130 ms de transição
    if fade != s.fade {
        if s.fade == 0.0 {
            let _ = o.mostrar(&s.vr, true);
        }
        let _ = o.alpha(&s.vr, g.opacidade * fade);
        if fade == 0.0 {
            let _ = o.mostrar(&s.vr, false);
        }
        s.fade = fade;
    }
}

fn laco(c: Caminhos, painel: Arc<Mutex<Painel>>, rx: mpsc::Receiver<Gravar>, ao_fechar_steamvr: Box<dyn Fn() + Send>) {
    let mut sessao: Option<Sessao> = None;
    // Depois de um Quit, espera o vrserver sumir: não reconecta num SteamVR desligando.
    let mut esperando_fechar = false;
    let mut auto = auto::Auto::new();
    let mut desenho = hud::Hud::default();
    let mut historico: Vec<f32> = Vec::with_capacity(61);
    let inicio = Instant::now();
    let mut proximo = Instant::now();

    loop {
        // Pedidos da janela são atendidos na hora; o resto roda a 1 Hz (as métricas mudam
        // nesse ritmo), menos o gesto do relógio, que precisa de ~30 Hz para parecer instantâneo.
        let mut espera = proximo.saturating_duration_since(Instant::now());
        if sessao.as_ref().is_some_and(|s| s.gesto.is_some()) {
            espera = espera.min(Duration::from_millis(33));
        }
        match rx.recv_timeout(espera) {
            Ok(p) => {
                let r = match &sessao {
                    Some(s) => p.valores.iter().try_for_each(|(sec, k, v)| s.vr.gravar(sec, k, *v)),
                    None => Err(SEM_SESSAO.into()),
                };
                let _ = p.resposta.send(r);
                continue;
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
        if let Some(s) = sessao.as_mut() {
            gesto(s);
        }
        if Instant::now() < proximo {
            continue;
        }
        proximo = Instant::now() + Duration::from_secs(1);

        let cfg = core::config(&std::fs::read_to_string(c.app.join("ghostframe.ini")).unwrap_or_default());
        let ovr = core::ler_openvrpaths(&c.openvrpaths);

        let steamvr = vr::processo_rodando("vrserver.exe");
        if sessao.is_none() {
            // Só conecta com o SteamVR já aberto: o companion nunca deve abrir o SteamVR sozinho.
            esperando_fechar &= steamvr;
            if !steamvr || esperando_fechar {
                continue;
            }
            let Some(runtime) = &ovr.runtime else { continue };
            match vr::Vr::conectar(runtime) {
                Ok(v) => {
                    sessao = Some(Sessao {
                        vr: v,
                        hud: None,
                        pos_hud: String::new(),
                        autostart: None,
                        gesto: None,
                        olhando: false,
                        fade: 0.0,
                        posicionado: Instant::now(),
                        ligado_desde: None,
                        sem_entrega: None,
                        fg_bloqueado: false,
                        hud_hash: 0,
                    });
                    painel.lock().unwrap_or_else(|e| e.into_inner()).erro = None;
                }
                Err(e) => {
                    painel.lock().unwrap_or_else(|e| e.into_inner()).erro = Some(format!("SteamVR: {e}"));
                    proximo = Instant::now() + Duration::from_secs(3);
                    continue;
                }
            }
        }
        let s = sessao.as_mut().unwrap();
        // Quit pedido pelo SteamVR, ou o vrserver sumiu sem avisar (travou, foi morto).
        let saiu = s.vr.deve_sair();
        if saiu || !steamvr {
            sessao = None;
            esperando_fechar = saiu;
            *painel.lock().unwrap_or_else(|e| e.into_inner()) = Painel::default();
            ao_fechar_steamvr();
            continue;
        }
        let mut erro: Option<String> = None;

        let info = s.vr.info_hmd();
        let timing = s.vr.timing();
        // A trava vale para o jogo aberto no SteamVR, a menos que o usuário fixe um.
        let jogo_atual = s.vr.jogo_atual();
        let jogo = match texto(&cfg, "app.jogo_fixo") {
            "" => jogo_atual.as_ref().map(|(k, _)| k.clone()).unwrap_or_default(),
            fixo => fixo.to_string(),
        };
        let jogo = jogo.as_str();
        let f = |sec: &str, k: &str, t: vr::Tipo| match s.vr.ler(sec, k, t) {
            Some(vr::Valor::Float(x)) => json!(x),
            Some(vr::Valor::Int(x)) => json!(x),
            Some(vr::Valor::Bool(x)) => json!(x),
            None => Value::Null,
        };
        let trava = f(jogo, "framesToThrottle", vr::Tipo::Int).as_i64().unwrap_or(0);
        let config_vr = json!({
            "supersample": f("steamvr", "supersampleScale", vr::Tipo::Float),
            "supersampleManual": f("steamvr", "supersampleManualOverride", vr::Tipo::Bool),
            "motionSmoothing": f("steamvr", "motionSmoothing", vr::Tipo::Bool),
            "home": f("steamvr", "enableHomeApp", vr::Tipo::Bool),
            "trava": trava,
        });

        // O que o proxy está fazendo, pelo ghostframe.log (linhas STATS e FRAMEGEN).
        let log = core::alvo(&cfg, &ovr)
            .and_then(|d| core::log_do_alvo(&d))
            .filter(|(_, idade)| *idade < 6.0)
            .and_then(|(p, _)| core::ler_fim(&p, 64 * 1024).ok())
            .map(|t| core::ler_log(&t))
            .unwrap_or(Value::Null);
        let fg = &log["framegen"];
        let fg_num = |k: &str| fg[k].as_f64().map(|x| x as f32);
        let ligado = texto(&cfg, "framegen.ativo") == "1";
        let agora = Instant::now();
        if ligado {
            s.ligado_desde.get_or_insert(agora);
        } else {
            s.ligado_desde = None;
            s.fg_bloqueado = false;
        }
        // Funcionando = o proxy gerou frames ("pronto"). "esperando" só conta nos primeiros
        // segundos depois de ligar; parado em "esperando" não segura a trava.
        let no_inicio = s.ligado_desde.is_some_and(|t| agora.duration_since(t) < Duration::from_secs(10));
        let fg_ok = ligado
            && !s.fg_bloqueado
            && match fg["estado"].as_str() {
                Some("pronto") => true,
                Some("esperando") => no_inicio,
                _ => false,
            };

        let decisao = match (&info, &timing) {
            (Some(i), Some(t)) => Some(auto.atualizar(&auto::Entrada {
                hz: i.hz,
                ms_trabalho: t.ms_trabalho,
                framegen_disponivel: fg_ok,
                agora_s: inicio.elapsed().as_secs_f64(),
            })),
            _ => None,
        };

        // Trava do jogo para o frame gen: Auto decide; Manual segue o multiplicador escolhido.
        // Sem frame gen funcionando, só desfaz a trava que o próprio app pôs (a do usuário fica).
        let gravar_trava = |j: &str, t: i64| {
            let v = vr::Valor::Int(t as i32);
            s.vr.gravar(j, "framesToThrottle", v).and_then(|_| s.vr.gravar(j, "additionalFramesToPredict", v))
        };
        let mut nossa = trava_nossa(&c.app);
        // Trocou de jogo: a trava que o app pôs no jogo anterior sai primeiro.
        if let Some((j, _)) = nossa.clone().filter(|(j, _)| j != jogo) {
            match gravar_trava(&j, 0) {
                Ok(()) => {
                    nossa = None;
                    gravar_trava_nossa(&c.app, &nossa);
                }
                Err(e) => erro = Some(format!("Trava de fps: {e}")),
            }
        }
        let manual = texto(&cfg, "framegen.multiplicador").parse::<i64>().unwrap_or(2).clamp(2, 3) - 1;
        let alvo_trava = if jogo.is_empty() {
            None // nenhum jogo aberto: nada a travar agora
        } else if fg_ok {
            if texto(&cfg, "framegen.modo") == "auto" { decisao.map(|d| i64::from(d.trava)) } else { Some(manual) }
        } else if nossa.is_some() {
            Some(0)
        } else {
            None
        };
        if let Some(t) = alvo_trava.filter(|t| *t != trava) {
            // Lembra antes de gravar: se a 2ª escrita falhar, a trava ainda é nossa.
            let antes = nossa.clone();
            if t > 0 {
                gravar_trava_nossa(&c.app, &Some((jogo.to_string(), t)));
            }
            match gravar_trava(jogo, t) {
                Ok(()) => {
                    nossa = (t > 0).then(|| (jogo.to_string(), t));
                    gravar_trava_nossa(&c.app, &nossa);
                }
                Err(e) => {
                    let lembrar = if t > 0 { Some((jogo.to_string(), t)) } else { antes };
                    gravar_trava_nossa(&c.app, &lembrar);
                    erro = Some(format!("Trava de fps: {e}"));
                }
            }
        }
        // Com a nossa trava, o proxy tem de estar entregando frames: senão o jogo ficaria
        // a meia taxa sem nada no lugar. ~6 s sem gerados desfaz e bloqueia até religar.
        let gerados_agora = fg_num("gerados").unwrap_or(0.0);
        if trava > 0 && nossa.as_ref().is_some_and(|(_, v)| *v > 0) && gerados_agora < 0.5 {
            if agora.duration_since(*s.sem_entrega.get_or_insert(agora)) > Duration::from_secs(6) {
                s.fg_bloqueado = true;
                erro = Some("o frame gen não entregou frames com a trava ligada: trava desfeita (veja o log)".into());
            }
        } else {
            s.sem_entrega = None;
        }

        // fps que chega no headset: os Present do compositor mais os gerados que o proxy
        // entrega sozinho com a trava (sem trava, o gerado ocupa o lugar de um repetido).
        let st = &log["stats"];
        let present = st
            .get("DirectMode::Present")
            .or_else(|| st.get("VirtualDisplay::Present"))
            .and_then(|s| s.as_array()?.last()?.get(1)?.as_f64())
            .map(|x| x as f32);
        let gerados = fg_num("gerados").filter(|_| ligado);
        let extras = if fg_num("trava").unwrap_or(0.0) > 0.0 { gerados.unwrap_or(0.0) } else { 0.0 };
        let fps_headset = present.map(|p| p + extras);
        if let Some(x) = fps_headset.or(timing.as_ref().map(|t| t.fps_jogo)) {
            if historico.len() == 60 {
                historico.remove(0);
            }
            historico.push(x);
        }
        let modo = if !ligado {
            "SEM FG".to_string()
        } else if !fg_ok {
            "FG ESPERANDO".to_string()
        } else if texto(&cfg, "framegen.modo") == "auto" {
            format!("AUTO {}×", decisao.map(|d| d.multiplicador).unwrap_or(1))
        } else {
            format!("MANUAL {}×", manual + 1)
        };
        let metricas = hud::Metricas {
            fps_headset,
            fps_jogo: timing.as_ref().map(|t| t.fps_jogo),
            hz: info.as_ref().map(|i| i.hz),
            reais: fg_num("reais").filter(|_| ligado).or(present).map(|x| x.round() as u32),
            gerados: Some(gerados.map(|x| x.round() as u32).unwrap_or(0)),
            ms_jogo: timing.as_ref().map(|t| t.ms_trabalho),
            ms_framegen: fg_num("gpuMs").filter(|x| *x >= 0.0 && ligado),
            latencia_ms: None,
            modo,
            historico: historico.clone(),
        };

        // HUD no headset.
        let hud_ativo = texto(&cfg, "hud.ativo") == "1";
        let ancora = match texto(&cfg, "hud.ancora") {
            "pd" => vr::Ancora::PulsoDireito,
            "visao" => vr::Ancora::Visao,
            "mundo" => vr::Ancora::Mundo,
            _ => vr::Ancora::PulsoEsquerdo,
        };
        let no_pulso = matches!(ancora, vr::Ancora::PulsoEsquerdo | vr::Ancora::PulsoDireito);
        let com_gesto = hud_ativo && no_pulso && texto(&cfg, "hud.olhar") == "1";
        let cm = |k| num(&cfg, k, 0.0) / 100.0;
        let deslocamento = [cm("hud.x"), cm("hud.y"), cm("hud.z")];
        let opacidade = num(&cfg, "hud.opacidade", 90.0) / 100.0;
        if hud_ativo && s.hud.is_none() {
            match s.vr.overlay(CHAVE_HUD, "ghostframeVR") {
                Ok(o) => {
                    s.hud = Some(o);
                    s.hud_hash = 0;
                }
                Err(e) => erro = Some(format!("HUD: {e}")),
            }
            s.pos_hud.clear();
            s.fade = 0.0;
        }
        if no_pulso && s.posicionado.elapsed() > Duration::from_secs(5) {
            s.pos_hud.clear();
        }
        if let Some(o) = &s.hud {
            if hud_ativo {
                let (px, w, h) = desenho.desenhar(&metricas, &itens(&cfg));
                // FNV-1a: barato para ~0,5 MB por segundo, e evita reenviar a mesma imagem.
                let hash = px.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, b| (h ^ u64::from(*b)).wrapping_mul(0x100_0000_01b3));
                if hash != s.hud_hash {
                    match o.imagem(&s.vr, px, w, h) {
                        Ok(()) => s.hud_hash = hash,
                        Err(e) => erro = Some(format!("HUD: {e}")),
                    }
                }
                let pos = ["hud.ancora", "hud.tamanho", "hud.opacidade", "hud.x", "hud.y", "hud.z", "hud.olhar"]
                    .map(|k| texto(&cfg, k))
                    .join("|");
                if pos != s.pos_hud {
                    let alpha = if com_gesto { opacidade * s.fade } else { opacidade };
                    let ok = o.posicionar(&s.vr, ancora, num(&cfg, "hud.tamanho", 12.0) / 100.0, deslocamento, [0.0; 3], alpha);
                    // Controle desligado: tenta de novo no próximo segundo.
                    match ok {
                        Ok(()) => {
                            s.pos_hud = pos;
                            s.posicionado = Instant::now();
                        }
                        Err(e) => erro = Some(format!("HUD: {e}")),
                    }
                }
            }
            // Com o gesto, quem mostra e esconde é o laço rápido.
            if !com_gesto {
                let _ = o.mostrar(&s.vr, hud_ativo);
                s.fade = 0.0;
            } else if s.fade == 0.0 {
                let _ = o.mostrar(&s.vr, false);
            }
        }
        s.gesto = com_gesto.then(|| Gesto { ancora, rel: ancora.matriz(deslocamento, [0.0; 3]), opacidade });
        if !com_gesto {
            s.olhando = false;
        }

        // Iniciar junto com o SteamVR.
        let quer = texto(&cfg, "app.autostart") == "1";
        let manifesto = c.app.join("ghostframevr.vrmanifest");
        if s.autostart != Some(quer) {
            // Desligado e nunca registrado: não aparece na lista do SteamVR sem o usuário pedir.
            let r = if !quer && !manifesto.exists() {
                Ok(())
            } else {
                let exe = std::env::current_exe().unwrap_or_default();
                std::fs::create_dir_all(&c.app)
                    .and_then(|_| std::fs::write(&manifesto, vr::manifesto(&exe)))
                    .map_err(|e| e.to_string())
                    .and_then(|_| s.vr.autostart(&manifesto, quer))
            };
            match r {
                Ok(()) => s.autostart = Some(quer),
                Err(e) => erro = Some(format!("Iniciar com o SteamVR: {e}")),
            }
        }

        let mut p = painel.lock().unwrap_or_else(|e| e.into_inner());
        p.vr = match &info {
            Some(i) => json!({
                "hz": i.hz, "render": [i.largura, i.altura], "modelo": i.modelo, "fabricante": i.fabricante,
                "timing": timing.as_ref().map(|t| json!({
                    "fpsJogo": t.fps_jogo, "msCpu": t.ms_cpu, "msGpu": t.ms_gpu, "msTrabalho": t.ms_trabalho,
                    "reprojetados": t.reprojetados, "perdidos": t.perdidos,
                })),
                "config": config_vr,
                "jogo": jogo_atual.as_ref().map(|(k, n)| json!({ "chave": k, "nome": n })),
            }),
            None => Value::Null,
        };
        p.auto = decisao.map_or(Value::Null, |d| {
            json!({ "recomendado": d.recomendado, "multiplicador": d.multiplicador, "trava": d.trava, "motivo": d.motivo })
        });
        p.hud_conectado = s.hud.is_some() && hud_ativo;
        p.metricas = metricas;
        p.erro = erro;
    }
}
