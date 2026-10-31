//! Ponte cliente OpenVR (o "companion"): fala com um SteamVR que já está rodando,
//! como app à parte (nunca no processo do jogo). Info do headset, configs ao vivo,
//! frame timing do compositor, HUD em overlay e autostart junto com o SteamVR.
//! O C++ de native/gf_openvr.cpp faz o papel do openvr_api.dll.
//!
//! Tudo numa thread só: `Vr` não é Send de propósito. Erro vira Result/None; se o
//! SteamVR morrer no meio, as chamadas falham e `deve_sair` avisa para soltar a sessão.
use serde_json::json;
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::{Path, PathBuf};

pub const APP_KEY: &str = "io.github.luizhc06.ghostframevr";
const MAX_QUADROS: usize = 128; // igual a kMaxQuadros no C++

#[repr(C)]
struct GfVr {
    _p: [u8; 0],
}

/// Espelho de GfQuadro (gf_openvr.cpp): um frame do jogo no histórico do compositor.
/// Tempos em ms; `wgp_chamado`/`poses_prontas`/`frame_pronto` são relativos a `t` (s).
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Quadro {
    t: f64,
    intervalo: f32,
    wgp_chamado: f32,
    poses_prontas: f32,
    frame_pronto: f32,
    gpu_pre: f32,
    gpu_pos: f32,
    apresentacoes: u32,
    perdidos: u32,
    flags: u32,
}

#[cfg(windows)]
type CharSo = u16;
#[cfg(not(windows))]
type CharSo = u8;

extern "C" {
    fn gf_vr_conectar(dll: *const CharSo, msg: *mut c_char, tam: u32) -> *mut GfVr;
    fn gf_vr_desconectar(v: *mut GfVr);
    fn gf_vr_info_hmd(v: *mut GfVr, hz: *mut f32, larg: *mut u32, alt: *mut u32, modelo: *mut c_char,
                      fabricante: *mut c_char, tam: u32) -> bool;
    fn gf_vr_ler(v: *mut GfVr, secao: *const c_char, chave: *const c_char, tipo: i32, valor: *mut f64) -> i32;
    fn gf_vr_gravar(v: *mut GfVr, secao: *const c_char, chave: *const c_char, tipo: i32, valor: f64) -> i32;
    fn gf_vr_nome_erro(v: *mut GfVr, qual: c_char, e: i32) -> *const c_char;
    fn gf_vr_quadros(v: *mut GfVr, saida: *mut Quadro, max: u32) -> u32;
    fn gf_vr_deve_sair(v: *mut GfVr) -> bool;
    fn gf_vr_autostart(v: *mut GfVr, manifesto: *const c_char, chave: *const c_char, ligado: bool) -> i32;
    fn gf_overlay_criar(v: *mut GfVr, chave: *const c_char, nome: *const c_char, h: *mut u64) -> i32;
    fn gf_overlay_imagem(v: *mut GfVr, h: u64, rgba: *const c_void, larg: u32, alt: u32) -> i32;
    fn gf_overlay_posicionar(v: *mut GfVr, h: u64, ancora: i32, m: *const f32, largura: f32, alpha: f32) -> i32;
    fn gf_overlay_mostrar(v: *mut GfVr, h: u64, visivel: bool) -> i32;
    fn gf_overlay_alpha(v: *mut GfVr, h: u64, alpha: f32) -> i32;
    fn gf_vr_poses(v: *mut GfVr, mao: i32, hmd: *mut f32, ctrl: *mut f32) -> bool;
}

#[derive(Debug, Clone)]
pub struct InfoHmd {
    pub hz: f32,
    pub largura: u32,
    pub altura: u32,
    pub modelo: String,
    pub fabricante: String,
}

