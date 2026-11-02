//! Lógica do app sem Tauri, testável no Linux: achar o driver alvo, instalar o
//! proxy, ler o ghostframe.log, ler/gravar o ghostframe.ini e o steamvr.vrsettings.
use serde_json::{json, Map, Value};
use std::fs;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

/// Texto que só existe na nossa DLL (log de src/proxy/driver_main.cpp).
const MARCA_PROXY: &[u8] = b"ghostframe proxy iniciado";
/// Só nas DLLs que já sabem gerar frames.
const MARCA_FRAMEGEN: &[u8] = b"recursos: framegen";
pub const VRCHAT: &str = "steam.app.438100";

/// Opções do ghostframe.ini e seus padrões. Tudo desligado por padrão.
pub const PADROES: &[(&str, &str)] = &[
    ("framegen.ativo", "0"),
    ("framegen.modo", "auto"),
    ("framegen.multiplicador", "2"),
    ("hud.ativo", "0"),
    ("hud.ancora", "pe"),
    ("hud.olhar", "1"),
    ("hud.tamanho", "12"),
    ("hud.opacidade", "90"),
    ("hud.x", "0"),
    ("hud.y", "0"),
    ("hud.z", "0"),
    ("hud.itens", "fps,frames,ft,graf"),
    ("app.alvo", ""),
    ("app.jogo_fixo", ""), // vazio = trava no jogo que estiver aberto
    ("app.autostart", "0"),
    ("app.proxy_versao", ""), // versão do app que instalou o proxy (atualização automática)
    // Opções de inicialização do VRChat que a aba VRChat monta para copiar (não vão a lugar nenhum sozinhas).
    ("vrchat.prioridade", "1"),
    ("vrchat.thread", "0"),
    ("vrchat.afinidade", "0"),
];

// ---------------------------------------------------------------------------
// config.json do VRChat (%LocalAppdata%Low\VRChat\VRChat). Só chaves documentadas pelo
// VRChat: https://docs.vrchat.com/docs/avatar-particle-system-limits

const VRC_LIMITADOR: &str = "particle_system_limiter";
/// Os padrões da documentação, gravados só se o arquivo ainda não tiver a chave.
const VRC_PS_PADRAO: &[(&str, i64)] = &[
    ("ps_max_particles", 50000),
    ("ps_max_systems", 200),
    ("ps_max_emission", 5000),
    ("ps_max_total_emission", 40000),
    ("ps_mesh_particle_divider", 60),
    ("ps_mesh_particle_poly_limit", 50000),
    ("ps_collision_penalty_high", 50),
    ("ps_collision_penalty_med", 30),
    ("ps_collision_penalty_low", 10),
    ("ps_trails_penalty", 10),
];

fn vrc_raiz(texto: &str) -> Result<Map<String, Value>, String> {
    let texto = texto.trim_start_matches('\u{feff}'); // "UTF-8 com BOM" do Bloco de Notas
    if texto.trim().is_empty() {
        return Ok(Map::new()); // o VRChat não cria o arquivo sozinho
    }
    match serde_json::from_str::<Value>(texto) {
        Ok(Value::Object(m)) => Ok(m),
        Ok(_) => Err("o config.json do VRChat não é um objeto JSON".into()),
        Err(e) => Err(format!("o config.json do VRChat é um JSON inválido ({e}): não mexi nele")),
    }
}

/// O limitador de partículas está ligado nesse config.json? Erro = arquivo inválido.
pub fn vrc_limitador(texto: &str) -> Result<bool, String> {
    let m = vrc_raiz(texto)?;
    Ok(m.get("betas").and_then(Value::as_array).is_some_and(|b| b.iter().any(|x| x == VRC_LIMITADOR)))
}

/// O mesmo config.json com o limitador ligado ou desligado; o resto do arquivo fica igual.
pub fn vrc_com_limitador(texto: &str, ligar: bool) -> Result<String, String> {
    let mut m = vrc_raiz(texto)?;
    let betas = m.entry("betas").or_insert_with(|| json!([]));
    let Some(b) = betas.as_array_mut() else { return Err("\"betas\" no config.json do VRChat não é uma lista".into()) };
    b.retain(|x| x != VRC_LIMITADOR);
    if ligar {
        b.push(json!(VRC_LIMITADOR));
        for (k, v) in VRC_PS_PADRAO {
            m.entry(*k).or_insert(json!(v));
        }
    }
    serde_json::to_string_pretty(&Value::Object(m)).map_err(|e| e.to_string())
}

/// CPU AMD? A afinidade só é recomendada pelo VRChat em AMD (latência entre CCX).
pub fn cpu_amd() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        // CPUID existe em todo x86_64; o `unsafe` só é exigido em Rust antigo.
        #[allow(unused_unsafe)]
        let r = unsafe { std::arch::x86_64::__cpuid(0) };
        let mut v = [0u8; 12];
        v[..4].copy_from_slice(&r.ebx.to_le_bytes());
        v[4..8].copy_from_slice(&r.edx.to_le_bytes());
        v[8..].copy_from_slice(&r.ecx.to_le_bytes());
        &v == b"AuthenticAMD"
    }
    #[cfg(not(target_arch = "x86_64"))]
    false
}

fn erro(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::Other, msg.into())
}

/// Resultado do autoteste (a saída do teste ponta a ponta rodado na GPU do usuário).
pub fn ler_autoteste(saida: &str, codigo: Option<i32>) -> Value {
    let linhas: Vec<&str> = saida.lines().map(str::trim).collect();
    let depois = |l: &str, p: &str| l.find(p).map(|i| l[i + p.len()..].to_string());
    let skip = linhas.iter().find_map(|l| l.strip_prefix("SKIP: ")).map(str::to_string);
    let gpu = linhas
        .iter()
        .find_map(|l| depois(l, "FrameGen: iniciado (hardware, ").and_then(|r| r.rsplit_once(", feature").map(|(g, _)| g.to_string())))
        .or_else(|| linhas.iter().find_map(|l| l.strip_prefix("adaptador padrão: ").map(|r| r.split(" (vendor").next().unwrap_or(r).to_string())));
    let resumo: Vec<&str> = linhas.iter().copied().filter(|l| ["a) ", "b) ", "c) ", "d) ", "e) ", "f) ", "g) "].iter().any(|p| l.starts_with(p))).collect();
    let falhas: Vec<&str> = linhas.iter().copied().filter(|l| l.starts_with("FALHOU:")).collect();
    let ms: Vec<f64> = linhas
        .iter()
        .filter(|l| l.contains("FRAMEGEN estado=pronto"))
        .filter_map(|l| depois(l, "gpu_ms=")?.split_whitespace().next()?.parse::<f64>().ok())
        .filter(|x| *x > 0.0)
        .collect();
    // "(95%)" logo depois de "trocados pelo gerado" (b) e de "extras gerados" (c).
    let pct = |marca: &str| {
        linhas.iter().find_map(|l| depois(l, marca)?.split('(').nth(1)?.split('%').next()?.trim().parse::<f64>().ok())
    };
    json!({
        "passou": skip.is_none() && codigo == Some(0) && linhas.iter().any(|l| l.contains("TODOS OS TESTES PASSARAM")),
        "skip": skip,
        // Diagnóstico da escrita entre devices (cada jeito: ok ou o que veio diferente).
        "sonda": linhas.iter().find_map(|l| l.strip_prefix("sonda: ")),
        "gpu": gpu,
        "resumo": resumo,
        "falhas": falhas,
        "gpuMsMedia": (!ms.is_empty()).then(|| ms.iter().sum::<f64>() / ms.len() as f64),
        "gpuMsMax": ms.iter().cloned().fold(None, |m: Option<f64>, x| Some(m.map_or(x, |m| m.max(x)))),
        "pctRepetidos": pct("repetidos trocados pelo gerado"),
        "pctTrava": pct(" reais intactos, "),
        // d): jogo abaixo dos Hz sem repetir e sem trava (o caso do Pico Connect).
        "pctRelogio": pct("gerados no vsync vazio"),
        // g): manual 2x, o proxy trava o jogo na metade e gera no vsync do meio.
        "pctManual": linhas.iter().filter(|l| l.starts_with("g) ")).find_map(|l| depois(l, " com 2 Presents, ")?.split('(').nth(1)?.split('%').next()?.trim().parse::<f64>().ok()),
    })
}

