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
pub const VRCHAT: &str = "steam.app.438100";

/// Opções do ghostframe.ini e seus padrões. Tudo desligado por padrão.
pub const PADROES: &[(&str, &str)] = &[
    ("framegen.ativo", "0"),
    ("framegen.modo", "auto"),
    ("framegen.multiplicador", "2"),
    ("framegen.qualidade", "1"),
    ("framegen.correcao_pose", "1"),
    ("hud.ativo", "0"),
    ("hud.ancora", "pe"),
    ("hud.olhar", "1"),
    ("hud.tamanho", "12"),
    ("hud.opacidade", "90"),
    ("hud.x", "0"),
    ("hud.y", "0"),
    ("hud.z", "0"),
    ("hud.itens", "fps,frames,ft,graf"),
    ("gravacao.ativo", "0"),
    ("gravacao.segundos", "5"),
    ("app.alvo", ""),
    ("app.jogo", VRCHAT),
    ("app.autostart", "0"),
];

fn erro(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::Other, msg.into())
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

/// Drivers conhecidos: (trecho do nome no manifesto, rótulo, suporte).
/// "alvo" = foco atual; "streaming" = deve funcionar, sem teste ainda;
/// "nativo" = headset com reprojeção própria, sem ganho com o proxy.
const CONHECIDOS: &[(&str, &str, &str)] = &[
    ("pico", "Pico Connect", "alvo"),
    ("alvr", "ALVR", "streaming"),
    ("vrlink", "Steam Link", "streaming"),
    ("oculus", "Meta Link / Air Link", "streaming"),
    ("lighthouse", "Index / Vive", "nativo"),
    ("holographic", "Windows MR", "nativo"),
];

pub fn tipo(nome: &str) -> (String, &'static str) {
    let n = nome.to_lowercase();
    CONHECIDOS
        .iter()
        .find(|(k, _, _)| n.contains(k))
        .map(|(_, r, s)| (r.to_string(), *s))
        .unwrap_or_else(|| (nome.to_string(), "desconhecido"))
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
        let (rotulo, suporte) = tipo(&self.nome);
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
            .filter(|d| tipo(&d.nome).1 != "desconhecido")
            .collect();
        embutidos.sort_by(|a, b| a.nome.cmp(&b.nome));
        drivers.extend(embutidos);
    }
    OpenVrPaths { runtime: primeiro(&v, "runtime"), config: primeiro(&v, "config"), log: primeiro(&v, "log"), drivers }
}