/// Média da última ~1 s do histórico do compositor (um item por frame entregue pelo jogo).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Timing {
    /// Frames que o jogo entregou por segundo.
    pub fps_jogo: f32,
    /// CPU da thread de render do jogo: das poses prontas ao 2º Submit
    /// (o "app CPU" do gráfico de frame timing do SteamVR).
    pub ms_cpu: f32,
    /// GPU do jogo: m_flPreSubmitGpuMs + m_flPostSubmitGpuMs (sem o compositor).
    pub ms_gpu: f32,
    /// Custo do frame do jogo, para decidir se ele aguenta refresh/N:
    /// `max(média(intervalo − bloqueado), ms_gpu)`, onde intervalo =
    /// m_flClientFrameIntervalMs (entre chamadas de WaitGetPoses) e bloqueado =
    /// max(0, m_flNewPosesReadyMs − m_flWaitGetPosesCalledMs) (tempo preso no WaitGetPoses).
    /// A trava do SteamVR (framesToThrottle / meia taxa) só alonga o tempo preso no
    /// WaitGetPoses, então o que sobra é o trabalho do jogo, igual a 1/1, 1/2 ou 1/3.
    /// Limite: em engine com main thread em paralelo à de render (Unity multithread,
    /// ex.: VRChat), a main thread trabalha enquanto a de render espera no WaitGetPoses;
    /// com trava esse trabalho some da medida, que vira um piso. Destravar exige testar.
    pub ms_trabalho: f32,
    /// Apresentações além das planejadas (a trava já conta como planejada).
    pub reprojetados: u32,
    /// Vezes que o frame anterior foi escaneado de novo sem reprojeção.
    pub perdidos: u32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Valor {
    Bool(bool),
    Int(i32),
    Float(f32),
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Tipo {
    Bool,
    Int,
    Float,
}

/// Onde o overlay fica preso. A ordem é a do `ancora` em gf_overlay_posicionar.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Ancora {
    PulsoEsquerdo,
    PulsoDireito,
    /// Relativo ao headset.
    Visao,
    /// Absoluto no espaço standing.
    Mundo,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Overlay(u64);

#[derive(Debug)]
pub struct Vr {
    p: *mut GfVr, // ponteiro cru: deixa Vr !Send, preso à thread do companion
}

fn texto(b: &[u8]) -> String {
    CStr::from_bytes_until_nul(b).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default()
}

fn cstr(s: &str) -> Result<CString, String> {
    CString::new(s).map_err(|_| format!("texto com NUL: {s:?}"))
}

fn biblioteca_cliente(runtime: &Path) -> PathBuf {
    if cfg!(windows) {
        runtime.join("bin").join("win64").join("vrclient_x64.dll")
    } else {
        runtime.join("bin").join("linux64").join("vrclient.so")
    }
}

#[cfg(windows)]
fn caminho_so(p: &Path) -> Vec<CharSo> {
    use std::os::windows::ffi::OsStrExt;
    p.as_os_str().encode_wide().chain([0]).collect()
}

#[cfg(not(windows))]
fn caminho_so(p: &Path) -> Vec<CharSo> {
    use std::os::unix::ffi::OsStrExt;
    p.as_os_str().as_bytes().iter().copied().chain([0]).collect()
}

impl Vr {
    /// Só chame com o vrserver já rodando: o app entra como Background e não abre o SteamVR.
    pub fn conectar(runtime_dir: &Path) -> Result<Vr, String> {
        let dll = biblioteca_cliente(runtime_dir);
        if !dll.is_file() {
            return Err(format!("não achei {}", dll.display()));
        }
        let mut msg = [0u8; 256];
        let p = unsafe { gf_vr_conectar(caminho_so(&dll).as_ptr(), msg.as_mut_ptr().cast(), msg.len() as u32) };
        if p.is_null() {
            Err(texto(&msg))
        } else {
            Ok(Vr { p })
        }
    }

    fn checar(&self, qual: u8, e: i32) -> Result<(), String> {
        if e == 0 {
            return Ok(());
        }
        let nome = unsafe { gf_vr_nome_erro(self.p, qual as c_char, e) };
        Err(if nome.is_null() {
            format!("erro {e}")
        } else {
            unsafe { CStr::from_ptr(nome) }.to_string_lossy().into_owned()
        })
    }

    pub fn info_hmd(&self) -> Option<InfoHmd> {
        let (mut hz, mut largura, mut altura) = (0.0, 0, 0);
        let (mut modelo, mut fabricante) = ([0u8; 128], [0u8; 128]);
        let ok = unsafe {
            gf_vr_info_hmd(self.p, &mut hz, &mut largura, &mut altura, modelo.as_mut_ptr().cast(),
                           fabricante.as_mut_ptr().cast(), 128)
        };
        ok.then(|| InfoHmd { hz, largura, altura, modelo: texto(&modelo), fabricante: texto(&fabricante) })
    }

    pub fn ler(&self, secao: &str, chave: &str, tipo: Tipo) -> Option<Valor> {
        let (s, c) = (cstr(secao).ok()?, cstr(chave).ok()?);
        let mut v = 0.0;
        let e = unsafe { gf_vr_ler(self.p, s.as_ptr(), c.as_ptr(), tipo as i32, &mut v) };
        (e == 0).then(|| match tipo {
            Tipo::Bool => Valor::Bool(v != 0.0),
            Tipo::Int => Valor::Int(v as i32),
            Tipo::Float => Valor::Float(v as f32),
        })
    }

    /// Grava pelo vrserver: vale na hora e ele mesmo persiste no steamvr.vrsettings.
    pub fn gravar(&self, secao: &str, chave: &str, v: Valor) -> Result<(), String> {
        let (s, c) = (cstr(secao)?, cstr(chave)?);
        let (tipo, num) = match v {
            Valor::Bool(b) => (Tipo::Bool, b as i32 as f64),
            Valor::Int(i) => (Tipo::Int, i as f64),
            Valor::Float(f) => (Tipo::Float, f as f64),
        };
        self.checar(b's', unsafe { gf_vr_gravar(self.p, s.as_ptr(), c.as_ptr(), tipo as i32, num) })
    }

    /// None sem jogo rodando ou com histórico curto demais.
    pub fn timing(&self) -> Option<Timing> {
        let mut q = [Quadro::default(); MAX_QUADROS];
        let n = unsafe { gf_vr_quadros(self.p, q.as_mut_ptr(), MAX_QUADROS as u32) } as usize;
        agregar(&q[..n.min(MAX_QUADROS)])
    }

    /// Esvazia a fila de eventos. true = o SteamVR pediu para sair (já confirmamos):
    /// solte o `Vr` logo, senão o SteamVR mata o processo. Chame pelo menos 1×/s.
    pub fn deve_sair(&mut self) -> bool {
        unsafe { gf_vr_deve_sair(self.p) }
    }

    pub fn overlay(&self, chave: &str, nome: &str) -> Result<Overlay, String> {
        let (c, n) = (cstr(chave)?, cstr(nome)?);
        let mut h = 0;
        self.checar(b'o', unsafe { gf_overlay_criar(self.p, c.as_ptr(), n.as_ptr(), &mut h) })?;
        Ok(Overlay(h))
    }

    /// Poses (HmdMatrix34_t por linhas) do headset e do controle da âncora de pulso.
    /// None se a âncora não é de pulso ou se algum dos dois está sem tracking.
    pub fn poses(&self, ancora: Ancora) -> Option<([f32; 12], [f32; 12])> {
        let mao = match ancora {
            Ancora::PulsoEsquerdo => 0,
            Ancora::PulsoDireito => 1,
            _ => return None,
        };
        let (mut hmd, mut ctrl) = ([0.0; 12], [0.0; 12]);
        unsafe { gf_vr_poses(self.p, mao, hmd.as_mut_ptr(), ctrl.as_mut_ptr()) }.then_some((hmd, ctrl))
    }

    /// Registra o .vrmanifest (ver `manifesto`) e liga/desliga a abertura junto com o SteamVR.
    pub fn autostart(&self, manifesto: &Path, ligado: bool) -> Result<(), String> {
        let m = cstr(manifesto.to_str().ok_or("caminho do manifesto não é UTF-8")?)?;
        let k = cstr(APP_KEY)?;
        self.checar(b'a', unsafe { gf_vr_autostart(self.p, m.as_ptr(), k.as_ptr(), ligado) })
    }
}

impl Drop for Vr {
    fn drop(&mut self) {
        unsafe { gf_vr_desconectar(self.p) }
    }
}

impl Overlay {
    /// RGBA, 4 bytes por pixel. Os overlays morrem junto com a sessão do `Vr`.
    pub fn imagem(&self, vr: &Vr, rgba: &[u8], largura: u32, altura: u32) -> Result<(), String> {
        if largura == 0 || altura == 0 || rgba.len() as u64 != largura as u64 * altura as u64 * 4 {
            return Err(format!("imagem de {} bytes não é {largura}×{altura} RGBA", rgba.len()));
        }
        vr.checar(b'o', unsafe { gf_overlay_imagem(vr.p, self.0, rgba.as_ptr().cast(), largura, altura) })
    }

    /// `deslocamento_m`/`rotacao_graus` ajustam a pose padrão da âncora (zeros = padrão),
    /// nos eixos do próprio overlay: X ao longo do texto, Y para cima, Z saindo da face.
    /// O pulso segue o índice do controle de agora: chame de novo se ele reconectar.
    pub fn posicionar(&self, vr: &Vr, ancora: Ancora, largura_m: f32, deslocamento_m: [f32; 3],
                      rotacao_graus: [f32; 3], alpha: f32) -> Result<(), String> {
        let m = ancora.matriz(deslocamento_m, rotacao_graus);
        let e = unsafe { gf_overlay_posicionar(vr.p, self.0, ancora as i32, m.as_ptr(), largura_m, alpha.clamp(0.0, 1.0)) };
        if e == 19 && matches!(ancora, Ancora::PulsoEsquerdo | Ancora::PulsoDireito) {
            return Err("controle dessa mão não encontrado (desligado?)".into()); // VROverlayError_InvalidTrackedDevice
        }
        vr.checar(b'o', e)
    }

    pub fn alpha(&self, vr: &Vr, alpha: f32) -> Result<(), String> {
        vr.checar(b'o', unsafe { gf_overlay_alpha(vr.p, self.0, alpha.clamp(0.0, 1.0)) })
    }

    pub fn mostrar(&self, vr: &Vr, visivel: bool) -> Result<(), String> {
        vr.checar(b'o', unsafe { gf_overlay_mostrar(vr.p, self.0, visivel) })
    }
}

/// Conteúdo do .vrmanifest que registra o app no SteamVR (o chamador grava em
/// %LOCALAPPDATA%\ghostframeVR\ghostframevr.vrmanifest e passa para `Vr::autostart`).
pub fn manifesto(exe: &Path) -> String {
    let m = json!({
        "source": "builtin",
        "applications": [{
            "app_key": APP_KEY,
            "launch_type": "binary",
            "binary_path_windows": exe.to_string_lossy(),
            "arguments": "--bandeja",
            "is_dashboard_overlay": true,
            "strings": { "en_us": { "name": "ghostframeVR", "description": "HUD e ajustes ao vivo do ghostframeVR" } }
        }]
    });
    serde_json::to_string_pretty(&m).unwrap_or_default()
}

/// Ver a doc de `Timing`. Considera o histórico um item por frame do jogo, como o
/// exemplo da wiki do OpenVR (fps a partir de m_flSystemTimeInSeconds consecutivos).
fn agregar(q: &[Quadro]) -> Option<Timing> {
    let fim = q.last()?.t;
    let janela = &q[q.iter().position(|f| f.t >= fim - 1.0)?..];
    let dur = fim - janela[0].t;
    if janela.len() < 2 || dur <= 0.0 {
        return None;
    }
    let media = |f: &dyn Fn(&Quadro) -> f32| janela.iter().map(f).sum::<f32>() / janela.len() as f32;
    let ms_gpu = media(&|f| f.gpu_pre + f.gpu_pos);
    let ocupado = media(&|f| (f.intervalo - (f.poses_prontas - f.wgp_chamado).max(0.0)).max(0.0));
    let travados = |f: &Quadro| (f.flags & 0xF00) >> 8; // VR_COMPOSITOR_NUMBER_OF_THROTTLED_FRAMES
    Some(Timing {
        fps_jogo: ((janela.len() - 1) as f64 / dur) as f32,
        // Late start: WaitGetPoses chamado depois das poses prontas volta na hora.
        ms_cpu: media(&|f| (f.frame_pronto - f.poses_prontas.max(f.wgp_chamado)).max(0.0)),
        ms_gpu,
        ms_trabalho: ocupado.max(ms_gpu),
        reprojetados: janela.iter().map(|f| f.apresentacoes.saturating_sub(1 + travados(f))).sum(),
        perdidos: janela.iter().map(|f| f.perdidos).sum(),
    })
}

type M3 = [[f32; 3]; 3];

fn mul(a: M3, b: M3) -> M3 {
    let mut r = [[0.0; 3]; 3];
    for (i, linha) in r.iter_mut().enumerate() {
        for (j, v) in linha.iter_mut().enumerate() {
            *v = (0..3).map(|k| a[i][k] * b[k][j]).sum();
        }
    }
    r
}

/// Graus em torno de X, Y e Z, compostos como R = Rx·Ry·Rz.
fn rotacao(g: [f32; 3]) -> M3 {
    let [(sx, cx), (sy, cy), (sz, cz)] = g.map(|a| a.to_radians().sin_cos());
    let rx = [[1.0, 0.0, 0.0], [0.0, cx, -sx], [0.0, sx, cx]];
    let ry = [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]];
    let rz = [[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]];
    mul(mul(rx, ry), rz)
}

