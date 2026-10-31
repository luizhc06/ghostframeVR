//! HUD de pulso no headset: desenha as métricas num buffer RGBA8 (alfa direto)
//! para o overlay do SteamVR (IVROverlay::SetOverlayRaw, ~4x por segundo).
use std::fmt::Write;

pub struct Metricas {
    pub fps_headset: Option<f32>,
    pub fps_jogo: Option<f32>,
    pub hz: Option<f32>,
    /// Frames no último segundo.
    pub reais: Option<u32>,
    pub gerados: Option<u32>,
    pub ms_jogo: Option<f32>,
    pub ms_framegen: Option<f32>,
    pub latencia_ms: Option<f32>,
    /// Ex.: "AUTO 2×", "MANUAL 3×", "DESLIGADO".
    pub modo: String,
    /// Últimas ~60 amostras de fps do headset.
    pub historico: Vec<f32>,
}

pub struct Itens {
    pub fps: bool,
    pub frames: bool,
    pub frametime: bool,
    pub latencia: bool,
    pub gpu: bool,
    pub grafico: bool,
}

/// Reaproveita o buffer e o texto entre chamadas: sem alocar por frame depois do primeiro.
#[derive(Default)]
pub struct Hud {
    px: Vec<u8>,
    txt: String,
}

const L: i32 = 512;
const M: i32 = 24;
const CHANFRO: i32 = 16;
const TOPO: i32 = 58;
const SO_CABECALHO: i32 = 50;
const ALT_FPS: i32 = 53;
const ALT_LINHA: i32 = 24;
const ALT_GRAFICO: i32 = 56;
const FIM: i32 = 12;

const FUNDO: [u8; 4] = [3, 12, 16, 217];
const BORDA: [u8; 4] = [0, 240, 255, 140];
const DIVISA: [u8; 4] = [0, 240, 255, 45];
const CIANO: [u8; 4] = [0, 240, 255, 255];
const BRILHO: [u8; 4] = [0, 240, 255, 18];
const AREA: [u8; 4] = [0, 240, 255, 30];
const GELO: [u8; 4] = [200, 247, 255, 255];
const ROTULO: [u8; 4] = [127, 169, 179, 255];
const BRANCO: [u8; 4] = [255, 255, 255, 255];
const ESCURO: [u8; 4] = [0, 16, 20, 255];

