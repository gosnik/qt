//! Builds the pinned PrismML llama.cpp fork with CMake and generates bindings.
//!
//! Environment:
//! - `QT_LLAMA_CPP_DIR`: llama.cpp source tree (default
//!   `<workspace>/third_party/llama.cpp`).
//! - `QT_LLAMA_GPU_BACKENDS`: `auto` (default), `cpu`/`off`/`none`, `all`, or
//!   a list such as `cuda,vulkan`.
//! - `QT_LLAMA_BUILD_PROFILE=full`: release profile (CUDA+Vulkan+HIP by
//!   default, broad CUDA architectures, NCCL/RCCL off).
//! - `QT_LLAMA_CMAKE_ARGS`: extra CMake configure arguments (shell words).
//! - `QT_LLAMA_CMAKE_GENERATOR`: CMake generator (default Ninja when found).
//! - `QT_LLAMA_BUILD_JOBS`: parallel build jobs (default Cargo's `NUM_JOBS`).
//! - `QT_LLAMA_REQUIRE=1`: fail instead of building without llama.cpp when
//!   the source tree is missing.

#[allow(dead_code)]
#[path = "build/backends.rs"]
mod backends;

use std::collections::BTreeSet;
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

use backends::{BackendRequest, ConfigureOptions, Probe, SystemProbe};

const WATCHED_ENV: [&str; 12] = [
    "QT_LLAMA_CPP_DIR",
    "QT_LLAMA_GPU_BACKENDS",
    "QT_LLAMA_BUILD_PROFILE",
    "QT_LLAMA_CMAKE_ARGS",
    "QT_LLAMA_CMAKE_GENERATOR",
    "QT_LLAMA_BUILD_JOBS",
    "QT_LLAMA_REQUIRE",
    "VULKAN_SDK",
    "Vulkan_ROOT",
    "CUDAToolkit_ROOT",
    "ROCM_PATH",
    "LIBCLANG_PATH",
];

/// Source files whose change should trigger a rebuild of llama.cpp. The CMake
/// build itself is incremental, so this only needs to catch submodule bumps.
const WATCHED_SOURCES: [&str; 6] = [
    "CMakeLists.txt",
    "include/llama.h",
    "src/llama.cpp",
    "src/llama-ext.h",
    "ggml/include/ggml.h",
    "ggml/src/ggml.c",
];

