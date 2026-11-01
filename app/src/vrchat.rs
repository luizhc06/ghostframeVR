//! Opções do VRChat que o app ajusta com o jogo FECHADO, no registro do Windows
//! (PlayerPrefs da Unity em HKCU\Software\VRChat\VRChat). Nunca encosta no processo do
//! jogo (Easy Anti-Cheat). Os nomes e valores foram conferidos num registro real do
//! VRChat (o print do menu ao lado do valor gravado), não chutados.

use serde_json::{json, Map, Value};

pub const CHAVE_REGISTRO: &str = r"Software\VRChat\VRChat";
pub const REG_BINARY: u32 = 3;
pub const REG_DWORD: u32 = 4;

/// Nome do valor no registro: `<chave>_h<hash>`, hash djb2 com XOR da Unity.
pub fn nome(chave: &str) -> String {
    let h = chave.bytes().fold(5381u32, |h, c| h.wrapping_mul(33) ^ u32::from(c));
    format!("{chave}_h{h}")
}

/// Valor de PlayerPrefs: int em REG_DWORD de 4 bytes, float em REG_DWORD de 8 bytes
/// (double), texto em REG_BINARY terminado em zero.
#[derive(Debug, Clone, PartialEq)]
pub enum Valor {
    Int(i32),
    Float(f64),
    Texto(String),
}

impl Valor {
    pub fn de(tipo: u32, dados: &[u8]) -> Option<Valor> {
        match (tipo, dados.len()) {
            (REG_DWORD, 4) => Some(Valor::Int(i32::from_le_bytes(dados.try_into().ok()?))),
            (REG_DWORD, 8) => Some(Valor::Float(f64::from_le_bytes(dados.try_into().ok()?))),
            (REG_BINARY, _) => Some(Valor::Texto(String::from_utf8_lossy(dados.split(|&b| b == 0).next()?).into())),
            _ => None,
        }
    }
    pub fn para(&self) -> (u32, Vec<u8>) {
        match self {
            Valor::Int(i) => (REG_DWORD, i.to_le_bytes().to_vec()),
            Valor::Float(f) => (REG_DWORD, f.to_le_bytes().to_vec()),
            Valor::Texto(t) => (REG_BINARY, t.bytes().chain([0]).collect()),
        }
    }
}

/// Um ajuste da aba VRChat. `por_conta`: o VRChat também guarda uma cópia por conta
/// (`usr_<id>_<chave>`), que vale sobre a global; gravamos as duas.
pub struct Ajuste {
    pub id: &'static str,
    pub chave: &'static str,
    pub por_conta: bool,
    /// O que um VRChat recém-instalado grava (registro real): "desligar" o ajuste volta a isto.
    pub padrao: i32,
    /// Valor do Modo leve.
    pub leve: i32,
}

pub const AJUSTES: &[Ajuste] = &[
    // 5 não bloqueia, 4 bloqueia Very Poor, 3 bloqueia Poor e abaixo (Minimum Displayed Performance Rank).
    // O Modo leve usa 3 ("Medium"), o que o VRChat sugere na ajuda de desempenho.
    Ajuste { id: "rank", chave: "VRC_AVATAR_PERFORMANCE_RATING_MINIMUM_TO_DISPLAY", por_conta: false, padrao: 5, leve: 3 },
    Ajuste { id: "maxLigado", chave: "currentShowMaxNumberOfAvatarsEnabled", por_conta: true, padrao: 0, leve: 1 },
    Ajuste { id: "maxAvatares", chave: "avatarProxyShowMaxNumber", por_conta: true, padrao: 80, leve: 15 },
    // GPU (sobra GPU para o frame gen): Quarter = 2, Low = 0, Low = 0, Disabled = 1.
    Ajuste { id: "espelho", chave: "VRC_MIRROR_RESOLUTION", por_conta: false, padrao: 0, leve: 2 },
    Ajuste { id: "sombras", chave: "SHADOW_QUALITY", por_conta: false, padrao: 2, leve: 0 },
    Ajuste { id: "lod", chave: "LOD_QUALITY", por_conta: false, padrao: 1, leve: 0 },
    Ajuste { id: "aa", chave: "VRC_ADVANCED_GRAPHICS_ANTIALIASING", por_conta: false, padrao: 4, leve: 1 },
];

pub fn ajuste(id: &str) -> Option<&'static Ajuste> {
    AJUSTES.iter().find(|a| a.id == id)
}

