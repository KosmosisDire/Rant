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
    let status = std::process::Command::new("cmake")
        .arg(format!("-DSRC={}", root.join("src").display()))
        .arg(format!("-DOUT={}", out.display()))
        .arg("-P")
        .arg(&pack)
        .stdout(std::process::Stdio::null())
        .status();
    match status {
        Ok(s) if s.success() => Ok(out.to_path_buf()),
        Ok(s) => Err(format!("tools/pack.cmake failed ({s})")),
        Err(_) => Err("cmake was not found: building Rant from source packs src/ with tools/pack.cmake, install cmake".to_owned()),
    }
}