/// "0.2.10-alpha" > "0.2.9-alpha": compara os números, não o texto.
pub fn versao_maior(a: &str, b: &str) -> bool {
    let n = |v: &str| v.split(|c: char| !c.is_ascii_digit()).filter(|x| !x.is_empty()).map(|x| x.parse::<u64>().unwrap_or(0)).take(3).collect::<Vec<_>>();
    n(a) > n(b)
}

pub fn agora() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

pub fn ler_json(p: &Path) -> Option<Value> {
    let t = fs::read_to_string(p).ok()?;
    serde_json::from_str(t.trim_start_matches('\u{feff}')).ok()
}

// ---------------------------------------------------------------------------
// Driver alvo

/// Drivers conhecidos: (trecho do nome do driver ou da pasta, rótulo, suporte).
/// "alvo" = foco atual; "streaming" = headset por streaming, deve funcionar, sem teste;
/// "nativo" = headset com reprojeção própria (sem ganho); o resto (trackers, Standable,
/// utilitários) não é headset e não aparece como alvo.
const CONHECIDOS: &[(&str, &str, &str)] = &[
    ("pico", "Pico Connect", "alvo"),
    ("alvr", "ALVR", "streaming"),
    ("vrlink", "Steam Link", "streaming"),
    ("virtualdesktop", "Virtual Desktop", "streaming"),
    ("virtual desktop", "Virtual Desktop", "streaming"),
    ("vdesktop", "Virtual Desktop", "streaming"),
    ("oculus", "Meta Link / Air Link", "streaming"),
    ("lighthouse", "Index / Vive", "nativo"),
    ("holographic", "Windows MR", "nativo"),
];

/// (rótulo, suporte) pelo nome do driver; na falta, pela pasta (ex.: "...\PICO Connect\...").
pub fn tipo(nome: &str, raiz: &Path) -> (String, &'static str) {
    let n = nome.to_lowercase();
    let r = raiz.to_string_lossy().to_lowercase();
    CONHECIDOS
        .iter()
        .find(|(k, _, _)| n.contains(k))
        .or_else(|| CONHECIDOS.iter().find(|(k, _, s)| *s != "nativo" && r.contains(k)))
        .map(|(_, r, s)| (r.to_string(), *s))
        .unwrap_or_else(|| (nome.to_string(), "desconhecido"))
}

/// Headset por streaming: os únicos onde o proxy faz sentido.
pub fn eh_streaming(d: &Driver) -> bool {
    matches!(tipo(&d.nome, &d.raiz).1, "alvo" | "streaming")
}

#[derive(Clone)]
pub struct Driver {
    pub nome: String,
    pub raiz: PathBuf,
    pub dll: PathBuf,
    /// Vem dentro da pasta do SteamVR (atualizações do SteamVR podem desfazer a instalação).
    pub embutido: bool,
}

impl Driver {
    pub fn json(&self) -> Value {
        let (rotulo, suporte) = tipo(&self.nome, &self.raiz);
        json!({ "nome": self.nome, "raiz": self.raiz, "dll": self.dll, "embutido": self.embutido,
                "rotulo": rotulo, "suporte": suporte })
    }
}

pub struct OpenVrPaths {
    pub runtime: Option<PathBuf>,
    pub config: Option<PathBuf>,
    pub log: Option<PathBuf>,
    pub drivers: Vec<Driver>,
}

fn primeiro(v: &Value, chave: &str) -> Option<PathBuf> {
    v.get(chave)?.as_array()?.first()?.as_str().map(PathBuf::from)
}

/// Lê %LOCALAPPDATA%\openvr\openvrpaths.vrpath: drivers externos (Pico Connect, ALVR...)
/// e, da pasta do SteamVR, os embutidos que reconhecemos (Steam Link, Meta Link...).
pub fn ler_openvrpaths(arquivo: &Path) -> OpenVrPaths {
    let v = ler_json(arquivo).unwrap_or(Value::Null);
    let mut drivers: Vec<Driver> = v
        .get("external_drivers")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(|d| driver_em(Path::new(d)))
        .collect();
    if let Some(Ok(pastas)) = primeiro(&v, "runtime").map(|r| fs::read_dir(r.join("drivers"))) {
        let mut embutidos: Vec<Driver> = pastas
            .filter_map(|e| Some(e.ok()?.path()))
            .filter(|p| p.is_dir())
            .map(|p| Driver { embutido: true, ..driver_em(&p) })
            .filter(|d| tipo(&d.nome, &d.raiz).1 != "desconhecido")
            .collect();
        embutidos.sort_by(|a, b| a.nome.cmp(&b.nome));
        drivers.extend(embutidos);
    }
    // O que o SteamVR carregou de fato na última vez (pega driver que não está no
    // openvrpaths.vrpath, como os que se registram só enquanto o app deles está aberto).
    let runtime = primeiro(&v, "runtime");
    if let Some(texto) = primeiro(&v, "log").and_then(|l| ler_fim(&l.join("vrserver.txt"), 2 * 1024 * 1024).ok()) {
        for raiz in raizes_no_log(&texto) {
            let chave = raiz.to_string_lossy().to_lowercase();
            if drivers.iter().any(|d| d.raiz.to_string_lossy().to_lowercase() == chave) {
                continue;
            }
            let embutido = runtime.as_ref().is_some_and(|r| chave.starts_with(&r.to_string_lossy().to_lowercase()));
            drivers.push(Driver { embutido, ..driver_em(&raiz) });
        }
    }
    OpenVrPaths { runtime, config: primeiro(&v, "config"), log: primeiro(&v, "log"), drivers }
}

/// Pastas de driver citadas no vrserver.txt: qualquer caminho "...\bin\win64\driver_X.dll"
/// (sem depender do formato exato das linhas do SteamVR).
pub fn raizes_no_log(texto: &str) -> Vec<PathBuf> {
    let mut r: Vec<PathBuf> = Vec::new();
    for linha in texto.lines() {
        // ASCII: mantém os índices iguais aos da linha original (acentos no caminho).
        let baixa = linha.to_ascii_lowercase().replace('/', "\\");
        let Some(fim) = baixa.find("\\bin\\win64\\driver_") else { continue };
        // Começo do caminho: a letra do drive ("C:\"); sem ela, depois da última aspa.
        let antes = &baixa[..fim];
        let Some(ini) = antes.rfind(":\\").and_then(|i| i.checked_sub(1)).or_else(|| antes.rfind('"').map(|q| q + 1)) else {
            continue;
        };
        let raiz = PathBuf::from(linha[ini..fim].trim_matches(|c: char| c == '"' || c == '\'' || c.is_whitespace()));
        if !r.contains(&raiz) {
            r.push(raiz);
        }
    }
    r
}