impl Ancora {
    /// Pose padrão (posição em m, rotação em graus) no espaço da âncora. Controle no
    /// OpenVR: +X direita, +Y em cima (polegar), −Z para a frente. Relógio nas costas do
    /// pulso, face para fora da mão (−X na esquerda, +X na direita), texto ao longo do
    /// antebraço e legível com a palma para baixo. A forma do controle muda a origem: é
    /// para isso que o ajuste do usuário existe.
    fn padrao(self) -> ([f32; 3], [f32; 3]) {
        match self {
            Ancora::PulsoEsquerdo => ([-0.03, 0.0, 0.12], [0.0, -90.0, 180.0]),
            Ancora::PulsoDireito => ([0.03, 0.0, 0.12], [0.0, 90.0, 180.0]),
            Ancora::Visao => ([0.0, -0.15, -0.8], [0.0; 3]), // um pouco abaixo do centro da visão
            Ancora::Mundo => ([0.0, 1.2, -1.0], [0.0; 3]),   // 1 m à frente do centro da área
        }
    }

    /// HmdMatrix34_t por linhas: pose padrão seguida do ajuste do usuário.
    pub fn matriz(self, deslocamento: [f32; 3], rotacao_graus: [f32; 3]) -> [f32; 12] {
        let (p0, r0) = self.padrao();
        let a = rotacao(r0);
        let r = mul(a, rotacao(rotacao_graus));
        let mut m = [0.0; 12];
        for i in 0..3 {
            m[i * 4..i * 4 + 3].copy_from_slice(&r[i]);
            m[i * 4 + 3] = p0[i] + (0..3).map(|k| a[i][k] * deslocamento[k]).sum::<f32>();
        }
        m
    }
}

