// Links NotolibraEngine.framework from a Chromium output directory
// (NOTOLIBRA_ENGINE_OUT, default ~/chromium/src/out/Dev), with an rpath to it.
fn main() {
    let out = std::env::var("NOTOLIBRA_ENGINE_OUT").unwrap_or_else(|_| {
        format!("{}/chromium/src/out/Dev", std::env::var("HOME").unwrap())
    });
    println!("cargo:rerun-if-env-changed=NOTOLIBRA_ENGINE_OUT");
    println!("cargo:rustc-link-search=framework={out}");
    println!("cargo:rustc-link-lib=framework=NotolibraEngine");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{out}");
    println!("cargo:rustc-link-lib=framework=IOSurface");
    println!("cargo:rustc-link-lib=framework=CoreGraphics");
}