/// Nome do driver no manifesto. O SteamVR aceita comentários e vírgula sobrando no
/// JSON (jsoncpp); aqui, se o parser estrito falhar, pega o campo "name" direto.
fn nome_no_manifesto(raiz: &Path) -> Option<String> {
    let t = fs::read_to_string(raiz.join("driver.vrdrivermanifest")).ok()?;
    let t = t.trim_start_matches('\u{feff}');
    if let Some(n) = serde_json::from_str::<Value>(t).ok().and_then(|m| m.get("name")?.as_str().map(String::from)) {
        return Some(n);
    }
    let resto = &t[t.find("\"name\"")? + 6..];
    let resto = resto[resto.find(':')? + 1..].trim_start().strip_prefix('"')?;
    Some(resto[..resto.find('"')?].to_string())
}

/// O SteamVR carrega <raiz>\bin\win64\driver_<name>.dll, com <name> do manifesto.
/// Se esse arquivo não existe, vale a DLL de driver que estiver de fato na pasta.
pub fn driver_em(raiz: &Path) -> Driver {
    // Mesma forma para o que vem do openvrpaths, do vrserver.txt e do usuário: a lista
    // consegue marcar o driver escolhido comparando caminhos.
    let raiz = normalizar_raiz(&raiz.to_string_lossy());
    let raiz = raiz.as_path();
    let pasta = raiz.join("bin").join("win64");
    let mut nome = nome_no_manifesto(raiz)
        .unwrap_or_else(|| raiz.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
    let mut dll = pasta.join(format!("driver_{nome}.dll"));
    if !dll.exists() {
        let achadas: Vec<String> = fs::read_dir(&pasta)
            .into_iter()
            .flatten()
            .filter_map(|e| Some(e.ok()?.file_name().to_string_lossy().into_owned()))
            .filter(|f| {
                let f = f.to_lowercase();
                f.starts_with("driver_") && f.ends_with(".dll") && !f.ends_with("_real.dll")
            })
            .collect();
        let alvo = nome.to_lowercase();
        if let Some(f) = achadas.iter().find(|f| f.to_lowercase().contains(&alvo)).or(achadas.first()) {
            dll = pasta.join(f);
            nome = f[7..f.len() - 4].to_string();
        }
    }
    Driver { nome, raiz: raiz.to_path_buf(), dll, embutido: false }
}

/// Alvo padrão: Pico; senão o streaming que o usuário instalou (externo); por último
/// os de streaming do próprio SteamVR.
pub fn alvo_padrao(drivers: &[Driver]) -> Option<&Driver> {
    // Só headset por streaming: trackers e utilitários (Standable etc.) nunca são alvo.
    let prioridade = |d: &Driver| match (tipo(&d.nome, &d.raiz).1, d.embutido) {
        ("alvo", _) => 0,
        ("streaming", false) => 1,
        ("streaming", true) => 2,
        _ => 9,
    };
    drivers.iter().filter(|d| prioridade(d) < 9).min_by_key(|d| prioridade(d))
}

/// Pasta do driver a partir do que o usuário colou: tira aspas ("Copiar como caminho"
/// do Explorer põe), aceita a própria DLL ou a pasta bin\\win64 e sobe até a raiz.
pub fn normalizar_raiz(texto: &str) -> PathBuf {
    // Em texto (não PathBuf) para tratar "\\" igual no Windows e nos testes do Linux.
    let mut t = texto.trim().trim_matches('"').trim().trim_end_matches(['\\', '/']);
    let sobe = |t: &mut &str, cond: &dyn Fn(&str) -> bool| {
        let i = t.rfind(['\\', '/']).map(|i| i + 1).unwrap_or(0);
        if cond(&t[i..].to_ascii_lowercase()) {
            *t = t[..i].trim_end_matches(['\\', '/']);
        }
    };
    sobe(&mut t, &|n| n.ends_with(".dll") || n == "driver.vrdrivermanifest");
    sobe(&mut t, &|n| n == "win64");
    sobe(&mut t, &|n| n == "bin");
    PathBuf::from(t)
}

/// O driver escolhido no app (app.alvo) ou o padrão detectado.
pub fn alvo(cfg: &Map<String, Value>, ovr: &OpenVrPaths) -> Option<Driver> {
    match cfg.get("app.alvo").and_then(Value::as_str).filter(|s| !s.is_empty()) {
        Some(p) => Some(driver_em(Path::new(p))),
        None => alvo_padrao(&ovr.drivers).cloned(),
    }
}

// ---------------------------------------------------------------------------
// Instalação

#[derive(Debug, PartialEq)]
pub enum Estado {
    /// A DLL do driver não existe nesse caminho.
    SemDll,
    /// Driver original, proxy não instalado.
    Original,
    Instalado,
    /// Nossa DLL está lá, mas a _real sumiu: não dá para repassar.
    Quebrado,
    /// Tem _real, mas a DLL principal não é nossa: o Pico Connect se atualizou e a restaurou.
    Sobrescrito,
}

impl Estado {
    pub fn codigo(&self) -> &'static str {
        match self {
            Estado::SemDll => "sem_dll",
            Estado::Original => "original",
            Estado::Instalado => "instalado",
            Estado::Quebrado => "quebrado",
            Estado::Sobrescrito => "sobrescrito",
        }
    }
}

pub fn caminho_real(dll: &Path) -> PathBuf {
    let stem = dll.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    dll.with_file_name(format!("{stem}_real.dll"))
}

/// (é a nossa DLL, ela tem frame gen). O app pergunta a cada segundo; só relê a DLL
/// (que pode ter dezenas de MB) se ela mudou.
fn marcas(p: &Path) -> (bool, bool) {
    type Cache = Option<(PathBuf, u64, Option<SystemTime>, (bool, bool))>;
    static CACHE: Mutex<Cache> = Mutex::new(None);
    let (tam, mtime) = fs::metadata(p).map(|m| (m.len(), m.modified().ok())).unwrap_or((0, None));
    let mut c = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((cp, ct, cm, r)) = c.as_ref() {
        if cp == p && *ct == tam && *cm == mtime {
            return *r;
        }
    }
    let tem = |b: &[u8], m: &[u8]| b.windows(m.len()).any(|w| w == m);
    let r = fs::read(p).map(|b| (tem(&b, MARCA_PROXY), tem(&b, MARCA_FRAMEGEN))).unwrap_or((false, false));
    *c = Some((p.to_path_buf(), tam, mtime, r));
    r
}

fn eh_nossa(p: &Path) -> bool {
    marcas(p).0
}

/// O proxy instalado é diferente do que vem dentro deste app (abriu um app mais novo).
/// Compara os bytes, mas só relê a DLL quando ela muda.
pub fn proxy_diferente(dll: &Path, proxy: &[u8]) -> bool {
    type Cache = Option<(PathBuf, u64, Option<SystemTime>, bool)>;
    static CACHE: Mutex<Cache> = Mutex::new(None);
    if proxy.is_empty() || estado(dll) != Estado::Instalado {
        return false;
    }
    let (tam, mtime) = fs::metadata(dll).map(|m| (m.len(), m.modified().ok())).unwrap_or((0, None));
    let mut c = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((cp, ct, cm, r)) = c.as_ref() {
        if cp == dll && *ct == tam && *cm == mtime {
            return *r;
        }
    }
    let r = tam != proxy.len() as u64 || fs::read(dll).map(|b| b != proxy).unwrap_or(false);
    *c = Some((dll.to_path_buf(), tam, mtime, r));
    r
}

/// O proxy instalado nesse driver já sabe gerar frames (proxy antigo: precisa atualizar).
pub fn proxy_com_framegen(dll: &Path) -> bool {
    estado(dll) == Estado::Instalado && marcas(dll).1
}

pub fn estado(dll: &Path) -> Estado {
    if !dll.exists() {
        return Estado::SemDll;
    }
    match (eh_nossa(dll), caminho_real(dll).exists()) {
        (true, true) => Estado::Instalado,
        (true, false) => Estado::Quebrado,
        (false, true) => Estado::Sobrescrito,
        (false, false) => Estado::Original,
    }
}

