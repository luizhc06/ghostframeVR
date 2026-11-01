//! Opções de inicialização de um jogo na Steam (`LaunchOptions` em
//! `userdata\<conta>\config\localconfig.vdf`). Edição cirúrgica: só o texto do valor muda
//! (ou entra uma linha nova); o resto do arquivo, que tem MBs, fica byte a byte igual.
//! As chaves do KeyValues da Valve não diferenciam maiúsculas ("Software" / "software").

#[derive(Debug)]
enum No {
    Valor { chave: String, ini: usize, fim: usize }, // [ini, fim) = conteúdo entre as aspas
    Bloco { chave: String, abre: usize, fecha: usize, filhos: Vec<No> },
}

struct Leitor<'a> {
    t: &'a [u8],
    i: usize,
}

enum Tok {
    Texto(usize, usize),
    Abre(usize),
    Fecha(usize),
    Fim,
}

impl Leitor<'_> {
    fn tok(&mut self) -> Result<Tok, String> {
        let t = self.t;
        loop {
            while self.i < t.len() && t[self.i].is_ascii_whitespace() {
                self.i += 1;
            }
            if t[self.i..].starts_with(b"//") {
                while self.i < t.len() && t[self.i] != b'\n' {
                    self.i += 1;
                }
                continue;
            }
            break;
        }
        let Some(&c) = t.get(self.i) else { return Ok(Tok::Fim) };
        match c {
            b'{' => {
                self.i += 1;
                Ok(Tok::Abre(self.i - 1))
            }
            b'}' => {
                self.i += 1;
                Ok(Tok::Fecha(self.i - 1))
            }
            b'"' => {
                let ini = self.i + 1;
                let mut j = ini;
                while j < t.len() && t[j] != b'"' {
                    j += if t[j] == b'\\' { 2 } else { 1 };
                }
                if j >= t.len() {
                    return Err("localconfig.vdf: aspas sem fechar".into());
                }
                self.i = j + 1;
                Ok(Tok::Texto(ini, j))
            }
            _ => {
                let ini = self.i;
                while self.i < t.len() && !t[self.i].is_ascii_whitespace() && !b"{}\"".contains(&t[self.i]) {
                    self.i += 1;
                }
                Ok(Tok::Texto(ini, self.i))
            }
        }
    }

    /// Lê pares chave/valor até `}` (dentro de um bloco) ou o fim do arquivo (na raiz).
    fn nos(&mut self, raiz: bool) -> Result<(Vec<No>, usize), String> {
        let mut v = Vec::new();
        loop {
            let chave = match self.tok()? {
                Tok::Texto(a, b) => String::from_utf8_lossy(&self.t[a..b]).into_owned(),
                Tok::Fecha(p) if !raiz => return Ok((v, p)),
                Tok::Fim if raiz => return Ok((v, self.t.len())),
                _ => return Err("localconfig.vdf: estrutura inesperada".into()),
            };
            match self.tok()? {
                Tok::Texto(ini, fim) => v.push(No::Valor { chave, ini, fim }),
                Tok::Abre(abre) => {
                    let (filhos, fecha) = self.nos(false)?;
                    v.push(No::Bloco { chave, abre, fecha, filhos });
                }
                _ => return Err("localconfig.vdf: chave sem valor".into()),
            }
        }
    }
}

fn ler(texto: &str) -> Result<Vec<No>, String> {
    Leitor { t: texto.as_bytes(), i: 0 }.nos(true).map(|(v, _)| v)
}

fn achar<'a>(nos: &'a [No], chave: &str) -> Option<&'a No> {
    nos.iter().find(|n| match n {
        No::Valor { chave: c, .. } | No::Bloco { chave: c, .. } => c.eq_ignore_ascii_case(chave),
    })
}

fn filhos<'a>(n: &'a No) -> &'a [No] {
    match n {
        No::Bloco { filhos, .. } => filhos,
        No::Valor { .. } => &[],
    }
}

const CAMINHO_APPS: [&str; 5] = ["UserLocalConfigStore", "Software", "Valve", "Steam", "apps"];

fn bloco_apps(nos: &[No]) -> Option<&No> {
    let mut n = achar(nos, CAMINHO_APPS[0])?;
    for c in &CAMINHO_APPS[1..] {
        n = achar(filhos(n), c)?;
    }
    matches!(n, No::Bloco { .. }).then_some(n)
}

