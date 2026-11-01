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

    // Autoteste: o teste ponta a ponta (vrserver falso + Pico falso) vai embutido para rodar
    // na GPU do usuário. Sem as variáveis (build local), embute vazio e o botão avisa.
    for (var, nome) in [("GF_AUTOTESTE_EXE", "autoteste.exe"), ("GF_AUTOTESTE_PICO", "autoteste_pico.dll")] {
        let destino = PathBuf::from(env::var("OUT_DIR").unwrap()).join(nome);
        match env::var(var) {
            Ok(p) => {
                fs::copy(&p, &destino).unwrap_or_else(|e| panic!("{var}={p}: {e}"));
                println!("cargo:rerun-if-changed={p}");
            }
            Err(_) => fs::write(&destino, b"").unwrap(),
        }
        println!("cargo:rerun-if-env-changed={var}");
    }

    // Ponte cliente OpenVR (src/vr.rs). Compila em todo alvo para o `cargo test` do
    // Linux pegar erro de C++ e de link antes do CI do Windows.
    cc::Build::new()
        .cpp(true)
        .file("native/gf_openvr.cpp")
        .include("../third_party/openvr")
        .flag_if_supported("-Wno-unused-variable") // constantes de versão do openvr.h
        .compile("gf_openvr");
    println!("cargo:rerun-if-changed=native/gf_openvr.cpp");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        println!("cargo:rustc-link-lib=d3d11"); // textura do HUD (gf_overlay_imagem)
    }
    println!("cargo:rerun-if-changed=../third_party/openvr/openvr.h");

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        tauri_build::build();
    }
}
