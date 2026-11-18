//! llama.cpp GPU backend selection and CMake argument construction.
//!
//! Ported from the archived `scripts/build_llama_cpp.py`. The logic is split
//! into pure functions (request parsing, selection, argument building) that
//! are unit tested from `src/lib.rs`, and a [`SystemProbe`] that inspects the
//! build machine.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// Backend names understood by the build (`ggml_` prefixes and dashes are
/// normalised away).
pub const ALL_BACKENDS: [&str; 6] = ["cuda", "hip", "metal", "vulkan", "opencl", "sycl"];

/// CUDA architectures compiled into the `full` (release) profile.
pub const FULL_CUDA_ARCHITECTURES: &str = "75;80;86;89;90";

/// What the build machine offers, as far as backend selection is concerned.
pub trait Probe {
    /// CUDA toolkit present and an NVIDIA GPU visible.
    fn cuda(&self) -> bool;
    /// ROCm/HIP toolkit present and an AMD GPU visible.
    fn hip(&self) -> bool;
    /// Building for macOS.
    fn metal(&self) -> bool;
    /// Vulkan headers, SPIR-V headers, `glslc`, loader and a device present.
    fn vulkan(&self) -> bool;
}

/// How `QT_LLAMA_GPU_BACKENDS` was interpreted.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BackendRequest {
    /// Detect installed SDKs and hardware.
    Auto,
    /// CPU only.
    Cpu,
    /// An explicit list (also produced by `all`).
    Explicit(BTreeSet<String>),
}

/// The default request: `auto` for developer builds, CUDA+Vulkan+HIP for the
/// `full` release profile (the archived `native_full` environment).
pub fn default_request(full_profile: bool) -> &'static str {
    if full_profile {
        "cuda,vulkan,hip"
    } else {
        "auto"
    }
}

/// Parse a `QT_LLAMA_GPU_BACKENDS` value.
pub fn parse_request(value: &str) -> BackendRequest {
    let requested = value.trim().to_ascii_lowercase();
    match requested.as_str() {
        "" | "auto" => BackendRequest::Auto,
        "0" | "false" | "no" | "off" | "cpu" | "none" => BackendRequest::Cpu,
        "all" => BackendRequest::Explicit(ALL_BACKENDS.iter().map(|s| s.to_string()).collect()),
        _ => BackendRequest::Explicit(
            requested
                .replace(';', ",")
                .split(',')
                .map(str::trim)
                .filter(|token| !token.is_empty())
                .map(|token| token.replace("ggml_", "").replace('-', "_"))
                .collect(),
        ),
    }
}

/// Resolve a request against the machine.
pub fn select_backends(request: &BackendRequest, probe: &dyn Probe) -> BTreeSet<String> {
    match request {
        BackendRequest::Cpu => BTreeSet::new(),
        BackendRequest::Explicit(set) => set.clone(),
        BackendRequest::Auto => {
            let mut backends = BTreeSet::new();
            if probe.cuda() {
                backends.insert("cuda".to_string());
            }
            if probe.hip() {
                backends.insert("hip".to_string());
            }
            if probe.metal() {
                backends.insert("metal".to_string());
            }
            if probe.vulkan() {
                backends.insert("vulkan".to_string());
            }
            backends
        }
    }
}

fn on_off(enabled: bool) -> &'static str {
    if enabled { "ON" } else { "OFF" }
}

/// Inputs for the CMake configure command beyond the backend set.
#[derive(Debug, Clone, Default)]
pub struct ConfigureOptions {
    pub full_profile: bool,
    /// Architectures reported by `nvidia-smi` (e.g. `["89"]`).
    pub cuda_architectures: Vec<String>,
    /// Architectures reported by `rocminfo` (e.g. `["gfx1100"]`).
    pub hip_architectures: Vec<String>,
    /// `(include_dir, library)` for OpenCL when found.
    pub opencl: Option<(PathBuf, PathBuf)>,
    /// Extra arguments from `QT_LLAMA_CMAKE_ARGS`, already split.
    pub extra_args: Vec<String>,
}