fn escapar(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

fn desescapar(s: &str) -> String {
    let mut r = String::new();
    let mut it = s.chars();
    while let Some(c) = it.next() {
        if c == '\\' {
            match it.next() {
                Some('n') => r.push('\n'),
                Some('t') => r.push('\t'),
                Some(x @ ('\\' | '"')) => r.push(x),
                Some(x) => {
                    r.push('\\'); // escape desconhecido: a barra fica, como estava
                    r.push(x);
                }
                None => r.push('\\'),
            }
        } else {
            r.push(c);
        }
    }
    r
}

/// As opções de inicialização atuais de um jogo ("" se não tem).
pub fn opcoes(vdf: &str, appid: &str) -> Result<String, String> {
    let nos = ler(vdf)?;
    let app = bloco_apps(&nos).and_then(|a| achar(filhos(a), appid));
    Ok(match app.and_then(|a| achar(filhos(a), "LaunchOptions")) {
        Some(No::Valor { ini, fim, .. }) => desescapar(&vdf[*ini..*fim]),
        _ => String::new(),
    })
}

/// O mesmo arquivo com as opções de inicialização do jogo trocadas por `valor`.
pub fn com_opcoes(vdf: &str, appid: &str, valor: &str) -> Result<String, String> {
    let nos = ler(vdf)?;
    let apps = bloco_apps(&nos).ok_or("a Steam ainda não tem a lista de jogos dessa conta (abra o VRChat uma vez pela Steam)")?;
    let novo = escapar(valor);
    let (a, b, inserir) = match achar(filhos(apps), appid) {
        Some(app @ No::Bloco { fecha, .. }) => match achar(filhos(app), "LaunchOptions") {
            Some(No::Valor { ini, fim, .. }) => (*ini, *fim, novo),
            _ => (*fecha, *fecha, format!("\t\"LaunchOptions\"\t\t\"{novo}\"\n")),
        },
        Some(No::Valor { .. }) => return Err(format!("localconfig.vdf: \"{appid}\" não é um bloco")),
        None => {
            let No::Bloco { fecha, .. } = apps else { unreachable!() };
            (*fecha, *fecha, format!("\t\"{appid}\"\n\t{{\n\t\t\"LaunchOptions\"\t\t\"{novo}\"\n\t}}\n"))
        }
    };
    Ok(format!("{}{}{}", &vdf[..a], inserir, &vdf[b..]))
}

/// Junta as opções do usuário com as nossas: as nossas (mesmos nomes) são trocadas, as
/// outras ficam como estão e na mesma ordem.
pub fn mesclar(atuais: &str, nossas: &str) -> String {
    const NOSSAS: [&str; 3] = ["--process-priority=", "--main-thread-priority=", "--affinity="];
    let mut v: Vec<&str> = atuais.split_whitespace().filter(|o| !NOSSAS.iter().any(|n| o.starts_with(n))).collect();
    v.extend(nossas.split_whitespace());
    v.join(" ")
}

/// Conta Steam usada por último (loginusers.vdf: MostRecent = 1; com uma conta só, ela)
/// como id de 32 bits da pasta userdata.
pub fn conta_recente(loginusers: &str) -> Option<String> {
    let nos = ler(loginusers).ok()?;
    let users: Vec<(&str, &[No])> = filhos(achar(&nos, "users")?)
        .iter()
        .filter_map(|u| match u {
            No::Bloco { chave, filhos, .. } => Some((chave.as_str(), filhos.as_slice())),
            No::Valor { .. } => None,
        })
        .collect();
    let recente = users.iter().find(|(_, f)| matches!(achar(f, "MostRecent"), Some(No::Valor { ini, fim, .. }) if &loginusers[*ini..*fim] == "1"));
    let (id, _) = recente.or(if users.len() == 1 { users.first() } else { None })?;
    id.parse::<u64>().ok()?.checked_sub(76561197960265728).map(|x| x.to_string())
}

#[cfg(test)]
mod testes {
    use super::*;

    const VDF: &str = "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"438100\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LastPlayed\"\t\t\"1793500000\"\n\t\t\t\t\t\t\"LaunchOptions\"\t\t\"--no-vr \\\"x\\\"\"\n\t\t\t\t\t}\n\t\t\t\t\t\"250820\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LastPlayed\"\t\t\"1\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n\t\"friends\" { \"x\" \"y\" } // comentário\n}\n";

    #[test]
    fn le_e_troca_so_o_valor() {
        assert_eq!(opcoes(VDF, "438100").unwrap(), "--no-vr \"x\"");
        let novo = com_opcoes(VDF, "438100", "--no-vr --process-priority=1").unwrap();
        assert_eq!(opcoes(&novo, "438100").unwrap(), "--no-vr --process-priority=1");
        // O resto do arquivo não mudou nada.
        let antes = VDF.find("\"--no-vr").unwrap();
        assert_eq!(&novo[..antes], &VDF[..antes]);
        assert!(novo.ends_with(&VDF[VDF.find("\n\t\t\t\t\t}\n\t\t\t\t\t\"250820\"").unwrap()..]));
    }

    #[test]
    fn cria_linha_e_bloco_que_faltam() {
        // Jogo sem LaunchOptions: entra a linha no bloco dele.
        let a = com_opcoes(VDF, "250820", "--x").unwrap();
        assert_eq!(opcoes(&a, "250820").unwrap(), "--x");
        assert_eq!(opcoes(&a, "438100").unwrap(), "--no-vr \"x\"");
        // Jogo sem bloco: entra o bloco dentro de apps.
        let b = com_opcoes(VDF, "999", "--y \"z\"").unwrap();
        assert_eq!(opcoes(&b, "999").unwrap(), "--y \"z\"");
        assert!(ler(&b).is_ok());
        // Sem apps: erro, nada é inventado.
        assert!(com_opcoes("\"UserLocalConfigStore\" { }", "438100", "--x").is_err());
        assert!(com_opcoes("\"UserLocalConfigStore\" { \"a\" ", "438100", "--x").is_err());
        assert_eq!(opcoes("\"UserLocalConfigStore\" { }", "438100").unwrap(), "");
    }

    #[test]
    fn mescla_com_as_opcoes_do_usuario() {
        assert_eq!(mesclar("--no-vr --process-priority=2 --fps=90", "--process-priority=1"), "--no-vr --fps=90 --process-priority=1");
        assert_eq!(mesclar("", "--process-priority=1 --affinity=FFF"), "--process-priority=1 --affinity=FFF");
        assert_eq!(mesclar("--affinity=F --x", ""), "--x");
        // Escape desconhecido fica como estava.
        assert_eq!(desescapar(r#"a\q \" \\ b"#), r#"a\q " \ b"#);
    }

    /// Com GF_VDF apontando para um localconfig.vdf real (uma cópia): lê, edita e confere
    /// que só o trecho do jogo mudou.
    #[test]
    fn arquivo_real_se_houver() {
        let Ok(p) = std::env::var("GF_VDF") else { return };
        let t = std::fs::read_to_string(p).unwrap();
        let antes = opcoes(&t, "438100").unwrap();
        let novo = com_opcoes(&t, "438100", &mesclar(&antes, "--process-priority=1")).unwrap();
        assert_eq!(opcoes(&novo, "438100").unwrap(), mesclar(&antes, "--process-priority=1"));
        let (a, b) = (t.as_bytes(), novo.as_bytes());
        let pre = a.iter().zip(b).take_while(|(x, y)| x == y).count();
        let suf = a.iter().rev().zip(b.iter().rev()).take_while(|(x, y)| x == y).count();
        println!("real: {} bytes, antes={antes:?}, mudou só [{pre}..{}] -> +{} bytes", a.len(), a.len() - suf, b.len() as i64 - a.len() as i64);
        assert!(pre + suf >= a.len().min(b.len()) - 80, "mudou mais do que o trecho do jogo");
    }

    #[test]
    fn conta_usada_por_ultimo() {
        let lu = "\"users\"\n{\n\t\"76561198000000001\"\n\t{\n\t\t\"MostRecent\"\t\t\"0\"\n\t}\n\t\"76561198000000002\"\n\t{\n\t\t\"AccountName\"\t\t\"x\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t}\n}\n";
        assert_eq!(conta_recente(lu).as_deref(), Some("39734274"));
        assert_eq!(conta_recente("\"users\" { }"), None);
        // Uma conta só, sem MostRecent (loginusers real): é ela.
        let uma = "\"users\"\n{\n\t\"76561199070133680\"\n\t{\n\t\t\"AccountName\"\t\t\"x\"\n\t}\n}\n";
        assert_eq!(conta_recente(uma).as_deref(), Some("1109867952"));
        // Duas sem MostRecent: não chuta.
        let duas = "\"users\" { \"76561198000000001\" { } \"76561198000000002\" { } }";
        assert_eq!(conta_recente(duas), None);
    }
}
