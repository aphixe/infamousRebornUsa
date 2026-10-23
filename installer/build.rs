use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=assets/infamous.ico");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let icon =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("assets/infamous.ico");
    let rc = out.join("infamous-icon.rc");
    let res = out.join("infamous-icon.res");
    fs::write(
        &rc,
        format!("1 ICON \"{}\"\n", icon.to_string_lossy().replace('\\', "/")),
    )
    .unwrap();
    let compiler = if let Some(path) = env::var_os("WindowsSdkVerBinPath") {
        PathBuf::from(path).join("x64/rc.exe")
    } else {
        let kits = PathBuf::from(
            env::var_os("ProgramFiles(x86)").unwrap_or_else(|| "C:/Program Files (x86)".into()),
        )
        .join("Windows Kits/10/bin");
        let mut candidates: Vec<_> = fs::read_dir(kits)
            .expect("Windows SDK is required")
            .flatten()
            .map(|entry| entry.path().join("x64/rc.exe"))
            .filter(|path| path.is_file())
            .collect();
        candidates.sort();
        candidates
            .pop()
            .expect("Windows SDK resource compiler rc.exe is required")
    };
    let status = Command::new(compiler)
        .arg("/nologo")
        .arg("/fo")
        .arg(&res)
        .arg(&rc)
        .status()
        .expect("Could not run rc.exe");
    assert!(status.success(), "Icon resource compilation failed");
    println!("cargo:rustc-link-arg-bin=infamous-launch={}", res.display());
    if env::var_os("CARGO_FEATURE_GUI").is_some() {
        println!("cargo:rustc-link-arg-bin=infamous-setup={}", res.display());
    }
}