/// O SteamVR carrega <raiz>\bin\win64\driver_<name>.dll, com <name> do manifesto.
pub fn driver_em(raiz: &Path) -> Driver {
    let nome = ler_json(&raiz.join("driver.vrdrivermanifest"))
        .and_then(|m| m.get("name")?.as_str().map(String::from))
        .unwrap_or_else(|| raiz.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
    let dll = raiz.join("bin").join("win64").join(format!("driver_{nome}.dll"));
    Driver { nome, raiz: raiz.to_path_buf(), dll, embutido: false }
}

/// Alvo padrão: Pico; senão o streaming que o usuário instalou (externo);
/// senão um externo desconhecido; por último os de streaming do próprio SteamVR.
pub fn alvo_padrao(drivers: &[Driver]) -> Option<&Driver> {
    let prioridade = |d: &Driver| match (tipo(&d.nome).1, d.embutido) {
        ("alvo", _) => 0,
        ("streaming", false) => 1,
        ("desconhecido", false) => 2,
        ("streaming", true) => 3,
        _ => 9,
    };
    drivers.iter().filter(|d| prioridade(d) < 9).min_by_key(|d| prioridade(d))
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

/// O app pergunta a cada segundo; só relê a DLL (que pode ter dezenas de MB) se ela mudou.
fn eh_nossa(p: &Path) -> bool {
    type Cache = Option<(PathBuf, u64, Option<SystemTime>, bool)>;
    static CACHE: Mutex<Cache> = Mutex::new(None);
    let (tam, mtime) = fs::metadata(p).map(|m| (m.len(), m.modified().ok())).unwrap_or((0, None));
    let mut c = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((cp, ct, cm, r)) = c.as_ref() {
        if cp == p && *ct == tam && *cm == mtime {
            return *r;
        }
    }
    let r = fs::read(p).map(|b| b.windows(MARCA_PROXY.len()).any(|w| w == MARCA_PROXY)).unwrap_or(false);
    *c = Some((p.to_path_buf(), tam, mtime, r));
    r
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
            fs::write(dll, proxy)?;
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
            fs::rename(&real, dll)?;
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
    // O processo que importa é o vrserver: o último que logou Provider/STATS.
    let Some(pid) = linhas
        .iter()
        .rev()
        .find(|l| l.2.starts_with("STATS") || l.2.starts_with("Provider::Init"))
        .or(linhas.last())
        .map(|l| l.0)
    else {
        return Value::Null;
    };
    // Só a última sessão desse pid.
    let cab = format!("(pid {pid})");
    let ini = texto.rfind(&cab).unwrap_or(0);
    let linhas: Vec<_> = texto[ini..]
        .lines()
        .filter_map(|l| {
            let (c, msg) = l.strip_prefix('[')?.split_once("] ")?;
            let mut it = c.split_whitespace();
            (it.next()? == pid).then_some(())?;
            Some((it.next()?.parse::<f64>().ok()?, msg))
        })
        .collect();

    let mut hmd = false;
    let mut video = Value::Null;
    let mut depth = Value::Null;
    let mut motion = Value::Null;
    let mut render = Value::Null;
    let mut avisos: Vec<String> = Vec::new();
    let mut stats = Map::new();
    for (t, m) in &linhas {
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
        if (m.contains("!!") || m.contains("ERRO")) && !avisos.iter().any(|a| a == m) {
            avisos.push(m.to_string());
        }
    }
    let fim = linhas.len().saturating_sub(80);
    json!({
        "pid": pid,
        "t": linhas.last().map(|l| l.0).unwrap_or(0.0),
        "hmd": hmd, "video": video, "depth": depth, "motionSmoothing": motion, "render": render,
        "avisos": avisos, "stats": stats,
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

// ---------------------------------------------------------------------------
// Processos (só olhamos os nomes; nunca abrimos o processo do jogo)

pub fn processos(s: &mut sysinfo::System) -> Value {
    s.refresh_processes(sysinfo::ProcessesToUpdate::All, true);
    let nomes: Vec<String> = s.processes().values().map(|p| p.name().to_string_lossy().to_lowercase()).collect();
    let tem = |f: &dyn Fn(&str) -> bool| nomes.iter().any(|n| f(n));
    json!({ "steamvr": tem(&|n| n.starts_with("vrserver")) })
}

// ---------------------------------------------------------------------------
#[cfg(test)]
mod testes {
    use super::*;

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
    }

    #[test]
    fn le_log_da_ultima_sessao_do_vrserver() {
        let log = "\n===== nova sessao (pid 10) =====\n\
[    10     0.001] STATS DirectMode::Present            10.0/s  dt(ms) avg=100.00 min=90.00 max=110.00\n\
\n===== nova sessao (pid 22) log=C:/x/ghostframe.log =====\n\
[    22     0.010] Provider::Init -> 0\n\
[    22     0.020] Host::TrackedDeviceAdded serial=PA7 class=HMD\n\
[    22     0.030]   -> embrulhando IVRDriverDirectModeComponent\n\
[    22     0.040] Host::SetRecommendedRenderTargetSize dev=0 2160x2160\n\
[    22     0.050] DirectMode::SubmitLayer #0 eye=0 tex=0x1 depth=0x0 bounds=[0 0 1 1]\n\
[    22     0.060] DirectMode::SubmitLayer #0 eye=1 tex=0x2 depth=0x7c bounds=[0 0 1 1]\n\
[    22     0.070] DirectMode::GetFrameTiming #0 flagsIn=0x0(motionSmoothing=off) -> presents=1\n\
[    33     0.075] HmdDriverFactory(\"IVRWatchdogProvider_001\") -> ok\n\
[    22     2.000] STATS DirectMode::Present            71.9/s  dt(ms) avg=13.91 min=12.00 max=20.50\n\
[    22     2.100]   !! versão diferente da que conhecemos\n";
        let r = ler_log(log);
        assert_eq!(r["pid"], "22");
        assert_eq!(r["hmd"], true);
        assert_eq!(r["video"], "DirectMode");
        assert_eq!(r["depth"], true);
        assert_eq!(r["motionSmoothing"], "off");
        assert_eq!(r["render"], "2160x2160");
        assert_eq!(r["stats"]["DirectMode::Present"], json!([[2.0, 71.9, 13.91, 12.0, 20.5]]));
        assert_eq!(r["avisos"].as_array().unwrap().len(), 1);
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
