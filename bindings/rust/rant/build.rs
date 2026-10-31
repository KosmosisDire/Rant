// Compiles the amalgamated C library into the crate, so a Rust program needs no DLL.

include!("dist.rs");

fn main() {
    let crate_dir = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("dist");
    let dist = amalgamate(&crate_dir, &out).unwrap_or_else(|e| panic!("rant: {e}"));
    let root = crate_dir.ancestors().nth(3).unwrap_or(&crate_dir).to_path_buf();
    for watched in [crate_dir.join("csrc"), root.join("src"), root.join("tools").join("pack.cmake"), root.join("VERSION")] {
        println!("cargo:rerun-if-changed={}", watched.display());
    }
    cc::Build::new().file(dist.join("rant.c")).include(&dist).warnings(false).compile("rant");

    // the OS libraries tools/rant_platform.cmake names
    let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap();
    let libs: &[&str] = match os.as_str() {
        "windows" => &["ws2_32", "bcrypt", "winmm"],
        "linux" => &["rt", "pthread"],
        _ => &[],
    };
    for lib in libs {
        println!("cargo:rustc-link-lib={lib}");
    }
}