impl Hud {
    pub fn desenhar(&mut self, m: &Metricas, it: &Itens) -> (&[u8], u32, u32) {
        let linhas = [it.frames, it.frametime, it.latencia, it.gpu].iter().filter(|&&b| b).count() as i32;
        let corpo = it.fps as i32 * ALT_FPS + linhas * ALT_LINHA + it.grafico as i32 * ALT_GRAFICO;
        let h = if corpo == 0 { SO_CABECALHO } else { TOPO + corpo + FIM };
        self.px.clear();
        self.px.resize((L * h * 4) as usize, 0);
        let txt = &mut self.txt;
        let mut t = Tela { px: &mut self.px, w: L, h };

        for y in 0..h {
            for x in 0..L {
                if dentro(x, y, L, h, CHANFRO) {
                    t.ponto(x, y, FUNDO);
                    if !dentro(x - 2, y - 2, L - 4, h - 4, CHANFRO - 1) {
                        t.ponto(x, y, BORDA);
                    }
                }
            }
        }
        // Cantos em L, como os .painel do app.
        t.ret(L - 20, 6, 14, 2, CIANO);
        t.ret(L - 8, 6, 2, 14, CIANO);
        t.ret(6, h - 8, 14, 2, CIANO);
        t.ret(6, h - 20, 2, 14, CIANO);

        t.texto(M, 18, "GHOSTFRAME", 2, CIANO);
        let modo = m.modo.char_indices().nth(24).map_or(&m.modo[..], |(i, _)| &m.modo[..i]);
        let (cw, ch) = (largura(modo, 2) + 16, 24);
        for y in 0..ch {
            for x in 0..cw {
                if dentro(x, y, cw, ch, 5) {
                    t.ponto(L - M - cw + x, 13 + y, CIANO);
                }
            }
        }
        t.texto(L - M - cw + 8, 18, modo, 2, ESCURO);
        if corpo == 0 {
            return (&self.px, L as u32, h as u32);
        }
        t.ret(M, 44, L - 2 * M, 1, DIVISA);

        let mut y = TOPO;
        if it.fps {
            txt.clear();
            valor(txt, m.fps_headset, 0, "");
            // Brilho: o número repetido em volta, bem transparente, vira um halo.
            for (dx, dy) in (-2..=2).flat_map(|dx| (-2..=2).map(move |dy| (dx, dy))) {
                t.texto(M + dx, y + dy, txt, 5, BRILHO);
            }
            t.texto(M, y, txt, 5, BRANCO);
            t.texto(M + largura(txt, 5) + 12, y + 21, "FPS", 2, CIANO);
            for (dy, rotulo, v) in [(1, "JOGO", m.fps_jogo), (21, "HZ", m.hz)] {
                txt.clear();
                valor(txt, v, 0, "");
                t.texto(L - M - 120, y + dy, rotulo, 2, ROTULO);
                t.texto(L - M - largura(txt, 2), y + dy, txt, 2, GELO);
            }
            y += ALT_FPS;
        }
        if it.frames {
            txt.clear();
            valor(txt, m.reais.map(|v| v as f32), 0, "");
            txt.push('/');
            valor(txt, m.gerados.map(|v| v as f32), 0, "");
            t.linha(y, "REAIS/GERADOS", txt);
            y += ALT_LINHA;
        }
        for (ativo, rotulo, v) in
            [(it.frametime, "FRAME TIME", m.ms_jogo), (it.latencia, "LATENCIA", m.latencia_ms), (it.gpu, "GPU FG", m.ms_framegen)]
        {
            if ativo {
                txt.clear();
                valor(txt, v, 1, " MS");
                t.linha(y, rotulo, txt);
                y += ALT_LINHA;
            }
        }
        if it.grafico {
            t.grafico(y + 4, ALT_GRAFICO - 14, &m.historico, m.hz);
        }
        (&self.px, L as u32, h as u32)
    }
}

/// Acrescenta o número ou "--" se faltar.
fn valor(s: &mut String, v: Option<f32>, casas: usize, unidade: &str) {
    match v.filter(|v| v.is_finite()) {
        Some(v) => {
            let _ = write!(s, "{v:.casas$}{unidade}");
        }
        None => s.push_str("--"),
    }
}

/// Dentro do retângulo w×h com os cantos superior esquerdo e inferior direito chanfrados em k.
fn dentro(x: i32, y: i32, w: i32, h: i32, k: i32) -> bool {
    x >= 0 && y >= 0 && x < w && y < h && x + y >= k && (w - 1 - x) + (h - 1 - y) >= k
}

fn largura(s: &str, e: i32) -> i32 {
    (s.chars().count() as i32 * 6 - 1) * e
}

struct Tela<'a> {
    px: &'a mut [u8],
    w: i32,
    h: i32,
}