/// Ids das contas que já entraram neste PC (`usr_<uuid>` em qualquer parte dos nomes).
pub fn contas(nomes: &[String]) -> Vec<String> {
    let mut r: Vec<String> = Vec::new();
    for n in nomes {
        let mut resto = n.as_str();
        while let Some(i) = resto.find("usr_") {
            let id = &resto[i..];
            let uuid = id.get(4..40).unwrap_or("");
            let ok = uuid.len() == 36
                && uuid.char_indices().all(|(j, c)| if [8, 13, 18, 23].contains(&j) { c == '-' } else { c.is_ascii_hexdigit() });
            if ok && !r.iter().any(|x| x == &id[..40]) {
                r.push(id[..40].to_string());
            }
            resto = &resto[i + 4..];
        }
    }
    r
}

/// Todos os nomes de registro de um ajuste: o global e o de cada conta.
pub fn nomes_do_ajuste(a: &Ajuste, contas: &[String]) -> Vec<String> {
    let mut v = vec![nome(a.chave)];
    if a.por_conta {
        v.extend(contas.iter().map(|c| nome(&format!("{c}_{}", a.chave))));
    }
    v
}

/// Valor atual de um ajuste: o da conta (se houver uma só e ela tiver), senão o global.
pub fn valor_atual(a: &Ajuste, contas: &[String], ler: &dyn Fn(&str) -> Option<Valor>) -> Option<i32> {
    let nomes = nomes_do_ajuste(a, contas);
    let por_conta = if contas.len() == 1 { nomes.get(1) } else { None };
    por_conta.and_then(|n| ler(n)).or_else(|| ler(&nomes[0])).and_then(|v| match v {
        Valor::Int(i) => Some(i),
        _ => None,
    })
}

/// Valores válidos de cada ajuste (o app recusa qualquer outro).
pub fn valido(id: &str, v: i32) -> bool {
    match id {
        "rank" => (3..=5).contains(&v),
        "maxLigado" => v == 0 || v == 1,
        "maxAvatares" => (1..=80).contains(&v),
        "espelho" => v == 0 || v == 2,
        "sombras" => v == 0 || v == 2,
        "lod" => v == 0 || v == 1,
        "aa" => v == 1 || v == 4,
        _ => false,
    }
}

/// Leitura crua do registro: Ok(None) só quando o valor não existe; erro de leitura é erro.
pub type LerCru<'a> = &'a dyn Fn(&str) -> Result<Option<(u32, Vec<u8>)>, String>;

/// Backup: nome → {tipo, bytes} do que havia antes da 1ª mudança (null = não existia).
/// Só entra quem ainda não está no backup: restaurar volta ao estado de antes do app.
pub fn completar_backup(backup: &mut Map<String, Value>, nomes: &[String], ler_cru: LerCru) -> Result<(), String> {
    for n in nomes {
        if !backup.contains_key(n) {
            let v = ler_cru(n)?.map_or(Value::Null, |(t, d)| json!({ "tipo": t, "dados": d }));
            backup.insert(n.clone(), v);
        }
    }
    Ok(())
}