/// Sem admin, Program Files não é gravável: aí o app pede elevação.
pub fn pode_escrever(dll: &Path) -> bool {
    fs::OpenOptions::new().append(true).open(dll).is_ok()
}

fn copiar_pasta(de: &Path, para: &Path) -> io::Result<()> {
    fs::create_dir_all(para)?;
    for e in fs::read_dir(de)? {
        let e = e?;
        let alvo = para.join(e.file_name());
        if e.file_type()?.is_dir() {
            copiar_pasta(&e.path(), &alvo)?;
        } else {
            fs::copy(e.path(), alvo)?;
        }
    }
    Ok(())
}

/// Põe `de` no lugar de `para`. Com a DLL carregada por algum programa (mesmo com o SteamVR
/// fechado), o Windows nega escrever por cima dela, até como administrador, mas deixa renomeá-la:
/// a velha sai do caminho como `.old-<hora>` e é apagada quando ninguém mais a usa.
fn trocar(de: &Path, para: &Path) -> io::Result<()> {
    let nome = para.file_name().unwrap_or_default().to_string_lossy().into_owned();
    if let Some(pasta) = para.parent() {
        for e in fs::read_dir(pasta).into_iter().flatten().flatten() {
            if e.file_name().to_string_lossy().starts_with(&format!("{nome}.old-")) {
                let _ = fs::remove_file(e.path());
            }
        }
    }
    if fs::rename(de, para).is_ok() {
        return Ok(());
    }
    let velha = para.with_file_name(format!("{nome}.old-{}", agora()));
    let fora = |e: io::Error| {
        let msg = format!("não consegui trocar {}: {e}. Outro programa está com ela aberta", para.display());
        io::Error::new(e.kind(), format!("{msg}: feche o programa do headset (PICO Connect, ALVR) e tente de novo"))
    };
    fs::rename(para, &velha).map_err(fora)?;
    if let Err(e) = fs::rename(de, para) {
        let _ = fs::rename(&velha, para);
        return Err(fora(e));
    }
    let _ = fs::remove_file(&velha);
    Ok(())
}

pub fn instalar(dll: &Path, proxy: &[u8], backups: &Path) -> io::Result<String> {
    if proxy.is_empty() {
        return Err(erro("este build do app não tem a DLL do proxy embutida (use o .exe da release)"));
    }
    match estado(dll) {
        Estado::SemDll => return Err(erro(format!("não achei {}", dll.display()))),
        Estado::Quebrado => {
            return Err(erro("a DLL original (_real) sumiu; reinstale o Pico Connect e tente de novo"))
        }
        Estado::Instalado => {
            // .tmp + rename: uma escrita pela metade não pode deixar uma DLL truncada
            // (ela pareceria "restaurada pelo Pico" e a próxima instalação a trataria como original).
            let tmp = dll.with_extension("dll.tmp");
            fs::write(&tmp, proxy)?;
            if let Err(e) = trocar(&tmp, dll) {
                let _ = fs::remove_file(&tmp);
                return Err(e);
            }
            return Ok("proxy atualizado".into());
        }
        Estado::Original | Estado::Sobrescrito => {}
    }
    let pasta = dll.parent().ok_or_else(|| erro("caminho inválido"))?;
    let backup = backups.join(format!("{}-{}", pasta.file_name().unwrap_or_default().to_string_lossy(), agora()));
    copiar_pasta(pasta, &backup)?;
    // Sobrescrito: a DLL atual é a original nova; ela substitui a _real antiga.
    let real = caminho_real(dll);
    fs::rename(dll, &real)?;
    if let Err(e) = fs::write(dll, proxy) {
        let _ = fs::rename(&real, dll);
        return Err(e);
    }
    Ok(format!("proxy instalado. Backup em {}", backup.display()))
}

pub fn desinstalar(dll: &Path) -> io::Result<String> {
    let real = caminho_real(dll);
    match estado(dll) {
        Estado::Instalado => {
            trocar(&real, dll)?;
            Ok("proxy removido, DLL original restaurada".into())
        }
        Estado::Sobrescrito => {
            fs::remove_file(&real)?;
            Ok("o Pico Connect já tinha restaurado a DLL; removi a cópia antiga (_real)".into())
        }
        _ => Err(erro("o proxy não está instalado nesse driver")),
    }
}

// ---------------------------------------------------------------------------
// ghostframe.log

/// O log mais recente entre os candidatos (pasta do driver ou %TEMP%) e sua idade em segundos.
pub fn log_mais_recente(candidatos: &[PathBuf]) -> Option<(PathBuf, f64)> {
    candidatos
        .iter()
        .filter_map(|p| Some((p.clone(), fs::metadata(p).ok()?.modified().ok()?)))
        .max_by_key(|(_, m)| *m)
        .map(|(p, m)| (p, m.elapsed().map(|d| d.as_secs_f64()).unwrap_or(0.0)))
}

/// O proxy escreve ao lado da DLL ou, sem permissão, em %TEMP% (ver src/proxy/log.cpp).
pub fn log_do_alvo(d: &Driver) -> Option<(PathBuf, f64)> {
    let pasta = d.dll.parent()?;
    log_mais_recente(&[pasta.join("ghostframe.log"), std::env::temp_dir().join("ghostframe.log")])
}

/// Últimos `max` bytes do arquivo, sem a primeira linha (que pode vir cortada).
pub fn ler_fim(p: &Path, max: u64) -> io::Result<String> {
    let mut f = fs::File::open(p)?;
    let tam = f.metadata()?.len();
    f.seek(SeekFrom::Start(tam.saturating_sub(max)))?;
    let mut b = Vec::new();
    f.read_to_end(&mut b)?;
    let t = String::from_utf8_lossy(&b).into_owned();
    Ok(if tam > max { t.split_once('\n').map(|x| x.1.to_string()).unwrap_or_default() } else { t })
}

