// Embeds assets/poweroffd.ico into the exe using the Windows SDK resource compiler.
// No extra crates needed; if rc.exe can't be found the build still succeeds (no exe icon).
use std::{env, fs, path::PathBuf, process::Command};

fn find_rc() -> Option<PathBuf> {
    if Command::new("rc.exe").arg("/?").output().is_ok() {
        return Some(PathBuf::from("rc.exe"));
    }
    let kits = PathBuf::from(env::var_os("ProgramFiles(x86)")?).join("Windows Kits/10/bin");
    let mut dirs: Vec<_> = fs::read_dir(kits).ok()?.flatten().map(|e| e.path()).collect();
    dirs.sort();
    dirs.into_iter()
        .rev()
        .map(|d| d.join("x64/rc.exe"))
        .find(|p| p.exists())
}

fn main() {
    println!("cargo:rerun-if-changed=assets/poweroffd.rc");
    println!("cargo:rerun-if-changed=assets/poweroffd.ico");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let Some(rc) = find_rc() else {
        println!("cargo:warning=rc.exe not found; exe icon not embedded");
        return;
    };
    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("poweroffd.res");
    let ok = Command::new(rc)
        .args(["/nologo", "/fo"])
        .arg(&out)
        .arg("assets/poweroffd.rc")
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if ok {
        println!("cargo:rustc-link-arg-bins={}", out.display());
    } else {
        println!("cargo:warning=rc.exe failed; exe icon not embedded");
    }
}
