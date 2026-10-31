//! Modo AUTO: escolhe o multiplicador do frame gen e a trava do SteamVR
//! (framesToThrottle) pelo custo do jogo. Sem OpenVR: quem chama mede e aplica.
use std::collections::VecDeque;

/// Janela do percentil. Curta para uma queda de 1 s não virar 2 s de "necessidade".
const JANELA_S: f64 = 0.5;
const SUBIR_S: f64 = 2.0;
const DESCER_S: f64 = 8.0;
/// Folga exigida para voltar a um multiplicador menor. Manter ou subir só exige
/// segurar Hz/N: essa faixa entre os dois limiares é o que impede o vai e volta.
const FOLGA: f32 = 1.1;

pub struct Entrada {
    pub hz: f32,
    /// Custo do jogo por frame, sem a espera da trava.
    pub ms_trabalho: f32,
    pub framegen_disponivel: bool,
    pub agora_s: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Decisao {
    pub recomendado: u8,
    pub multiplicador: u8,
    /// framesToThrottle do SteamVR: o jogo roda a Hz / (trava + 1).
    pub trava: u8,
    pub motivo: &'static str,
}

pub struct Auto {
    nivel: u8,
    /// (desde quando, nível que se sustentou durante toda a espera)
    pendente: Option<(f64, u8)>,
    amostras: VecDeque<(f64, f32)>,
    ordem: Vec<f32>,
    ultima: Decisao,
}

impl Auto {
    pub fn new() -> Self {
        Auto {
            nivel: 1,
            pendente: None,
            amostras: VecDeque::new(),
            ordem: Vec::new(),
            ultima: Decisao { recomendado: 1, multiplicador: 1, trava: 0, motivo: "sem dados" },
        }
    }

    pub fn atualizar(&mut self, e: &Entrada) -> Decisao {
        let valida = e.hz.is_finite() && e.hz > 0.0 && e.ms_trabalho.is_finite() && e.ms_trabalho > 0.0
            && e.agora_s.is_finite();
        if !valida {
            return self.ultima;
        }
        self.amostras.push_back((e.agora_s, e.ms_trabalho));
        while self.amostras.front().is_some_and(|a| e.agora_s - a.0 > JANELA_S) {
            self.amostras.pop_front();
        }
        // p90 do custo: o ritmo que o jogo segura em 9 de 10 frames; um engasgo isolado não conta.
        self.ordem.clear();
        self.ordem.extend(self.amostras.iter().map(|a| a.1));
        let i = (self.ordem.len() * 9).div_ceil(10) - 1;
        let capacidade = 1000.0 / *self.ordem.select_nth_unstable_by(i, f32::total_cmp).1;

        let atual = self.nivel;
        let alvo = (1..=3u8).find(|&n| capacidade >= e.hz / n as f32 * if n < atual { FOLGA } else { 1.0 });
        let n = alvo.unwrap_or(3);
        if n == atual {
            self.pendente = None;
        } else {
            let subir = n > atual;
            let (desde, sustentado) = match self.pendente {
                Some((d, p)) if (p > atual) == subir => (d, if subir { p.min(n) } else { p.max(n) }),
                _ => (e.agora_s, n),
            };
            if e.agora_s - desde >= if subir { SUBIR_S } else { DESCER_S } {
                self.nivel = sustentado;
                self.pendente = None;
            } else {
                self.pendente = Some((desde, sustentado));
            }
        }

        let motivo = match (e.framegen_disponivel, self.pendente, self.nivel) {
            (false, _, _) => "frame gen indisponível, sem trava",
            (_, Some((_, p)), nivel) if p > nivel => "fps caiu, confirmando",
            (_, Some(_), _) => "fps subiu, confirmando",
            _ if alvo.is_none() => "jogo abaixo de 1/3 do Hz",
            (_, _, 1) => "jogo segura o Hz",
            (_, _, 2) => "jogo segura metade do Hz",
            _ => "jogo segura 1/3 do Hz",
        };
        // Travar sem frames sintéticos só cortaria o fps do usuário.
        let multiplicador = if e.framegen_disponivel { self.nivel } else { 1 };
        self.ultima = Decisao { recomendado: self.nivel, multiplicador, trava: multiplicador - 1, motivo };
        self.ultima
    }
}

#[cfg(test)]
mod testes {
    use super::*;

