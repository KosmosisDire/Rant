// The amalgamated rant.h and rant.c, shared by build.rs and ffigen through include!. The
// crate-v release tag carries them in csrc/, a plain Rant checkout packs src/ into out.
fn amalgamate(crate_dir: &std::path::Path, out: &std::path::Path) -> Result<std::path::PathBuf, String> {
    let packaged = crate_dir.join("csrc");
    if packaged.join("rant.c").is_file() {
        return Ok(packaged);
    }
    // bindings/rust/rant sits three levels under the library root
    let root = crate_dir.ancestors().nth(3).unwrap_or(crate_dir);
    let pack = root.join("tools").join("pack.cmake");
    if !pack.is_file() {
        return Err(format!("{} is missing: this crate is neither packaged nor in a Rant checkout", pack.display()));
    }
    // Packs into a folder of this run's own, then replaces only the files that changed: a
    // build moments after another found the same files still held open on Windows.
    let fresh = out.with_extension(format!("pack{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&fresh);
    let status = std::process::Command::new("cmake")
        .arg(format!("-DSRC={}", root.join("src").display()))
        .arg(format!("-DOUT={}", fresh.display()))
        .arg("-P")
        .arg(&pack)
        .stdout(std::process::Stdio::null())
        .status();
    match status {
        Ok(s) if s.success() => {}
        Ok(s) => return Err(format!("tools/pack.cmake failed ({s})")),
        Err(_) => return Err("cmake was not found: building Rant from source packs src/ with tools/pack.cmake, install cmake".to_owned()),
    }
    let fail = |p: &std::path::Path, e: std::io::Error| format!("{}: {e}", p.display());
    std::fs::create_dir_all(out).map_err(|e| fail(out, e))?;
    for entry in std::fs::read_dir(&fresh).map_err(|e| fail(&fresh, e))? {
        let from = entry.map_err(|e| fail(&fresh, e))?.path();
        let to = out.join(from.file_name().unwrap_or_default());
        let packed = std::fs::read(&from).map_err(|e| fail(&from, e))?;
        if std::fs::read(&to).ok().as_deref() != Some(packed.as_slice()) {
            std::fs::write(&to, &packed).map_err(|e| fail(&to, e))?;
        }
    }
    let _ = std::fs::remove_dir_all(&fresh);
    Ok(out.to_path_buf())
}
