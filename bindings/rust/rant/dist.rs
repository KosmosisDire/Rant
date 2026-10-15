// Where the amalgamated rant.h and rant.c are, shared by build.rs and ffigen through include!.
fn dist_dir(crate_dir: &std::path::Path) -> std::path::PathBuf {
    if let Some(dir) = std::env::var_os("RANT_DIST") {
        return dir.into();
    }
    // a packaged crate carries its copy, a checkout reads the build's dist/
    let packaged = crate_dir.join("csrc");
    if packaged.join("rant.h").exists() {
        return packaged;
    }
    // bindings/rust/rant sits three levels under the library root
    crate_dir.ancestors().nth(3).unwrap_or(crate_dir).join("dist")
}