fn main() {
    for var in WATCHED_ENV {
        println!("cargo:rerun-if-env-changed={var}");
    }
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=build/backends.rs");
    println!("cargo:rerun-if-changed=cmake");
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo::rustc-check-cfg=cfg(llama_available)");

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let source_dir = env::var_os("QT_LLAMA_CPP_DIR")
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| {
            manifest_dir
                .ancestors()
                .nth(2)
                .unwrap_or(&manifest_dir)
                .join("third_party/llama.cpp")
        });

    if !source_dir.join("include/llama.h").is_file() {
        let reason = format!(
            "llama.cpp source tree is missing at {}; run `git submodule update --init third_party/llama.cpp` or set QT_LLAMA_CPP_DIR",
            source_dir.display()
        );
        if env::var("QT_LLAMA_REQUIRE").is_ok_and(|v| v == "1" || v.eq_ignore_ascii_case("true")) {
            panic!("{reason}");
        }
        println!("cargo:warning=building without in-process llama.cpp: {reason}");
        if source_dir.is_dir() {
            println!("cargo:rerun-if-changed={}", source_dir.display());
        }
        println!("cargo:rustc-env=QT_LLAMA_BACKENDS=");
        println!("cargo:rustc-env=QT_LLAMA_UNAVAILABLE_REASON={reason}");
        println!("cargo:available=0");
        return;
    }
    for rel in WATCHED_SOURCES {
        let path = source_dir.join(rel);
        if path.exists() {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }

    let cmake = backends::which("cmake")
        .unwrap_or_else(|| panic!("llama.cpp integration requires `cmake` for the native build"));
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let full_profile =
        env::var("QT_LLAMA_BUILD_PROFILE").is_ok_and(|v| v.eq_ignore_ascii_case("full"));

    let requested = env::var("QT_LLAMA_GPU_BACKENDS")
        .unwrap_or_else(|_| backends::default_request(full_profile).to_string());
    let request = backends::parse_request(&requested);
    let probe = SystemProbe {
        target_os: target_os.clone(),
    };
    let selected = backends::select_backends(&request, &probe as &dyn Probe);
    if selected.contains("vulkan") && !backends::spirv_header_present() {
        panic!(
            "llama.cpp Vulkan support requires SPIR-V Headers (`spirv.hpp`); install the `spirv-headers` system package or provide a Vulkan SDK through VULKAN_SDK, Vulkan_ROOT, CPATH, or CPLUS_INCLUDE_PATH"
        );
    }

    let extra_args = env::var("QT_LLAMA_CMAKE_ARGS")
        .ok()
        .map(|args| {
            backends::split_shell_words(&args)
                .unwrap_or_else(|err| panic!("invalid QT_LLAMA_CMAKE_ARGS: {err}"))
        })
        .unwrap_or_default();
    let options = ConfigureOptions {
        full_profile,
        cuda_architectures: if selected.contains("cuda") && !full_profile {
            backends::cuda_architectures_present()
        } else {
            Vec::new()
        },
        hip_architectures: if selected.contains("hip") && !full_profile {
            backends::hip_architectures_present()
        } else {
            Vec::new()
        },
        opencl: if selected.contains("opencl") {
            backends::opencl_locations()
        } else {
            None
        },
        extra_args,
    };

    let builder = CmakeBuild {
        cmake,
        wrapper_dir: manifest_dir.join("cmake"),
        source_dir: source_dir.clone(),
        generator: env::var("QT_LLAMA_CMAKE_GENERATOR")
            .ok()
            .filter(|g| !g.is_empty())
            .or_else(|| backends::which("ninja").map(|_| "Ninja".to_string())),
        jobs: env::var("QT_LLAMA_BUILD_JOBS")
            .ok()
            .or_else(|| env::var("NUM_JOBS").ok())
            .filter(|j| !j.is_empty()),
    };

    // Informational (shown with `cargo build -vv`); only problems are warnings.
    println!("llama.cpp GPU backends: {}", describe(&selected));
    let primary_dir = out_dir.join("build");
    let (build_dir, backends_built) = match builder.build(&primary_dir, &selected, &options) {
        Ok(()) => (primary_dir, selected),
        Err(err) if request == BackendRequest::Auto && !selected.is_empty() => {
            println!(
                "cargo:warning=llama.cpp build with GPU backends ({}) failed: {err}; falling back to a CPU-only build",
                describe(&selected)
            );
            let cpu_dir = out_dir.join("build-cpu");
            let cpu = BTreeSet::new();
            builder
                .build(&cpu_dir, &cpu, &options)
                .unwrap_or_else(|err| panic!("llama.cpp CPU-only build failed: {err}"));
            (cpu_dir, cpu)
        }
        Err(err) => panic!("llama.cpp build failed: {err}"),
    };

    let archives = collect_archives(&build_dir);
    for required in ["llama", "ggml", "ggml-base", "ggml-cpu", "qt_llama_shim"] {
        if !archives.iter().any(|(name, _)| name == required) {
            panic!(
                "llama.cpp build did not produce expected static library `{required}` under {}",
                build_dir.display()
            );
        }
    }
    let names: Vec<String> = archives.iter().map(|(n, _)| n.clone()).collect();
    let mut search_dirs: BTreeSet<PathBuf> = BTreeSet::new();
    for (_, dir) in &archives {
        search_dirs.insert(dir.clone());
    }
    for dir in &search_dirs {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    for name in backends::order_archives(&names) {
        println!("cargo:rustc-link-lib=static={name}");
    }

    let archive_files: Vec<String> = names.iter().map(|n| format!("lib{n}.a")).collect();
    let mut linked_backends = backends::built_backends(archive_files.iter().map(String::as_str));
    linked_backends.extend(backends_built.iter().cloned());
    link_system_libraries(&target_os, &linked_backends, &build_dir);

    let backend_list: Vec<&str> = linked_backends.iter().map(String::as_str).collect();
    println!(
        "cargo:rustc-env=QT_LLAMA_BACKENDS={}",
        backend_list.join(",")
    );
    println!("cargo:rustc-env=QT_LLAMA_UNAVAILABLE_REASON=");
    println!("cargo:rustc-cfg=llama_available");
    println!("cargo:available=1");
    println!("cargo:backends={}", backend_list.join(","));
    println!("cargo:include={}", source_dir.join("include").display());

    generate_bindings(&source_dir, &manifest_dir, &out_dir);
}

fn describe(backends: &BTreeSet<String>) -> String {
    if backends.is_empty() {
        "none".to_string()
    } else {
        backends.iter().cloned().collect::<Vec<_>>().join(", ")
    }
}

struct CmakeBuild {
    cmake: PathBuf,
    wrapper_dir: PathBuf,
    source_dir: PathBuf,
    generator: Option<String>,
    jobs: Option<String>,
}

impl CmakeBuild {
    fn build(
        &self,
        build_dir: &Path,
        backends: &BTreeSet<String>,
        options: &ConfigureOptions,
    ) -> Result<(), String> {
        std::fs::create_dir_all(build_dir).map_err(|e| e.to_string())?;
        let mut configure = Command::new(&self.cmake);
        configure
            .arg("-S")
            .arg(&self.wrapper_dir)
            .arg("-B")
            .arg(build_dir)
            .arg(format!("-DQT_LLAMA_CPP_DIR={}", self.source_dir.display()));
        // A generator cannot change once a build tree exists.
        if !build_dir.join("CMakeCache.txt").exists()
            && let Some(generator) = &self.generator
        {
            configure.arg("-G").arg(generator);
        }
        configure.args(backends::cmake_cache_args(backends, options));
        run(&mut configure, "configure")?;

        if backends.contains("vulkan") {
            // Shader generation spawns its own compiler pool; build it alone.
            let mut prebuild = Command::new(&self.cmake);
            prebuild.arg("--build").arg(build_dir).args([
                "--target",
                "ggml-vulkan",
                "--parallel",
                "1",
            ]);
            run(&mut prebuild, "Vulkan shader prebuild")?;
        }

        let mut build = Command::new(&self.cmake);
        build.arg("--build").arg(build_dir).args([
            "--target",
            "qt_llama_shim",
            "--config",
            "Release",
        ]);
        if let Some(jobs) = &self.jobs {
            build.args(["--parallel", jobs]);
        }
        run(&mut build, "build")
    }
}

fn run(command: &mut Command, what: &str) -> Result<(), String> {
    let output = command
        .output()
        .map_err(|e| format!("could not run cmake {what}: {e}"))?;
    if output.status.success() {
        return Ok(());
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    let tail = |text: &str| {
        let lines: Vec<&str> = text.lines().collect();
        lines[lines.len().saturating_sub(40)..].join("\n")
    };
    Err(format!(
        "cmake {what} exited with {}\n{}\n{}",
        output.status,
        tail(&stdout),
        tail(&stderr)
    ))
}

/// `(library name, directory)` of every static archive under `dir`.
fn collect_archives(dir: &Path) -> Vec<(String, PathBuf)> {
    let mut found = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(current) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&current) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                if path.file_name().is_some_and(|n| n == "CMakeFiles") {
                    continue;
                }
                stack.push(path);
                continue;
            }
            let Some(file) = path.file_name().and_then(|n| n.to_str()) else {
                continue;
            };
            let name = file
                .strip_prefix("lib")
                .and_then(|n| n.strip_suffix(".a"))
                .or_else(|| file.strip_suffix(".lib"));
            let Some(name) = name else { continue };
            let wanted = name == "llama"
                || name == "ggml"
                || name.starts_with("ggml-")
                || name == "qt_llama_shim";
            if wanted && !found.iter().any(|(n, _): &(String, PathBuf)| n == name) {
                found.push((name.to_string(), current.clone()));
            }
        }
    }
    found.sort();
    found
}

