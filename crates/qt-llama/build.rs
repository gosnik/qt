//! Enables the native llama.cpp backend when qt-llama-sys linked llama.cpp.
fn main() {
    println!("cargo::rustc-check-cfg=cfg(llama_native)");
    println!("cargo:rerun-if-env-changed=DEP_LLAMA_AVAILABLE");
    if std::env::var("DEP_LLAMA_AVAILABLE").is_ok_and(|v| v == "1") {
        println!("cargo:rustc-cfg=llama_native");
    }
}