/// The `-D` cache arguments that configure llama.cpp as static libraries
/// with the selected backends (mirrors the archived configure command).
pub fn cmake_cache_args(backends: &BTreeSet<String>, options: &ConfigureOptions) -> Vec<String> {
    let has = |name: &str| backends.contains(name);
    let mut args: Vec<String> = [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DLLAMA_BUILD_COMMON=OFF",
        "-DLLAMA_BUILD_TESTS=OFF",
        "-DLLAMA_BUILD_TOOLS=OFF",
        "-DLLAMA_BUILD_EXAMPLES=OFF",
        "-DLLAMA_BUILD_SERVER=OFF",
        "-DLLAMA_BUILD_UI=OFF",
        "-DLLAMA_USE_PREBUILT_UI=OFF",
        "-DLLAMA_CURL=OFF",
        "-DLLAMA_ALL_WARNINGS=OFF",
        "-DGGML_ALL_WARNINGS=OFF",
        "-DGGML_NATIVE=OFF",
        "-DGGML_LLAMAFILE=OFF",
        "-DGGML_OPENMP=OFF",
        "-DGGML_BLAS=OFF",
        "-DGGML_RPC=OFF",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    for (flag, name) in [
        ("GGML_CUDA", "cuda"),
        ("GGML_HIP", "hip"),
        ("GGML_METAL", "metal"),
        ("GGML_VULKAN", "vulkan"),
        ("GGML_SYCL", "sycl"),
        ("GGML_OPENCL", "opencl"),
    ] {
        args.push(format!("-D{flag}={}", on_off(has(name))));
    }
    if has("metal") {
        // Embed the Metal shader library so the binary has no runtime file
        // dependency.
        args.push("-DGGML_METAL_EMBED_LIBRARY=ON".into());
    }
    if has("opencl")
        && let Some((include, library)) = &options.opencl
    {
        args.push(format!("-DOpenCL_INCLUDE_DIR={}", include.display()));
        args.push(format!("-DOpenCL_LIBRARY={}", library.display()));
        args.push("-DGGML_OPENCL_TARGET_VERSION=220".into());
        args.push("-DGGML_OPENCL_USE_ADRENO_KERNELS=OFF".into());
    }
    if has("cuda") {
        if options.full_profile {
            args.push(format!(
                "-DCMAKE_CUDA_ARCHITECTURES={FULL_CUDA_ARCHITECTURES}"
            ));
            args.push("-DGGML_CUDA_NCCL=OFF".into());
        } else if options.cuda_architectures.is_empty() {
            args.push("-DCMAKE_CUDA_ARCHITECTURES=native".into());
        } else {
            args.push(format!(
                "-DCMAKE_CUDA_ARCHITECTURES={}",
                options.cuda_architectures.join(";")
            ));
        }
    }
    if has("hip") {
        if options.full_profile {
            args.push("-DGGML_HIP_RCCL=OFF".into());
        } else if !options.hip_architectures.is_empty() {
            args.push(format!(
                "-DCMAKE_HIP_ARCHITECTURES={}",
                options.hip_architectures.join(";")
            ));
        }
    }
    args.extend(options.extra_args.iter().cloned());
    args
}

/// Split `QT_LLAMA_CMAKE_ARGS` like a POSIX shell (quotes and backslashes).
pub fn split_shell_words(input: &str) -> Result<Vec<String>, String> {
    let mut words = Vec::new();
    let mut current = String::new();
    let mut in_word = false;
    let mut chars = input.chars();
    while let Some(ch) = chars.next() {
        match ch {
            '\'' => {
                in_word = true;
                loop {
                    match chars.next() {
                        Some('\'') => break,
                        Some(c) => current.push(c),
                        None => return Err("unterminated single quote".into()),
                    }
                }
            }
            '"' => {
                in_word = true;
                loop {
                    match chars.next() {
                        Some('"') => break,
                        Some('\\') => match chars.next() {
                            Some(c @ ('"' | '\\' | '$' | '`')) => current.push(c),
                            Some(c) => {
                                current.push('\\');
                                current.push(c);
                            }
                            None => return Err("unterminated double quote".into()),
                        },
                        Some(c) => current.push(c),
                        None => return Err("unterminated double quote".into()),
                    }
                }
            }
            '\\' => {
                in_word = true;
                match chars.next() {
                    Some(c) => current.push(c),
                    None => return Err("trailing backslash".into()),
                }
            }
            c if c.is_whitespace() => {
                if in_word {
                    words.push(std::mem::take(&mut current));
                    in_word = false;
                }
            }
            c => {
                in_word = true;
                current.push(c);
            }
        }
    }
    if in_word {
        words.push(current);
    }
    Ok(words)
}

/// System libraries a backend needs at link time.
pub fn system_libraries(backends: &BTreeSet<String>) -> Vec<&'static str> {
    let mut libs = Vec::new();
    if backends.contains("vulkan") {
        libs.push("vulkan");
    }
    if backends.contains("opencl") {
        libs.push("OpenCL");
    }
    if backends.contains("cuda") {
        libs.extend(["cudart", "cublas", "cublasLt", "cuda"]);
    }
    if backends.contains("hip") {
        libs.extend(["amdhip64", "rocblas", "hipblas"]);
    }
    libs
}