/// O que restaurar: nome → Some((tipo, bytes)) para gravar, None para apagar (só se o
/// backup diz null). Entrada estragada é erro: nunca vira "apagar".
pub fn restauracao(backup: &Map<String, Value>) -> Result<Vec<(String, Option<(u32, Vec<u8>)>)>, String> {
    backup
        .iter()
        .map(|(n, v)| {
            if v.is_null() {
                return Ok((n.clone(), None));
            }
            let cru = v.as_object().and_then(|o| {
                let t = u32::try_from(o.get("tipo")?.as_u64()?).ok()?;
                let d: Vec<u8> = o.get("dados")?.as_array()?.iter().map(|b| b.as_u64().and_then(|x| u8::try_from(x).ok())).collect::<Option<_>>()?;
                Some((t, d))
            });
            cru.map(|c| (n.clone(), Some(c))).ok_or(format!("backup do VRChat estragado em {n}: não restaurei nada"))
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Registro do Windows (advapi32), só o necessário. Sem crate: são 6 funções.
#[cfg(windows)]
pub mod reg {
    use super::CHAVE_REGISTRO;
    use std::ffi::c_void;

    type Hkey = *mut c_void;
    const HKEY_CURRENT_USER: Hkey = 0x8000_0001u32 as i32 as isize as Hkey; // como no SDK: estendido com sinal
    const KEY_READ: u32 = 0x20019;
    const KEY_SET_VALUE: u32 = 0x0002;
    const ERROR_SUCCESS: i32 = 0;
    const ERROR_FILE_NOT_FOUND: i32 = 2;

    #[link(name = "advapi32")]
    extern "system" {
        fn RegOpenKeyExW(k: Hkey, sub: *const u16, op: u32, sam: u32, r: *mut Hkey) -> i32;
        fn RegCloseKey(k: Hkey) -> i32;
        fn RegEnumValueW(k: Hkey, i: u32, nome: *mut u16, tnome: *mut u32, res: *mut u32, tipo: *mut u32, d: *mut u8, td: *mut u32) -> i32;
        fn RegQueryValueExW(k: Hkey, nome: *const u16, res: *mut u32, tipo: *mut u32, d: *mut u8, td: *mut u32) -> i32;
        fn RegSetValueExW(k: Hkey, nome: *const u16, res: u32, tipo: u32, d: *const u8, td: u32) -> i32;
        fn RegDeleteValueW(k: Hkey, nome: *const u16) -> i32;
    }

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().chain([0]).collect()
    }

    struct Chave(Hkey);
    impl Drop for Chave {
        fn drop(&mut self) {
            unsafe { RegCloseKey(self.0) };
        }
    }
    fn abrir(acesso: u32) -> Option<Chave> {
        let mut k: Hkey = std::ptr::null_mut();
        let r = unsafe { RegOpenKeyExW(HKEY_CURRENT_USER, w(CHAVE_REGISTRO).as_ptr(), 0, acesso, &mut k) };
        (r == ERROR_SUCCESS).then_some(Chave(k))
    }

    /// Nomes dos valores. Vazio = o VRChat ainda não rodou neste usuário.
    pub fn nomes() -> Vec<String> {
        let Some(k) = abrir(KEY_READ) else { return vec![] };
        let mut v = Vec::new();
        let mut buf = vec![0u16; 16384]; // limite de nome de valor do Windows
        for i in 0.. {
            let mut t = buf.len() as u32;
            let r = unsafe {
                RegEnumValueW(k.0, i, buf.as_mut_ptr(), &mut t, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut())
            };
            if r != ERROR_SUCCESS {
                break; // ERROR_NO_MORE_ITEMS: acabou
            }
            v.push(String::from_utf16_lossy(&buf[..t as usize]));
        }
        v
    }

    /// Ok(None) = o valor não existe. Qualquer outro problema é Err (o backup não pode
    /// achar que "não existia" e o Restaurar apagar um valor que existe).
    pub fn ler_cru(nome: &str) -> Result<Option<(u32, Vec<u8>)>, String> {
        let Some(k) = abrir(KEY_READ) else { return Ok(None) };
        let n = w(nome);
        let (mut tipo, mut tam) = (0u32, 0u32);
        let r = unsafe { RegQueryValueExW(k.0, n.as_ptr(), std::ptr::null_mut(), &mut tipo, std::ptr::null_mut(), &mut tam) };
        if r == ERROR_FILE_NOT_FOUND {
            return Ok(None);
        }
        if r != ERROR_SUCCESS {
            return Err(format!("registro do VRChat: erro {r} ao ler {nome}"));
        }
        let mut d = vec![0u8; tam as usize];
        let r = unsafe { RegQueryValueExW(k.0, n.as_ptr(), std::ptr::null_mut(), &mut tipo, d.as_mut_ptr(), &mut tam) };
        if r != ERROR_SUCCESS {
            return Err(format!("registro do VRChat: erro {r} ao ler {nome}"));
        }
        d.truncate(tam as usize);
        Ok(Some((tipo, d)))
    }

    pub fn ler(nome: &str) -> Option<(u32, Vec<u8>)> {
        ler_cru(nome).ok().flatten()
    }

    pub fn gravar(nome: &str, tipo: u32, dados: &[u8]) -> Result<(), String> {
        let k = abrir(KEY_SET_VALUE).ok_or("o VRChat ainda não rodou neste usuário do Windows")?;
        let r = unsafe { RegSetValueExW(k.0, w(nome).as_ptr(), 0, tipo, dados.as_ptr(), dados.len() as u32) };
        if r == ERROR_SUCCESS { Ok(()) } else { Err(format!("registro do VRChat: erro {r} em {nome}")) }
    }

    pub fn apagar(nome: &str) -> Result<(), String> {
        let k = abrir(KEY_SET_VALUE).ok_or("o VRChat ainda não rodou neste usuário do Windows")?;
        let r = unsafe { RegDeleteValueW(k.0, w(nome).as_ptr()) };
        if r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND { Ok(()) } else { Err(format!("registro do VRChat: erro {r} ao apagar {nome}")) }
    }
}

#[cfg(test)]
mod testes {
    use super::*;

    #[test]
    fn nomes_iguais_aos_do_registro_real() {
        // Tirados do registro de um VRChat real (Proton), não calculados à mão.
        assert_eq!(nome("VRC_AVATAR_PERFORMANCE_RATING_MINIMUM_TO_DISPLAY"), "VRC_AVATAR_PERFORMANCE_RATING_MINIMUM_TO_DISPLAY_h1859707483");
        assert_eq!(nome("avatarProxyShowMaxNumber"), "avatarProxyShowMaxNumber_h171091884");
        assert_eq!(nome("currentShowMaxNumberOfAvatarsEnabled"), "currentShowMaxNumberOfAvatarsEnabled_h1218052438");
        assert_eq!(nome("VRC_MIRROR_RESOLUTION"), "VRC_MIRROR_RESOLUTION_h4279880629");
        assert_eq!(nome("SHADOW_QUALITY"), "SHADOW_QUALITY_h2860313905");
        assert_eq!(nome("LOD_QUALITY"), "LOD_QUALITY_h4091515664");
        assert_eq!(nome("VRC_ADVANCED_GRAPHICS_ANTIALIASING"), "VRC_ADVANCED_GRAPHICS_ANTIALIASING_h4197584722");
        // Exemplo da própria documentação da Unity.
        assert_eq!(nome("DeckBase"), "DeckBase_h3232628825");
    }

    #[test]
    fn valores_no_formato_da_unity() {
        assert_eq!(Valor::de(REG_DWORD, &[0x50, 0, 0, 0]), Some(Valor::Int(80)));
        assert_eq!(Valor::de(REG_DWORD, &[0, 0, 0, 0, 0, 0, 0x59, 0x40]), Some(Valor::Float(100.0)));
        assert_eq!(Valor::de(REG_BINARY, b"High\0"), Some(Valor::Texto("High".into())));
        assert_eq!(Valor::de(7, &[1, 2]), None);
        for v in [Valor::Int(-1), Valor::Int(15), Valor::Float(0.5), Valor::Texto("Low".into())] {
            let (t, d) = v.para();
            assert_eq!(Valor::de(t, &d), Some(v));
        }
    }

    #[test]
    fn contas_e_nomes_por_conta() {
        let id = "usr_037f3c60-1111-2222-3333-444455556666";
        let nomes = vec![
            format!("FirstTimeEvent_First_Movement_{id}_h85267101"),
            format!("HasSeenShopRabbidsQMCallout{id}_h4227815174"),
            format!("{id}_LastExpiredSubscription_h3997970361"),
            "usr_naoehuuid_h1".to_string(),
            "SHADOW_QUALITY_h2860313905".to_string(),
        ];
        assert_eq!(contas(&nomes), vec![id.to_string()]);
        let a = ajuste("maxAvatares").unwrap();
        assert_eq!(nomes_do_ajuste(a, &contas(&nomes)), vec![nome("avatarProxyShowMaxNumber"), nome(&format!("{id}_avatarProxyShowMaxNumber"))]);
        assert_eq!(nomes_do_ajuste(ajuste("sombras").unwrap(), &contas(&nomes)).len(), 1);
        // Valor: o da conta vence o global.
        let por_conta = nome(&format!("{id}_avatarProxyShowMaxNumber"));
        let ler = |n: &str| if n == por_conta { Some(Valor::Int(15)) } else if n == nome("avatarProxyShowMaxNumber") { Some(Valor::Int(80)) } else { None };
        assert_eq!(valor_atual(a, &contas(&nomes), &ler), Some(15));
        assert_eq!(valor_atual(a, &[], &ler), Some(80));
    }

    #[test]
    fn modo_leve_padrao_e_validacao_coerentes() {
        for a in AJUSTES {
            assert!(valido(a.id, a.leve), "{} leve", a.id);
            assert!(valido(a.id, a.padrao), "{} padrão", a.id);
        }
        assert!(!valido("rank", 2) && !valido("maxAvatares", 0) && !valido("espelho", 1) && !valido("x", 0));
    }

    #[test]
    fn backup_guarda_so_o_primeiro_e_restaura() {
        let mut b = Map::new();
        let antes = |n: &str| Ok((n == "a").then(|| (REG_DWORD, vec![5, 0, 0, 0])));
        completar_backup(&mut b, &["a".into(), "b".into()], &antes).unwrap();
        // Uma segunda mudança não troca o "antes".
        completar_backup(&mut b, &["a".into()], &|_| Ok(Some((REG_DWORD, vec![4, 0, 0, 0])))).unwrap();
        let r = restauracao(&b).unwrap();
        assert!(r.contains(&("a".into(), Some((REG_DWORD, vec![5, 0, 0, 0])))));
        assert!(r.contains(&("b".into(), None))); // não existia: apaga
        // Erro de leitura não vira "não existia".
        assert!(completar_backup(&mut b, &["c".into()], &|_| Err("trava".into())).is_err());
        assert!(!b.contains_key("c"));
        // Entrada estragada não vira "apagar".
        let mut ruim = Map::new();
        ruim.insert("x".into(), json!({ "tipo": 4, "dados": [300] }));
        assert!(restauracao(&ruim).is_err());
        ruim.insert("x".into(), json!("lixo"));
        assert!(restauracao(&ruim).is_err());
    }
}