fn compor(a: &[f32; 12], b: &[f32; 12]) -> [f32; 12] {
    let mut r = [0.0; 12];
    for i in 0..3 {
        for j in 0..4 {
            r[i * 4 + j] = (0..3).map(|k| a[i * 4 + k] * b[k * 4 + j]).sum::<f32>() + if j == 3 { a[i * 4 + 3] } else { 0.0 };
        }
    }
    r
}

/// Gesto do relógio (como no XSOverlay): pulso perto, com a face do HUD virada para o
/// rosto e o headset olhando para ele. `rel` é a pose do HUD no controle (`Ancora::matriz`).
/// Aparece com 40° de folga e só some passando de 55°: sem piscar na borda.
pub fn olhando_pulso(hmd: &[f32; 12], ctrl: &[f32; 12], rel: &[f32; 12], visivel: bool) -> bool {
    let w = compor(ctrl, rel);
    let para_olho = [hmd[3] - w[3], hmd[7] - w[7], hmd[11] - w[11]];
    let d = para_olho.iter().map(|x| x * x).sum::<f32>().sqrt();
    if !(0.05..=0.8).contains(&d) {
        return false;
    }
    let face = (w[2] * para_olho[0] + w[6] * para_olho[1] + w[10] * para_olho[2]) / d;
    // O headset olha para −Z: cosseno entre a frente dele e a direção do HUD.
    let olhar = (hmd[2] * para_olho[0] + hmd[6] * para_olho[1] + hmd[10] * para_olho[2]) / d;
    let limite = if visivel { 55f32 } else { 40f32 }.to_radians().cos();
    face > limite && olhar > limite
}