    /// Roda `seg` segundos de amostras a 50 Hz e devolve todas as decisões.
    fn rodar(a: &mut Auto, t: &mut f64, seg: f64, hz: f32, fps: f32, fg: bool) -> Vec<Decisao> {
        let fim = *t + seg;
        let mut ds = Vec::new();
        while *t < fim - 1e-9 {
            ds.push(a.atualizar(&Entrada { hz, ms_trabalho: 1000.0 / fps, framegen_disponivel: fg, agora_s: *t }));
            *t += 0.02;
        }
        ds
    }

    fn estavel(fps: f32) -> Decisao {
        let (mut a, mut t) = (Auto::new(), 0.0);
        *rodar(&mut a, &mut t, 10.0, 90.0, fps, true).last().unwrap()
    }

    #[test]
    fn escolhe_pelo_que_o_jogo_segura() {
        for (fps, n) in [(45.0, 2), (30.0, 3), (95.0, 1), (60.0, 2), (36.0, 3)] {
            let d = estavel(fps);
            assert_eq!((d.recomendado, d.multiplicador, d.trava), (n, n, n - 1), "{fps} fps");
        }
        assert_eq!(estavel(95.0).motivo, "jogo segura o Hz");
        let d = estavel(20.0);
        assert_eq!((d.recomendado, d.motivo), (3, "jogo abaixo de 1/3 do Hz"));
    }

    #[test]
    fn sem_framegen_nunca_trava() {
        let (mut a, mut t) = (Auto::new(), 0.0);
        for d in rodar(&mut a, &mut t, 10.0, 90.0, 30.0, false) {
            assert_eq!((d.multiplicador, d.trava), (1, 0));
        }
        assert_eq!(a.ultima.recomendado, 3);
        let d = rodar(&mut a, &mut t, 0.02, 90.0, 30.0, true)[0];
        assert_eq!((d.multiplicador, d.trava), (3, 2));
    }

    #[test]
    fn histerese() {
        let (mut a, mut t) = (Auto::new(), 0.0);
        assert_eq!(rodar(&mut a, &mut t, 5.0, 90.0, 45.0, true).last().unwrap().recomendado, 2);
        // Queda de 1 s não troca.
        let ds = [rodar(&mut a, &mut t, 1.0, 90.0, 35.0, true), rodar(&mut a, &mut t, 10.0, 90.0, 45.0, true)].concat();
        assert!(ds.iter().all(|d| d.recomendado == 2));
        // Queda sustentada sobe em ~2 s.
        let ds = rodar(&mut a, &mut t, 3.0, 90.0, 35.0, true);
        let troca = ds.iter().position(|d| d.recomendado == 3).unwrap() as f64 * 0.02;
        assert!((1.9..2.2).contains(&troca), "subiu em {troca} s");
        // De volta a 45 fps: sem a folga de 10% para Hz/2, fica em 3.
        assert!(rodar(&mut a, &mut t, 10.0, 90.0, 45.0, true).iter().all(|d| d.recomendado == 3));
        // Folga sustentada desce só depois de ~8 s (+ o atraso da janela do percentil).
        let ds = rodar(&mut a, &mut t, 10.0, 90.0, 55.0, true);
        let troca = ds.iter().position(|d| d.recomendado == 2).unwrap() as f64 * 0.02;
        assert!((8.0..8.6).contains(&troca), "desceu em {troca} s");
    }

    #[test]
    fn entrada_invalida_mantem_a_ultima() {
        let (mut a, mut t) = (Auto::new(), 0.0);
        let antes = *rodar(&mut a, &mut t, 5.0, 90.0, 45.0, true).last().unwrap();
        for (hz, ms) in [(0.0, 22.0), (-90.0, 22.0), (f32::NAN, 22.0), (90.0, 0.0), (90.0, -1.0), (90.0, f32::NAN), (90.0, f32::INFINITY)] {
            assert_eq!(a.atualizar(&Entrada { hz, ms_trabalho: ms, framegen_disponivel: true, agora_s: t }), antes);
        }
        let e = Entrada { hz: 90.0, ms_trabalho: 22.0, framegen_disponivel: true, agora_s: f64::NAN };
        assert_eq!(a.atualizar(&e), antes);
        assert_eq!(Auto::new().atualizar(&e).motivo, "sem dados");
        assert_eq!(rodar(&mut a, &mut t, 1.0, 90.0, 45.0, true).last().unwrap(), &antes);
    }
}