fn cmake_cache_value(build_dir: &Path, key: &str) -> Option<PathBuf> {
    let cache = std::fs::read_to_string(build_dir.join("CMakeCache.txt")).ok()?;
    let prefix = format!("{key}:");
    cache.lines().find_map(|line| {
        let rest = line.strip_prefix(&prefix)?;
        let value = rest.split_once('=')?.1.trim();
        (!value.is_empty() && !value.ends_with("-NOTFOUND")).then(|| PathBuf::from(value))
    })
}

fn link_system_libraries(target_os: &str, backends: &BTreeSet<String>, build_dir: &Path) {
    let mut dirs: Vec<PathBuf> = Vec::new();
    if backends.contains("cuda") {
        if let Some(cudart) = cmake_cache_value(build_dir, "CUDA_CUDART")
            && let Some(parent) = cudart.parent()
        {
            dirs.push(parent.to_path_buf());
        }
        if let Some(root) = env::var_os("CUDAToolkit_ROOT") {
            dirs.push(PathBuf::from(root).join("lib64"));
        }
        dirs.push(PathBuf::from("/usr/local/cuda/lib64"));
        dirs.push(PathBuf::from("/usr/lib/x86_64-linux-gnu"));
    }
    if backends.contains("hip") || backends.contains("opencl") {
        if let Some(root) = env::var_os("ROCM_PATH") {
            dirs.push(PathBuf::from(root).join("lib"));
        }
        dirs.push(PathBuf::from("/opt/rocm/lib"));
    }
    if backends.contains("vulkan") {
        for var in ["VULKAN_SDK", "Vulkan_ROOT"] {
            if let Some(root) = env::var_os(var) {
                dirs.push(PathBuf::from(root).join("lib"));
            }
        }
    }
    let mut seen = BTreeSet::new();
    for dir in dirs {
        if dir.is_dir() && seen.insert(dir.clone()) {
            println!("cargo:rustc-link-search=native={}", dir.display());
        }
    }
    for lib in backends::system_libraries(backends) {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }
    match target_os {
        "macos" | "ios" => {
            println!("cargo:rustc-link-lib=dylib=c++");
            println!("cargo:rustc-link-lib=framework=Foundation");
            println!("cargo:rustc-link-lib=framework=Accelerate");
            if backends.contains("metal") {
                println!("cargo:rustc-link-lib=framework=Metal");
                println!("cargo:rustc-link-lib=framework=MetalKit");
            }
        }
        "windows" => {}
        _ => {
            println!("cargo:rustc-link-lib=dylib=stdc++");
            println!("cargo:rustc-link-lib=dylib=pthread");
            println!("cargo:rustc-link-lib=dylib=dl");
            println!("cargo:rustc-link-lib=dylib=m");
            if backends.contains("sycl") {
                println!("cargo:rustc-link-arg=-fsycl");
            }
        }
    }
}

fn generate_bindings(source_dir: &Path, manifest_dir: &Path, out_dir: &Path) {
    let bindings = bindgen::Builder::default()
        .header(manifest_dir.join("wrapper.h").to_string_lossy())
        .clang_arg(format!("-I{}", source_dir.join("include").display()))
        .clang_arg(format!("-I{}", source_dir.join("ggml/include").display()))
        .clang_arg(format!("-I{}", manifest_dir.join("cmake").display()))
        .allowlist_function("llama_.*")
        .allowlist_function("ggml_backend_.*")
        .allowlist_function("qt_llama_.*")
        .allowlist_type("llama_.*")
        .allowlist_type("qt_llama_.*")
        .allowlist_type("ggml_backend_.*")
        .allowlist_var("LLAMA_.*")
        .prepend_enum_name(false)
        .layout_tests(false)
        .generate_comments(false)
        .derive_default(true)
        .parse_callbacks(Box::new(
            bindgen::CargoCallbacks::new().rerun_on_header_files(false),
        ))
        .generate()
        .unwrap_or_else(|err| panic!("failed to generate llama.cpp bindings: {err}"));
    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .unwrap_or_else(|err| panic!("failed to write llama.cpp bindings: {err}"));
}