/// Backends implied by the `libggml-<name>.a` archives a build produced.
pub fn built_backends<'a>(archive_names: impl IntoIterator<Item = &'a str>) -> BTreeSet<String> {
    archive_names
        .into_iter()
        .filter_map(|name| {
            name.strip_prefix("libggml-")
                .and_then(|n| n.strip_suffix(".a"))
                .or_else(|| {
                    name.strip_prefix("ggml-")
                        .and_then(|n| n.strip_suffix(".lib"))
                })
        })
        .filter(|name| !matches!(*name, "cpu" | "base" | "blas" | "rpc"))
        .map(str::to_string)
        .collect()
}

/// Order static archives for single-pass linkers: `llama`, `ggml`, GPU
/// backends, then `ggml-cpu` and `ggml-base` (as the archived script did).
pub fn order_archives(names: &[String]) -> Vec<String> {
    let mut ordered: Vec<String> = Vec::new();
    let push = |name: &str, ordered: &mut Vec<String>| {
        if names.iter().any(|n| n == name) && !ordered.iter().any(|n| n == name) {
            ordered.push(name.to_string());
        }
    };
    push("qt_llama_shim", &mut ordered);
    push("llama", &mut ordered);
    push("ggml", &mut ordered);
    let mut gpu: Vec<&String> = names
        .iter()
        .filter(|n| n.starts_with("ggml-") && *n != "ggml-cpu" && *n != "ggml-base")
        .collect();
    gpu.sort();
    for name in gpu {
        push(name, &mut ordered);
    }
    push("ggml-cpu", &mut ordered);
    push("ggml-base", &mut ordered);
    ordered
}

// ---------------------------------------------------------------------------
// Real system probing (build machine)
// ---------------------------------------------------------------------------

fn command_output(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).into_owned())
}

fn command_succeeds(program: &str, args: &[&str]) -> bool {
    command_output(program, args).is_some()
}

/// Locate an executable on `PATH`.
pub fn which(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .map(|dir| dir.join(name))
        .find(|candidate| candidate.is_file())
}

fn env_paths(name: &str) -> Vec<PathBuf> {
    std::env::var_os(name)
        .map(|v| {
            std::env::split_paths(&v)
                .filter(|p| !p.as_os_str().is_empty())
                .collect()
        })
        .unwrap_or_default()
}

fn include_directories() -> Vec<PathBuf> {
    let mut dirs = vec![
        PathBuf::from("/usr/include"),
        PathBuf::from("/usr/local/include"),
    ];
    for var in ["VULKAN_SDK", "Vulkan_ROOT"] {
        if let Some(sdk) = std::env::var_os(var) {
            let root = PathBuf::from(sdk);
            dirs.push(root.join("include"));
            dirs.push(root.join("Include"));
        }
    }
    dirs.extend(env_paths("CPATH"));
    dirs.extend(env_paths("CPLUS_INCLUDE_PATH"));
    dirs
}

fn include_exists(relative: &[&str]) -> bool {
    include_directories()
        .iter()
        .any(|dir| relative.iter().any(|rel| dir.join(rel).is_file()))
}

/// SPIR-V headers are required for the Vulkan backend.
pub fn spirv_header_present() -> bool {
    include_exists(&[
        "spirv/unified1/spirv.hpp",
        "spirv-headers/spirv.hpp",
        "spirv.hpp",
    ])
}

fn library_dirs() -> Vec<PathBuf> {
    let mut dirs: Vec<PathBuf> = [
        "/usr/lib",
        "/usr/lib64",
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu",
        "/usr/local/lib",
        "/usr/local/cuda/lib64",
        "/opt/rocm/lib",
        "/opt/rocm/lib64",
    ]
    .iter()
    .map(PathBuf::from)
    .collect();
    for var in ["VULKAN_SDK", "Vulkan_ROOT"] {
        if let Some(sdk) = std::env::var_os(var) {
            dirs.push(PathBuf::from(sdk).join("lib"));
        }
    }
    dirs.extend(env_paths("LD_LIBRARY_PATH"));
    dirs
}

/// Find a library whose file name starts with one of `prefixes`.
pub fn find_library(prefixes: &[&str]) -> Option<PathBuf> {
    for dir in library_dirs() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        let mut matches: Vec<PathBuf> = entries
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(|n| prefixes.iter().any(|prefix| n.starts_with(prefix)))
            })
            .collect();
        matches.sort();
        if let Some(found) = matches.into_iter().next() {
            return Some(found);
        }
    }
    None
}

