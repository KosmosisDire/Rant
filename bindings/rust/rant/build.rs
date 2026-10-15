// Compiles the amalgamated C library into the crate, so a Rust program needs no DLL.

include!("dist.rs");

fn main() {
    let crate_dir = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let dist = dist_dir(&crate_dir);
    let source = dist.join("rant.c");
    if !source.exists() {
        panic!("rant: {} is missing. Build Rant with CMake first, or set RANT_DIST", source.display());
    }
    println!("cargo:rerun-if-env-changed=RANT_DIST");
    println!("cargo:rerun-if-changed={}", source.display());
    println!("cargo:rerun-if-changed={}", dist.join("rant.h").display());
    cc::Build::new().file(&source).include(&dist).warnings(false).compile("rant");

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