/// Resume a última sessão do vrserver no log: status, avisos e séries de STATS.
pub fn ler_log(texto: &str) -> Value {
    // Linha: "[  pid         t] mensagem"
    let linhas: Vec<(&str, f64, &str)> = texto
        .lines()
        .filter_map(|l| {
            let (cab, msg) = l.strip_prefix('[')?.split_once("] ")?;
            let mut it = cab.split_whitespace();
            Some((it.next()?, it.next()?.parse().ok()?, msg))
        })
        .collect();
    // Dois processos podem escrever: o vrserver (Provider::Init) e, em drivers que
    // entregam o vídeo pelo plugin do compositor, o vrcompositor (CompositorPlugin::Init).
    let ultimo = |pat: &str| linhas.iter().rev().find(|l| l.2.starts_with(pat)).map(|l| l.0);
    let mut pids: Vec<&str> = [ultimo("Provider::Init"), ultimo("CompositorPlugin::Init")].into_iter().flatten().collect();
    pids.dedup();
    if pids.is_empty() {
        match linhas.iter().rev().find(|l| l.2.starts_with("STATS")).or(linhas.last()) {
            Some(l) => pids.push(l.0),
            None => return Value::Null,
        }
    }
    let pid = pids[0];
    // Só a última sessão de cada pid. Cada processo conta o tempo desde que abriu o log:
    // alinha os relógios pelo fim (as duas sessões estão vivas ao mesmo tempo).
    let sessao = |p: &str| -> Vec<(f64, &str)> {
        let ini = texto.rfind(&format!("(pid {p})")).unwrap_or(0);
        texto[ini..]
            .lines()
            .filter_map(|l| {
                let (c, msg) = l.strip_prefix('[')?.split_once("] ")?;
                let mut it = c.split_whitespace();
                (it.next()? == p).then_some(())?;
                Some((it.next()?.parse::<f64>().ok()?, msg))
            })
            .collect()
    };
    let sessoes: Vec<Vec<(f64, &str)>> = pids.iter().map(|p| sessao(p)).collect();
    let t_ref = sessoes[0].last().map(|l| l.0).unwrap_or(0.0);
    let mut linhas: Vec<(f64, &str)> = Vec::new();
    for s in &sessoes {
        let fim = s.last().map(|l| l.0).unwrap_or(0.0);
        linhas.extend(s.iter().map(|(t, m)| (t - fim + t_ref, *m)));
    }
    linhas.sort_by(|a, b| a.0.total_cmp(&b.0));

    let mut hmd = false;
    let mut video = Value::Null;
    let mut depth = Value::Null;
    let mut motion = Value::Null;
    let mut render = Value::Null;
    let mut avisos: Vec<String> = Vec::new();
    let mut stats = Map::new();
    let mut framegen = Value::Null;
    // Para diagnosticar o caminho de vídeo de um driver novo.
    let mut componentes: Vec<String> = Vec::new();
    let mut interfaces: Vec<String> = Vec::new();
    let mut sem_frames = false;
    for (t, m) in &linhas {
        const GC: &str = "::GetComponent(\"";
        if let Some(i) = m.find(GC).filter(|_| m.contains("IMPLEMENTADO")) {
            let disp = m[..i].trim_start_matches("Device(").trim_end_matches(')');
            let nome = m[i + GC.len()..].split('"').next().unwrap_or("");
            let item = format!("{disp}: {nome}");
            if !componentes.contains(&item) {
                componentes.push(item);
            }
        } else if let Some(x) = m.strip_prefix("Driver da Pico declara interface: ") {
            if !interfaces.iter().any(|i| i == x) {
                interfaces.push(x.to_string());
            }
        }
        if m.contains("nenhum frame passou pelo proxy") {
            sem_frames = true;
        }
        if m.contains("TrackedDeviceAdded") && m.contains("class=HMD") {
            hmd = true;
        } else if m.contains("embrulhando IVRDriverDirectModeComponent") {
            video = "DirectMode".into();
        } else if m.contains("embrulhando IVRVirtualDisplay") {
            video = "VirtualDisplay".into();
        } else if let Some(i) = m.find("depth=0x") {
            let tem = !m[i + 8..].starts_with("0 ");
            depth = (depth.as_bool().unwrap_or(false) || tem).into();
        } else if let Some(i) = m.find("motionSmoothing=") {
            motion = m[i + 16..].split(')').next().unwrap_or("").into();
        } else if m.contains("SetRecommendedRenderTargetSize") {
            render = m.rsplit(' ').next().unwrap_or("").into();
        } else if let Some(resto) = m.strip_prefix("FRAMEGEN estado=") {
            // FRAMEGEN estado=pronto reais=45.0/s gerados=45.0/s falhas=0 trava=1 gpu_ms=1.42
            let campo = |k: &str| {
                resto.split_whitespace().find_map(|kv| kv.strip_prefix(k)?.strip_prefix('=')?.trim_end_matches("/s").parse::<f64>().ok())
            };
            framegen = json!({
                "t": t,
                "estado": resto.split_whitespace().next().unwrap_or(""),
                "reais": campo("reais"), "gerados": campo("gerados"), "falhas": campo("falhas"),
                "trava": campo("trava"), "gpuMs": campo("gpu_ms"),
                "presents": campo("presents"), "repetidos": campo("repetidos"),
            });
            // Série para o gráfico: [t, gerados/s, trava].
            let serie = stats.entry("FRAMEGEN").or_insert_with(|| json!([]));
            let v = serie.as_array_mut().unwrap();
            v.push(json!([t, campo("gerados").unwrap_or(0.0), campo("trava").unwrap_or(0.0)]));
            if v.len() > 150 {
                v.remove(0);
            }
        } else if let Some(resto) = m.strip_prefix("STATS ") {
            let Some((antes, depois)) = resto.split_once("/s") else { continue };
            let Some((nome, taxa)) = antes.trim_end().rsplit_once(char::is_whitespace) else { continue };
            let campo = |k: &str| {
                depois
                    .split_whitespace()
                    .find_map(|kv| kv.strip_prefix(k)?.strip_prefix('=')?.parse::<f64>().ok())
                    .unwrap_or(0.0)
            };
            let serie = stats.entry(nome.trim()).or_insert_with(|| json!([]));
            let v = serie.as_array_mut().unwrap();
            v.push(json!([t, taxa.parse::<f64>().unwrap_or(0.0), campo("avg"), campo("min"), campo("max")]));
            if v.len() > 150 {
                v.remove(0);
            }
        }
        let fg_problema = m.starts_with("FRAMEGEN") && (m.contains("falhou") || m.contains("indisponível") || m.contains("desativado"));
        if (m.contains("!!") || m.contains("ERRO") || fg_problema) && !avisos.iter().any(|a| a == m) {
            avisos.push(m.to_string());
        }
    }
    let fim = linhas.len().saturating_sub(80);
    json!({
        "pid": pid,
        "t": linhas.last().map(|l| l.0).unwrap_or(0.0),
        "hmd": hmd, "video": video, "depth": depth, "motionSmoothing": motion, "render": render,
        "avisos": avisos, "stats": stats, "framegen": framegen,
        "componentes": componentes, "interfaces": interfaces, "semFrames": sem_frames,
        "linhas": linhas[fim..].iter().map(|(t, m)| format!("{t:9.3}  {m}")).collect::<Vec<_>>(),
    })
}

// ---------------------------------------------------------------------------
// ghostframe.ini  (formato compatível com GetPrivateProfileInt, que a DLL vai usar)

/// Lê o ini como mapa "secao.chave" -> valor, completando com os padrões.
pub fn config(texto: &str) -> Map<String, Value> {
    let mut m: Map<String, Value> = PADROES.iter().map(|(k, v)| (k.to_string(), json!(v))).collect();
    let mut secao = "";
    for l in texto.lines().map(str::trim) {
        if let Some(s) = l.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            secao = s;
        } else if let Some((k, v)) = l.split_once('=').filter(|_| !l.starts_with(';')) {
            m.insert(format!("{secao}.{}", k.trim()), json!(v.trim()));
        }
    }
    m
}

pub fn config_para_ini(m: &Map<String, Value>) -> String {
    let mut out = String::from("; ghostframeVR — gerado pelo app de controle. Tudo desligado = proxy só observa.\n");
    let mut secao = "";
    let mut chaves: Vec<_> = m.iter().collect();
    chaves.sort_by_key(|(k, _)| k.split_once('.').map(|x| x.0).unwrap_or("").to_string());
    for (k, v) in chaves {
        let (s, chave) = k.split_once('.').unwrap_or(("app", k));
        if s != secao {
            out.push_str(&format!("\n[{s}]\n"));
            secao = s;
        }
        out.push_str(&format!("{chave}={}\n", v.as_str().map(String::from).unwrap_or_else(|| v.to_string())));
    }
    out
}

// ---------------------------------------------------------------------------
// steamvr.vrsettings: trava de fps por jogo (Throttling)