fn dir_has_entries(path: &Path) -> bool {
    std::fs::read_dir(path).is_ok_and(|mut entries| entries.next().is_some())
}

/// Probes the machine running the build script.
pub struct SystemProbe {
    pub target_os: String,
}

impl SystemProbe {
    fn cuda_hardware(&self) -> bool {
        command_succeeds("nvidia-smi", &["-L"])
            || dir_has_entries(Path::new("/proc/driver/nvidia/gpus"))
    }

    fn hip_hardware(&self) -> bool {
        command_output("rocminfo", &[]).is_some_and(|o| o.contains("gfx"))
            || dir_has_entries(Path::new("/sys/class/kfd/kfd/topology/nodes"))
    }

    fn vulkan_hardware(&self) -> bool {
        if command_succeeds("vulkaninfo", &["--summary"]) {
            return true;
        }
        std::fs::read_dir("/dev/dri").is_ok_and(|entries| {
            entries.flatten().any(|e| {
                e.file_name()
                    .to_str()
                    .is_some_and(|n| n.starts_with("renderD"))
            })
        })
    }
}

impl Probe for SystemProbe {
    fn cuda(&self) -> bool {
        let toolkit = which("nvcc").is_some()
            || Path::new("/usr/local/cuda/bin/nvcc").exists()
            || std::env::var_os("CUDAToolkit_ROOT").is_some();
        toolkit && self.cuda_hardware()
    }

    fn hip(&self) -> bool {
        let toolkit = which("hipcc").is_some() || std::env::var_os("ROCM_PATH").is_some();
        toolkit && self.hip_hardware()
    }

    fn metal(&self) -> bool {
        self.target_os == "macos"
    }

    fn vulkan(&self) -> bool {
        include_exists(&["vulkan/vulkan.h"])
            && spirv_header_present()
            && (which("glslc").is_some() || std::env::var_os("Vulkan_GLSLC_EXECUTABLE").is_some())
            && find_library(&["libvulkan.so", "libvulkan.dylib", "vulkan-1.lib"]).is_some()
            && self.vulkan_hardware()
    }
}

/// CUDA compute capabilities of local GPUs (`8.9` → `89`).
pub fn cuda_architectures_present() -> Vec<String> {
    let output = command_output(
        "nvidia-smi",
        &["--query-gpu=compute_cap", "--format=csv,noheader"],
    )
    .unwrap_or_default();
    parse_cuda_architectures(&output)
}

/// Parse `nvidia-smi --query-gpu=compute_cap` output.
pub fn parse_cuda_architectures(output: &str) -> Vec<String> {
    let mut archs: Vec<u32> = output
        .lines()
        .map(|line| line.trim().replace('.', ""))
        .filter(|a| !a.is_empty() && a.chars().all(|c| c.is_ascii_digit()))
        .filter_map(|a| a.parse().ok())
        .collect();
    archs.sort_unstable();
    archs.dedup();
    archs.into_iter().map(|a| a.to_string()).collect()
}

/// AMD GPU targets reported by `rocminfo`.
pub fn hip_architectures_present() -> Vec<String> {
    parse_hip_architectures(&command_output("rocminfo", &[]).unwrap_or_default())
}

/// Parse `Name: gfxNNNN` lines from `rocminfo`.
pub fn parse_hip_architectures(output: &str) -> Vec<String> {
    let set: BTreeSet<String> = output
        .lines()
        .filter_map(|line| line.trim().strip_prefix("Name:"))
        .map(str::trim)
        .filter(|v| v.starts_with("gfx"))
        .map(str::to_string)
        .collect();
    set.into_iter().collect()
}

/// OpenCL header and library locations, when the header supports the
/// llama.cpp backend.
pub fn opencl_locations() -> Option<(PathBuf, PathBuf)> {
    let header = [
        "/usr/include/CL/cl.h",
        "/usr/local/include/CL/cl.h",
        "/opt/rocm/include/CL/cl.h",
    ]
    .iter()
    .map(PathBuf::from)
    .find(|p| p.is_file())?;
    let text = std::fs::read_to_string(&header).ok()?;
    if !(text.contains("cl_mem_properties") && text.contains("clCreateBufferWithProperties")) {
        return None;
    }
    let library = find_library(&["libOpenCL.so", "OpenCL.lib"])?;
    let include = header.parent()?.parent()?.to_path_buf();
    Some((include, library))
}