impl Tela<'_> {
    /// Compõe `c` por cima (alfa direto).
    fn ponto(&mut self, x: i32, y: i32, c: [u8; 4]) {
        if x < 0 || y < 0 || x >= self.w || y >= self.h {
            return;
        }
        let i = ((y * self.w + x) * 4) as usize;
        let d = &mut self.px[i..i + 4];
        let (sa, da) = (c[3] as f32 / 255.0, d[3] as f32 / 255.0 * (1.0 - c[3] as f32 / 255.0));
        let a = sa + da;
        if a > 0.0 {
            for k in 0..3 {
                d[k] = ((c[k] as f32 * sa + d[k] as f32 * da) / a).round() as u8;
            }
        }
        d[3] = (a * 255.0).round() as u8;
    }

    fn ret(&mut self, x: i32, y: i32, w: i32, h: i32, c: [u8; 4]) {
        for yy in y..y + h {
            for xx in x..x + w {
                self.ponto(xx, yy, c);
            }
        }
    }

    fn texto(&mut self, x: i32, y: i32, s: &str, e: i32, c: [u8; 4]) {
        for (i, ch) in s.chars().enumerate() {
            for (lin, bits) in glifo(ch).iter().enumerate() {
                for col in 0..5 {
                    if bits >> (4 - col) & 1 == 1 {
                        self.ret(x + (i as i32 * 6 + col) * e, y + lin as i32 * e, e, e, c);
                    }
                }
            }
        }
    }

    fn linha(&mut self, y: i32, rotulo: &str, valor: &str) {
        self.texto(M, y, rotulo, 2, ROTULO);
        self.texto(L - M - largura(valor, 2), y, valor, 2, GELO);
    }

    /// Linha do fps do headset; o tracejado marca o Hz.
    fn grafico(&mut self, y0: i32, h: i32, hist: &[f32], hz: Option<f32>) {
        let w = L - 2 * M;
        let topo = hist.iter().copied().chain(hz).filter(|v| v.is_finite()).fold(1.0, f32::max) * 1.15;
        let alt = |v: f32| {
            let v = if v.is_finite() { v.clamp(0.0, topo) } else { 0.0 };
            y0 + h - 1 - (v / topo * (h - 1) as f32).round() as i32
        };
        self.ret(M, y0 + h - 1, w, 1, DIVISA);
        if let Some(hz) = hz.filter(|v| v.is_finite() && *v > 0.0) {
            for x in (M..M + w).step_by(8) {
                self.ret(x, alt(hz), 4, 1, BORDA);
            }
        }
        if hist.len() < 2 {
            return;
        }
        let mut antes = None;
        for i in 0..w {
            let p = i as f32 / (w - 1) as f32 * (hist.len() - 1) as f32;
            let (a, b) = (hist[p as usize], hist[(p as usize + 1).min(hist.len() - 1)]);
            let y = alt(a + (b - a) * p.fract());
            self.ret(M + i, y, 1, y0 + h - 1 - y, AREA);
            let (y1, y2) = antes.map_or((y, y), |ya: i32| (ya.min(y), ya.max(y)));
            self.ret(M + i, y1, 1, y2 - y1 + 2, CIANO);
            antes = Some(y);
        }
    }
}

/// Fonte 5×7 própria (domínio público): 7 linhas, bit 4 = coluna da esquerda. Só maiúsculas.
fn glifo(c: char) -> [u8; 7] {
    const FONTE: &[(char, [u8; 7])] = &[
        ('0', [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E]),
        ('1', [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E]),
        ('2', [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F]),
        ('3', [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E]),
        ('4', [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02]),
        ('5', [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E]),
        ('6', [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E]),
        ('7', [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08]),
        ('8', [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E]),
        ('9', [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C]),
        ('A', [0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11]),
        ('B', [0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E]),
        ('C', [0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E]),
        ('D', [0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C]),
        ('E', [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F]),
        ('F', [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10]),
        ('G', [0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F]),
        ('H', [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11]),
        ('I', [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E]),
        ('J', [0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C]),
        ('K', [0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11]),
        ('L', [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F]),
        ('M', [0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11]),
        ('N', [0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11]),
        ('O', [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E]),
        ('P', [0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10]),
        ('Q', [0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D]),
        ('R', [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11]),
        ('S', [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E]),
        ('T', [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04]),
        ('U', [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E]),
        ('V', [0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04]),
        ('W', [0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A]),
        ('X', [0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11]),
        ('Y', [0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04]),
        ('Z', [0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F]),
        (' ', [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]),
        ('.', [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C]),
        ('-', [0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00]),
        ('/', [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00]),
        (':', [0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00]),
        ('×', [0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00]),
        ('?', [0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04]),
    ];
    let c = c.to_ascii_uppercase();
    FONTE.iter().find(|g| g.0 == c).unwrap_or(&FONTE[FONTE.len() - 1]).1
}

#[cfg(test)]
mod testes {
    use super::*;

    fn itens(v: bool) -> Itens {
        Itens { fps: v, frames: v, frametime: v, latencia: v, gpu: v, grafico: v }
    }