/// Jogos que o SteamVR já conhece (blocos "steam.app.<id>" e "system.generated.<exe>")
/// e o VRChat. Nome vem do appmanifest da Steam quando dá.
pub fn jogos(vrsettings: &Value, steamapps: Option<&Path>) -> Vec<Value> {
    let mut chaves: Vec<&str> = vrsettings
        .as_object()
        .into_iter()
        .flatten()
        .map(|(k, _)| k.as_str())
        .filter(|k| k.starts_with("steam.app.") || k.starts_with("system.generated."))
        .collect();
    if !chaves.contains(&VRCHAT) {
        chaves.insert(0, VRCHAT);
    }
    chaves
        .iter()
        .map(|k| {
            let nome = if let Some(exe) = k.strip_prefix("system.generated.") {
                exe.to_string()
            } else {
                let id = k.trim_start_matches("steam.app.");
                steamapps
                    .and_then(|s| nome_do_app(&s.join(format!("appmanifest_{id}.acf"))))
                    .unwrap_or_else(|| if *k == VRCHAT { "VRChat".into() } else { format!("AppID {id}") })
            };
            json!({ "chave": k, "nome": nome })
        })
        .collect()
}

fn nome_do_app(acf: &Path) -> Option<String> {
    fs::read_to_string(acf).ok()?.lines().find_map(|l| {
        Some(l.trim().strip_prefix("\"name\"")?.trim().trim_matches('"').to_string())
    })
}

pub fn ler_steamvr(v: &Value, jogo: &str) -> Value {
    json!({
        "trava": v.get(jogo).and_then(|a| a.get("framesToThrottle")).and_then(Value::as_i64).unwrap_or(0),
        "motionSmoothing": v.pointer("/steamvr/motionSmoothing"),
        "home": v.pointer("/steamvr/enableHomeApp"),
        "supersample": v.pointer("/steamvr/supersampleScale"),
        "supersampleManual": v.pointer("/steamvr/supersampleManualOverride"),
    })
}

/// Grava no steamvr.vrsettings. Só com o SteamVR fechado: aberto, ele regrava o arquivo
/// ao sair, e o caminho certo é o IVRSettings (vr.rs). `None` remove a chave.
pub fn gravar_steamvr(arquivo: &Path, valores: &[(&str, &str, Option<Value>)]) -> io::Result<()> {
    let mut v = ler_json(arquivo).ok_or_else(|| erro(format!("não consegui ler {}", arquivo.display())))?;
    fs::copy(arquivo, arquivo.with_extension("vrsettings.ghostframe.bak"))?;
    let raiz = v.as_object_mut().ok_or_else(|| erro("steamvr.vrsettings inválido"))?;
    for (secao, chave, valor) in valores {
        let s = raiz.entry(*secao).or_insert_with(|| json!({}));
        let s = s.as_object_mut().ok_or_else(|| erro("steamvr.vrsettings inválido"))?;
        match valor {
            Some(x) => s.insert(chave.to_string(), x.clone()),
            None => s.remove(*chave),
        };
    }
    fs::write(arquivo, serde_json::to_string_pretty(&v)?)
}

/// frames = 0 remove a trava; 1 = metade da taxa do headset; 2 = um terço.
pub fn gravar_trava(arquivo: &Path, jogo: &str, frames: i64) -> io::Result<()> {
    let v = (frames != 0).then(|| json!(frames));
    gravar_steamvr(arquivo, &[(jogo, "framesToThrottle", v.clone()), (jogo, "additionalFramesToPredict", v)])
}

pub fn base64(dados: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(dados.len().div_ceil(3) * 4);
    for c in dados.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            s.push(if i <= c.len() { T[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' });
        }
    }
    s
}

/// Grava sem deixar o arquivo pela metade: o companion lê o ini a cada segundo.
pub fn gravar_atomico(arquivo: &Path, conteudo: &str) -> io::Result<()> {
    let tmp = arquivo.with_extension("tmp");
    fs::write(&tmp, conteudo)?;
    // O proxy (dentro do SteamVR) abre o ini por um instante a cada segundo, e no Windows
    // o rename falha enquanto ele está aberto: tenta de novo algumas vezes.
    let mut r = fs::rename(&tmp, arquivo);
    for _ in 0..10 {
        if r.is_ok() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(30));
        r = fs::rename(&tmp, arquivo);
    }
    if r.is_err() {
        let _ = fs::remove_file(&tmp); // não deixa .tmp órfão na pasta (a do VRChat inclusive)
    }
    r
}

// ---------------------------------------------------------------------------
#[cfg(test)]
mod testes {
    use super::*;

    #[test]
    fn le_o_resultado_do_autoteste() {
        // Saída real do teste ponta a ponta na CI (adaptador de software).
        let r = ler_autoteste(include_str!("amostras/autoteste_ci.txt"), Some(0));
        assert_eq!(r["passou"], true);
        assert_eq!(r["gpu"], "Microsoft Basic Render Driver");
        assert_eq!(r["pctRepetidos"], 95.0);
        assert_eq!(r["pctTrava"], 93.0);
        assert_eq!(r["resumo"].as_array().unwrap().len(), 6);
        let m = r["gpuMsMedia"].as_f64().unwrap();
        assert!((8.0..9.5).contains(&m), "{m}");
        assert_eq!(r["gpuMsMax"], 10.03);
        // Código de saída diferente de 0 ou SKIP: não passou.
        assert_eq!(ler_autoteste(include_str!("amostras/autoteste_ci.txt"), Some(1))["passou"], false);
        assert!(r["sonda"].is_null()); // a amostra da CI é de antes da sonda nova
        assert!(r["pctRelogio"].is_null()); // e de antes do cenário d
        let d = ler_autoteste("d) 60 quadros do jogo: 60 com 2 Presents no driver, 60 reais intactos, 58 gerados no vsync vazio (97%)\n", Some(0));
        assert_eq!(d["pctRelogio"], 97.0);
        let g = ler_autoteste("g) 50 quadros do jogo: 50 com 2 Presents, 49 gerados (98%); cada quadro real levou em média 59.7 ms (2 vsyncs = 60 ms)\n", Some(0));
        assert_eq!(g["pctManual"], 98.0);
        let d = ler_autoteste("sonda: escrita+espera=ok; relida=ok; conteúdo na criação=4096 de 16384 bytes diferentes; keyed mutex=ok\n", Some(0));
        assert_eq!(d["sonda"], "escrita+espera=ok; relida=ok; conteúdo na criação=4096 de 16384 bytes diferentes; keyed mutex=ok");
        let s = ler_autoteste("SKIP: sem adaptador de hardware que compartilhe texturas\n", Some(77));
        assert_eq!(s["passou"], false);
        assert_eq!(s["skip"], "sem adaptador de hardware que compartilhe texturas");
        assert!(s["gpuMsMedia"].is_null());
        let f = ler_autoteste("FALHOU: trocados * 10 >= repetidos * 8 (linha 551)\n1 FALHA(S)\n", Some(1));
        assert_eq!(f["falhas"].as_array().unwrap().len(), 1);
    }

    #[test]
    fn compara_versoes() {
        assert!(versao_maior("0.2.10-alpha", "0.2.9-alpha"));
        assert!(!versao_maior("0.2.9-alpha", "0.2.10-alpha"));
        assert!(!versao_maior("0.2.10-alpha", "0.2.10-alpha"));
        assert!(versao_maior("0.3.0", ""));
    }

