use std::{env, fs, path::PathBuf};

fn main() {
    // A DLL do proxy vai embutida no .exe (o app instala ela sozinho).
    // Sem GF_PROXY_DLL (build local), embute vazio e o botão Instalar avisa.
    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("proxy.dll");
    match env::var("GF_PROXY_DLL") {
        Ok(p) => {
            fs::copy(&p, &out).unwrap_or_else(|e| panic!("GF_PROXY_DLL={p}: {e}"));
            println!("cargo:rerun-if-changed={p}");
        }
        Err(_) => fs::write(&out, b"").unwrap(),
    }
    println!("cargo:rerun-if-env-changed=GF_PROXY_DLL");

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        tauri_build::build();
    }
}