    fn exemplo() -> Metricas {
        Metricas {
            fps_headset: Some(89.6),
            fps_jogo: Some(44.8),
            hz: Some(90.0),
            reais: Some(45),
            gerados: Some(45),
            ms_jogo: Some(21.4),
            ms_framegen: Some(3.2),
            latencia_ms: Some(18.7),
            modo: "AUTO 2×".into(),
            historico: (0..60).map(|i| if (30..38).contains(&i) { 58.0 + (i % 4) as f32 } else { 88.0 + (i % 3) as f32 }).collect(),
        }
    }

    #[test]
    fn dimensoes_e_itens() {
        let mut hud = Hud::default();
        let (px, w, h) = hud.desenhar(&exemplo(), &itens(true));
        assert_eq!((w, px.len()), (512, (w * h * 4) as usize));
        let cheio = h;
        let sem_grafico = hud.desenhar(&exemplo(), &Itens { grafico: false, ..itens(true) }).2;
        let vazio = hud.desenhar(&exemplo(), &itens(false)).2;
        assert!(cheio > sem_grafico && sem_grafico > vazio && vazio > 0);
        // Tudo faltando não quebra.
        let nada = Metricas {
            fps_headset: None, fps_jogo: None, hz: None, reais: None, gerados: None, ms_jogo: None,
            ms_framegen: None, latencia_ms: Some(f32::NAN), modo: String::new(), historico: vec![f32::NAN; 3],
        };
        assert_eq!(hud.desenhar(&nada, &itens(true)).2, cheio);
    }

    #[test]
    fn reaproveita_o_buffer() {
        let mut hud = Hud::default();
        let (m, i) = (exemplo(), itens(true));
        let p = hud.desenhar(&m, &i).0.as_ptr();
        assert_eq!(hud.desenhar(&m, &i).0.as_ptr(), p);
        hud.desenhar(&m, &itens(false));
        assert_eq!(hud.desenhar(&m, &i).0.as_ptr(), p);
    }

    /// Grava target/hud_previa.png para olhar o HUD sem headset.
    #[test]
    fn previa_png() {
        let mut hud = Hud::default();
        let (px, w, h) = hud.desenhar(&exemplo(), &itens(true));
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("target");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("hud_previa.png"), png(px, w, h)).unwrap();
    }

    /// PNG RGBA8 mínimo: zlib só com blocos "stored" (sem compressão).
    fn png(px: &[u8], w: u32, h: u32) -> Vec<u8> {
        fn crc32(d: &[u8]) -> u32 {
            let mut c = !0u32;
            for &b in d {
                c ^= b as u32;
                for _ in 0..8 {
                    c = (c >> 1) ^ (0xEDB8_8320 & (c & 1).wrapping_neg());
                }
            }
            !c
        }
        fn bloco(out: &mut Vec<u8>, tipo: &[u8], d: &[u8]) {
            out.extend((d.len() as u32).to_be_bytes());
            let ini = out.len();
            out.extend(tipo);
            out.extend(d);
            let crc = crc32(&out[ini..]);
            out.extend(crc.to_be_bytes());
        }
        let cru: Vec<u8> = px.chunks(w as usize * 4).flat_map(|l| [0].iter().chain(l)).copied().collect();
        let mut z = vec![0x78, 0x01];
        let n = cru.chunks(65535).count();
        for (i, c) in cru.chunks(65535).enumerate() {
            z.push((i + 1 == n) as u8);
            z.extend((c.len() as u16).to_le_bytes());
            z.extend((!(c.len() as u16)).to_le_bytes());
            z.extend(c);
        }
        let (mut a, mut b) = (1u32, 0u32);
        for &x in &cru {
            a = (a + x as u32) % 65521;
            b = (b + a) % 65521;
        }
        z.extend((b << 16 | a).to_be_bytes());
        let mut ihdr = [w.to_be_bytes(), h.to_be_bytes()].concat();
        ihdr.extend([8, 6, 0, 0, 0]);
        let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
        bloco(&mut out, b"IHDR", &ihdr);
        bloco(&mut out, b"IDAT", &z);
        bloco(&mut out, b"IEND", &[]);
        out
    }
}