#[cfg(test)]
mod testes {
    use super::*;

    fn perto(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-4
    }

    /// Pose 3×4 a partir de rotação (graus) e posição.
    fn pose(g: [f32; 3], p: [f32; 3]) -> [f32; 12] {
        let r = rotacao(g);
        let mut m = [0.0; 12];
        for i in 0..3 {
            m[i * 4..i * 4 + 3].copy_from_slice(&r[i]);
            m[i * 4 + 3] = p[i];
        }
        m
    }

    #[test]
    fn gesto_do_relogio() {
        let id = pose([0.0; 3], [0.0; 3]);
        // Cabeça a 1,6 m olhando 35° para baixo; HUD 40 cm à frente e abaixo.
        let hmd = pose([-35.0, 0.0, 0.0], [0.0, 1.6, 0.0]);
        let pulso = [0.0, 1.35, -0.32];
        let virado = pose([-38.0, 0.0, 0.0], pulso); // face (+Z) apontando para o rosto
        assert!(olhando_pulso(&hmd, &virado, &id, false));
        // Braço abaixado: face para o chão.
        assert!(!olhando_pulso(&hmd, &pose([-90.0, 0.0, 0.0], pulso), &id, false));
        // Pulso de costas (face para longe do rosto).
        assert!(!olhando_pulso(&hmd, &pose([142.0, 0.0, 0.0], pulso), &id, false));
        // Olhando para o horizonte, pulso fora da visão.
        assert!(!olhando_pulso(&pose([0.0; 3], [0.0, 1.6, 0.0]), &pose([-80.0, 0.0, 0.0], [0.0, 0.9, -0.1]), &id, false));
        // Longe demais (braço esticado à frente não é olhar o relógio).
        assert!(!olhando_pulso(&hmd, &pose([-38.0, 0.0, 0.0], [0.0, 0.9, -0.9]), &id, false));
        // Histerese: a 48° de desvio aparece só se já estava visível.
        let borda = pose([-38.0 + 48.0, 0.0, 0.0], pulso);
        assert!(!olhando_pulso(&hmd, &borda, &id, false) && olhando_pulso(&hmd, &borda, &id, true));
        // Composição com a pose do HUD no controle: rel desloca e gira junto.
        let rel = pose([0.0, 0.0, 0.0], [0.0, 0.0, 0.1]);
        let c = compor(&virado, &rel);
        assert!(perto(c[3], virado[3]) && perto(c[7] - virado[7], virado[6] * 0.1));
    }