    #[test]
    fn config_do_vrchat_limitador_de_particulas() {
        // Sem arquivo: liga criando só o necessário.
        assert_eq!(vrc_limitador(""), Ok(false));
        let novo = vrc_com_limitador("", true).unwrap();
        assert_eq!(vrc_limitador(&novo), Ok(true));
        let v: Value = serde_json::from_str(&novo).unwrap();
        assert_eq!(v["ps_max_particles"], 50000);
        // Arquivo do usuário: o resto fica igual, valores dele não são trocados pelos padrões.
        let dele = r#"{"cache_size": 60, "betas": ["outro"], "ps_max_particles": 1000}"#;
        let ligado: Value = serde_json::from_str(&vrc_com_limitador(dele, true).unwrap()).unwrap();
        assert_eq!(ligado["cache_size"], 60);
        assert_eq!(ligado["ps_max_particles"], 1000);
        assert_eq!(ligado["betas"], json!(["outro", "particle_system_limiter"]));
        // Ligar de novo não duplica; desligar tira só o nosso.
        let duas = vrc_com_limitador(&vrc_com_limitador(dele, true).unwrap(), true).unwrap();
        assert_eq!(serde_json::from_str::<Value>(&duas).unwrap()["betas"], json!(["outro", "particle_system_limiter"]));
        let desligado: Value = serde_json::from_str(&vrc_com_limitador(&duas, false).unwrap()).unwrap();
        assert_eq!(desligado["betas"], json!(["outro"]));
        assert_eq!(vrc_limitador(&serde_json::to_string(&desligado).unwrap()), Ok(false));
        // Inválido: erro, nunca regrava.
        assert!(vrc_limitador("{ quebrado").is_err());
        assert!(vrc_com_limitador("{ quebrado", true).is_err());
        assert!(vrc_com_limitador("[1, 2]", true).is_err());
        assert!(vrc_com_limitador(r#"{"betas": "x"}"#, true).is_err());
        // Com BOM (o VRChat aceita): lê normal.
        assert_eq!(vrc_limitador("\u{feff}{\"betas\": [\"particle_system_limiter\"]}"), Ok(true));
    }

    fn tmp(nome: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("gf-teste-{nome}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn acha_driver_pico_e_instala_desinstala() {
        let raiz = tmp("inst");
        let drv = raiz.join("PICO Connect").join("driver");
        fs::create_dir_all(drv.join("bin/win64")).unwrap();
        fs::write(drv.join("driver.vrdrivermanifest"), r#"{"name": "pico"}"#).unwrap();
        fs::write(drv.join("bin/win64/driver_pico.dll"), b"ORIGINAL").unwrap();
        let vrpath = raiz.join("openvrpaths.vrpath");
        let lista = json!({"config": ["C:/Steam/config"], "external_drivers": ["/outro/driver", drv]});
        fs::write(&vrpath, format!("\u{feff}{lista}")).unwrap();

        let ovr = ler_openvrpaths(&vrpath);
        assert_eq!(ovr.config, Some(PathBuf::from("C:/Steam/config")));
        let d = alvo_padrao(&ovr.drivers).unwrap();
        assert_eq!(d.nome, "pico");
        assert_eq!(d.json()["rotulo"], "Pico Connect");
        assert_eq!(estado(&d.dll), Estado::Original);

        let proxy = b"xx ghostframe proxy iniciado xx";
        assert!(instalar(&d.dll, b"", &raiz.join("bk")).is_err());
        instalar(&d.dll, proxy, &raiz.join("bk")).unwrap();
        assert_eq!(estado(&d.dll), Estado::Instalado);
        assert_eq!(fs::read(caminho_real(&d.dll)).unwrap(), b"ORIGINAL");
        assert_eq!(fs::read_dir(raiz.join("bk")).unwrap().count(), 1);

        // Atualizar o proxy troca só a DLL e limpa a velha que ficou de uma troca com ela em uso.
        let pasta = d.dll.parent().unwrap().to_path_buf();
        fs::write(pasta.join("driver_pico.dll.old-1"), b"VELHA").unwrap();
        instalar(&d.dll, b"xx ghostframe proxy iniciado v2", &raiz.join("bk")).unwrap();
        assert_eq!(fs::read(&d.dll).unwrap(), b"xx ghostframe proxy iniciado v2");
        let mut nomes: Vec<_> = fs::read_dir(&pasta).unwrap().map(|e| e.unwrap().file_name()).collect();
        nomes.sort();
        assert_eq!(nomes, ["driver_pico.dll", "driver_pico_real.dll"]);

        // Atualização do Pico Connect devolve uma DLL nova: reinstalar troca a _real.
        fs::write(&d.dll, b"ORIGINAL2").unwrap();
        assert_eq!(estado(&d.dll), Estado::Sobrescrito);
        instalar(&d.dll, proxy, &raiz.join("bk")).unwrap();
        assert_eq!(fs::read(caminho_real(&d.dll)).unwrap(), b"ORIGINAL2");

        desinstalar(&d.dll).unwrap();
        assert_eq!(estado(&d.dll), Estado::Original);
        assert_eq!(fs::read(&d.dll).unwrap(), b"ORIGINAL2");
        assert!(desinstalar(&d.dll).is_err());
    }

    #[test]
    fn detecta_drivers_embutidos_e_escolhe_streaming_externo() {
        let raiz = tmp("emb");
        let mk = |pasta: &Path, nome: &str| {
            fs::create_dir_all(pasta.join("bin/win64")).unwrap();
            fs::write(pasta.join("driver.vrdrivermanifest"), format!(r#"{{"name": "{nome}"}}"#)).unwrap();
        };
        for n in ["lighthouse", "vrlink", "gamepad"] {
            mk(&raiz.join("SteamVR/drivers").join(n), n);
        }
        mk(&raiz.join("ALVR"), "alvr_server");
        let vrpath = raiz.join("openvrpaths.vrpath");
        fs::write(&vrpath, json!({"runtime": [raiz.join("SteamVR")], "external_drivers": [raiz.join("ALVR")]}).to_string()).unwrap();
        let ovr = ler_openvrpaths(&vrpath);
        let nomes: Vec<_> = ovr.drivers.iter().map(|d| (d.nome.as_str(), d.embutido)).collect();
        assert_eq!(nomes, [("alvr_server", false), ("lighthouse", true), ("vrlink", true)]);
        assert_eq!(alvo_padrao(&ovr.drivers).unwrap().nome, "alvr_server");
        assert_eq!(alvo_padrao(&ovr.drivers[1..]).unwrap().nome, "vrlink");
        assert!(alvo_padrao(&ovr.drivers[1..2]).is_none());
        assert!(!eh_streaming(&ovr.drivers[1]) && eh_streaming(&ovr.drivers[2]));
    }

    #[test]
    fn acha_a_dll_real_e_drivers_do_vrserver_txt() {
        let raiz = tmp("dll");
        // Manifesto com comentário e vírgula sobrando (o SteamVR aceita) e DLL com outro nome.
        let pico = raiz.join("PICO Connect").join("driver");
        fs::create_dir_all(pico.join("bin/win64")).unwrap();
        fs::write(pico.join("driver.vrdrivermanifest"), "{\n // driver\n \"name\" : \"PicoStreaming\",\n}").unwrap();
        fs::write(pico.join("bin/win64/driver_picostreaming.dll"), b"x").unwrap();
        fs::write(pico.join("bin/win64/driver_picostreaming_real.dll"), b"x").unwrap();
        let d = driver_em(&pico);
        // No Windows o nome do manifesto já acha o arquivo; aqui (diferencia maiúsculas) vem do arquivo.
        assert!(d.nome.eq_ignore_ascii_case("PicoStreaming"), "{}", d.nome);
        assert!(d.dll.exists() && !d.dll.to_string_lossy().ends_with("_real.dll"));
        assert_eq!(d.json()["rotulo"], "Pico Connect");

        // Standable no openvrpaths, Pico só no vrserver.txt: o alvo é o Pico.
        let std_ = raiz.join("Standable");
        fs::create_dir_all(std_.join("bin/win64")).unwrap();
        fs::write(std_.join("driver.vrdrivermanifest"), r#"{"name": "standable"}"#).unwrap();
        let logs = raiz.join("logs");
        fs::create_dir_all(&logs).unwrap();
        let dll_pico = pico.join("bin").join("win64").join("driver_picostreaming.dll");
        fs::write(logs.join("vrserver.txt"), format!("Wed Oct 31 2026 12:00:00.000 [Info] - Loaded server driver PicoStreaming from \"{}\"\n", dll_pico.display())).unwrap();
        let vrpath = raiz.join("openvrpaths.vrpath");
        fs::write(&vrpath, json!({"log": [logs], "external_drivers": [std_]}).to_string()).unwrap();
        let ovr = ler_openvrpaths(&vrpath);
        assert_eq!(ovr.drivers.len(), 2);
        assert!(!eh_streaming(&ovr.drivers[0]));
        assert_eq!(alvo_padrao(&ovr.drivers).unwrap().raiz, pico);

        let r = raizes_no_log("x C:\\Program Files\\PICO Connect\\driver\\bin\\win64\\driver_pico.dll y\nnada aqui\n");
        assert_eq!(r, [PathBuf::from("C:\\Program Files\\PICO Connect\\driver")]);
    }

    #[test]
    fn normaliza_o_caminho_colado() {
        let raiz = PathBuf::from("C:\\Program Files\\PICO Connect\\driver");
        for colado in [
            "\"C:\\Program Files\\PICO Connect\\driver\"",
            "  C:\\Program Files\\PICO Connect\\driver\\ ",
            "C:\\Program Files\\PICO Connect\\driver\\bin\\win64",
            "\"C:\\Program Files\\PICO Connect\\driver\\bin\\win64\\driver_pico.dll\"",
            "C:\\Program Files\\PICO Connect\\driver\\driver.vrdrivermanifest",
        ] {
            assert_eq!(normalizar_raiz(colado).to_string_lossy().replace('/', "\\"), raiz.to_string_lossy(), "{colado}");
        }
    }

    #[test]
    fn le_log_da_ultima_sessao_do_vrserver() {
        let log = "\n===== nova sessao (pid 10) =====\n\
[    10     0.001] STATS DirectMode::Present            10.0/s  dt(ms) avg=100.00 min=90.00 max=110.00\n\
\n===== nova sessao (pid 22) log=C:/x/ghostframe.log =====\n\
[    22     0.010] Provider::Init -> 0\n\
[    22     0.020] Host::TrackedDeviceAdded serial=PA7 class=HMD\n\
[    22     0.025] Device(HMD)::GetComponent(\"IVRDriverDirectModeComponent_009\") -> IMPLEMENTADO\n\
[    22     0.026] Device(HMD)::GetComponent(\"IVRCameraComponent_003\") -> null\n\
[    22     0.027] Driver da Pico declara interface: IVRServerDriverHost_006\n\
[    22     0.030]   -> embrulhando IVRDriverDirectModeComponent\n\
[    22     0.040] Host::SetRecommendedRenderTargetSize dev=0 2160x2160\n\
[    22     0.050] DirectMode::SubmitLayer #0 eye=0 tex=0x1 depth=0x0 bounds=[0 0 1 1]\n\
[    22     0.060] DirectMode::SubmitLayer #0 eye=1 tex=0x2 depth=0x7c bounds=[0 0 1 1]\n\
[    22     0.070] DirectMode::GetFrameTiming #0 flagsIn=0x0(motionSmoothing=off) -> presents=1\n\
[    33     0.075] HmdDriverFactory(\"IVRWatchdogProvider_001\") -> ok\n\
[    22     2.000] STATS DirectMode::Present            71.9/s  dt(ms) avg=13.91 min=12.00 max=20.50\n\
[    22     2.100]   !! versão diferente da que conhecemos\n\
[    22     2.200] FRAMEGEN falhou: Gerar (repassando o original)\n\
[    22     4.000] FRAMEGEN estado=pronto reais=45.0/s gerados=44.5/s falhas=1 trava=1 gpu_ms=1.42\n\
\n===== nova sessao (pid 40) log=C:/x/ghostframe.log =====\n\
[    40     0.010] CompositorPlugin::Init -> 0\n\
[    40     0.020] CompositorPlugin::GetComponent(\"IVRDriverDirectModeComponent_009\") -> IMPLEMENTADO\n\
[    40     1.500] STATS DirectMode::Present            44.0/s  dt(ms) avg=22.70 min=21.00 max=25.00\n";
        let r = ler_log(log);
        assert_eq!(r["pid"], "22");
        assert_eq!(r["hmd"], true);
        assert_eq!(r["video"], "DirectMode");
        assert_eq!(r["depth"], true);
        assert_eq!(r["motionSmoothing"], "off");
        assert_eq!(r["render"], "2160x2160");
        assert_eq!(r["stats"]["DirectMode::Present"], json!([[2.0, 71.9, 13.91, 12.0, 20.5], [4.0, 44.0, 22.7, 21.0, 25.0]]));
        assert_eq!(r["avisos"].as_array().unwrap().len(), 2);
        assert_eq!(r["componentes"], json!(["HMD: IVRDriverDirectModeComponent_009", "CompositorPlugin: IVRDriverDirectModeComponent_009"]));
        // O ponto do compositor (t 1.5 no relógio dele) entra alinhado pelo fim com o vrserver (4.0).
        assert_eq!(r["interfaces"], json!(["IVRServerDriverHost_006"]));
        assert_eq!(r["semFrames"], false);
        assert_eq!(r["framegen"]["estado"], "pronto");
        assert_eq!(r["framegen"]["gerados"], 44.5);
        assert_eq!(r["framegen"]["trava"], 1.0);
        assert_eq!(r["framegen"]["gpuMs"], 1.42);
        assert_eq!(ler_log(""), Value::Null);
    }

    #[test]
    fn base64_padrao() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn ini_ida_e_volta() {
        let mut c = config("[framegen]\nativo=1\n; comentario=9\n");
        assert_eq!(c["framegen.ativo"], "1");
        assert_eq!(c["framegen.multiplicador"], "2");
        assert!(!c.contains_key(".; comentario"));
        c.insert("framegen.multiplicador".into(), json!("3"));
        let volta = config(&config_para_ini(&c));
        assert_eq!(volta, c);
    }

    #[test]
    fn trava_do_vrchat_preserva_o_resto() {
        let d = tmp("vrs");
        let f = d.join("steamvr.vrsettings");
        fs::write(&f, r#"{"steamvr": {"supersampleScale": 1.2}, "steam.app.438100": {"motionSmoothingOverride": 1},
            "steam.app.620980": {}, "system.generated.jogo.exe": {}}"#).unwrap();
        fs::write(d.join("appmanifest_620980.acf"), "\"AppState\"\n{\n\t\"appid\"\t\t\"620980\"\n\t\"name\"\t\t\"Beat Saber\"\n}").unwrap();
        let v = ler_json(&f).unwrap();
        let nomes: Vec<_> = jogos(&v, Some(&d)).iter().map(|j| j["nome"].as_str().unwrap().to_string()).collect();
        assert_eq!(nomes, ["VRChat", "Beat Saber", "jogo.exe"]);

        gravar_trava(&f, "steam.app.620980", 1).unwrap();
        let v = ler_json(&f).unwrap();
        assert_eq!(ler_steamvr(&v, "steam.app.620980")["trava"], 1);
        assert_eq!(ler_steamvr(&v, VRCHAT)["trava"], 0);
        assert_eq!(v[VRCHAT]["motionSmoothingOverride"], 1);
        assert_eq!(v["steamvr"]["supersampleScale"], 1.2);
        gravar_trava(&f, "steam.app.620980", 0).unwrap();
        assert_eq!(ler_steamvr(&ler_json(&f).unwrap(), "steam.app.620980")["trava"], 0);
    }
}