    #[test]
    fn manifesto_tem_o_que_o_steamvr_le() {
        let exe = Path::new(r"C:\Program Files\ghostframeVR\ghostframeVR.exe");
        let v: serde_json::Value = serde_json::from_str(&manifesto(exe)).unwrap();
        let app = &v["applications"][0];
        assert_eq!(app["app_key"], APP_KEY);
        assert_eq!(app["launch_type"], "binary");
        assert_eq!(app["binary_path_windows"], exe.to_str().unwrap());
        assert_eq!(app["arguments"], "--bandeja");
        assert_eq!(app["is_dashboard_overlay"], true);
    }

    #[test]
    fn pulso_fica_nas_costas_da_mao() {
        // Coluna 2 da rotação = para onde a face do overlay aponta.
        let face = |m: [f32; 12]| [m[2], m[6], m[10]];
        let pos = |m: [f32; 12]| [m[3], m[7], m[11]];
        let esq = Ancora::PulsoEsquerdo.matriz([0.0; 3], [0.0; 3]);
        assert!(face(esq).iter().zip([-1.0, 0.0, 0.0]).all(|(a, b)| perto(*a, b)));
        assert!(pos(esq).iter().zip([-0.03, 0.0, 0.12]).all(|(a, b)| perto(*a, b)));
        let dir = Ancora::PulsoDireito.matriz([0.0; 3], [0.0; 3]);
        assert!(face(dir).iter().zip([1.0, 0.0, 0.0]).all(|(a, b)| perto(*a, b)));
        // Ajuste nos eixos do overlay: +Z afasta da face, para fora da mão.
        let longe = Ancora::PulsoEsquerdo.matriz([0.0, 0.0, 0.01], [0.0; 3]);
        assert!(perto(pos(longe)[0], -0.04));
        // Visão sem ajuste: só translação, de frente para o headset.
        let v = Ancora::Visao.matriz([0.0; 3], [0.0; 3]);
        let id = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -0.15, 0.0, 0.0, 1.0, -0.8];
        assert!(v.iter().zip(id).all(|(a, b)| perto(*a, b)));
        // Rotação do usuário compõe depois da padrão: 90° em Y vira a face do HUD para +X.
        assert!(perto(face(Ancora::Visao.matriz([0.0; 3], [0.0, 90.0, 0.0]))[0], 1.0));
    }

    /// Jogo com 8 ms de trabalho num headset de 90 Hz, a 1/1 e travado a 1/2.
    fn historico(trava: u32) -> Vec<Quadro> {
        let periodo = 1000.0 / 90.0 * (trava + 1) as f32;
        (0..120)
            .map(|i| Quadro {
                t: i as f64 * periodo as f64 / 1000.0,
                intervalo: periodo,
                // Tempos relativos ao vsync do frame: poses no running start (−3 ms),
                // 5 ms de render; o WaitGetPoses veio 8 ms depois das poses do frame anterior.
                wgp_chamado: -periodo + 5.0,
                poses_prontas: -3.0,
                frame_pronto: 2.0,
                gpu_pre: 6.0,
                gpu_pos: 0.5,
                // Eventos nos mesmos instantes nas duas taxas (a cada 1/3 s e 2/3 s).
                apresentacoes: 1 + trava + ((i * (trava + 1)) % 30 == 0) as u32,
                perdidos: ((i * (trava + 1)) % 60 == 0) as u32,
                flags: trava << 8,
            })
            .collect()
    }

    #[test]
    fn trabalho_nao_muda_com_a_trava() {
        let livre = agregar(&historico(0)).unwrap();
        let meia = agregar(&historico(1)).unwrap();
        assert!((livre.fps_jogo - 90.0).abs() < 0.5, "{livre:?}");
        assert!((meia.fps_jogo - 45.0).abs() < 0.5, "{meia:?}");
        // bloqueado = poses_prontas − wgp_chamado; ocupado = intervalo − bloqueado = 8 ms nos dois.
        assert!(perto(livre.ms_trabalho, 8.0) && perto(meia.ms_trabalho, 8.0), "{livre:?} {meia:?}");
        assert!(perto(meia.ms_gpu, 6.5) && perto(livre.ms_cpu, 5.0) && perto(meia.ms_cpu, 5.0));
        assert_eq!(livre.reprojetados, meia.reprojetados); // a trava não conta como reprojeção
        assert!(livre.reprojetados > 0 && livre.perdidos > 0);
        assert!(agregar(&[]).is_none() && agregar(&historico(0)[..1]).is_none());
    }

    #[test]
    fn conectar_falha_sem_runtime_valido() {
        let raiz = std::env::temp_dir().join(format!("gf-teste-vr-{}", std::process::id()));
        let dll = biblioteca_cliente(&raiz);
        assert!(Vr::conectar(&raiz).unwrap_err().contains("não achei"));
        // Arquivo que não é biblioteca: passa pelo C++ (prova que a ponte compila e linka).
        std::fs::create_dir_all(dll.parent().unwrap()).unwrap();
        std::fs::write(&dll, b"nao sou uma dll").unwrap();
        let e = Vr::conectar(&raiz).unwrap_err();
        assert!(e.contains("vrclient"), "{e}");
        let _ = std::fs::remove_dir_all(&raiz);
    }
}
