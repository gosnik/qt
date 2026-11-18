//! Raw FFI bindings to the pinned PrismML llama.cpp fork.
//!
//! The build script compiles llama.cpp (static libraries, GPU backends
//! auto-detected like the archived `scripts/build_llama_cpp.py`) and generates
//! bindings for `llama.h`, `ggml-backend.h` and a small C shim over the
//! C++-only memory helpers in `llama-ext.h`.
//!
//! When the llama.cpp source tree is missing the crate still builds, without
//! bindings: [`AVAILABLE`] is `false` and [`UNAVAILABLE_REASON`] says why.
//! Dependent crates read the `DEP_LLAMA_AVAILABLE` build metadata to gate
//! their native code.

#![allow(
    non_upper_case_globals,
    non_camel_case_types,
    non_snake_case,
    dead_code,
    unnecessary_transmutes,
    clippy::all
)]

/// Whether llama.cpp was compiled and linked into this build.
pub const AVAILABLE: bool = cfg!(llama_available);

/// Comma-separated GPU backends compiled in (empty for CPU-only builds).
pub const BACKENDS: &str = env!("QT_LLAMA_BACKENDS");

/// Why llama.cpp is unavailable (empty when [`AVAILABLE`]).
pub const UNAVAILABLE_REASON: &str = env!("QT_LLAMA_UNAVAILABLE_REASON");

#[cfg(llama_available)]
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
#[path = "../build/backends.rs"]
mod backends;

#[cfg(test)]
mod tests {
    use super::backends::*;
    use std::collections::BTreeSet;

    struct FakeProbe {
        cuda: bool,
        hip: bool,
        metal: bool,
        vulkan: bool,
    }

    impl Probe for FakeProbe {
        fn cuda(&self) -> bool {
            self.cuda
        }
        fn hip(&self) -> bool {
            self.hip
        }
        fn metal(&self) -> bool {
            self.metal
        }
        fn vulkan(&self) -> bool {
            self.vulkan
        }
    }

    fn set(items: &[&str]) -> BTreeSet<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parses_backend_requests_like_the_archived_script() {
        assert_eq!(parse_request(""), BackendRequest::Auto);
        assert_eq!(parse_request(" AUTO "), BackendRequest::Auto);
        for cpu in ["0", "false", "no", "off", "cpu", "none", "OFF"] {
            assert_eq!(parse_request(cpu), BackendRequest::Cpu, "{cpu}");
        }
        assert_eq!(
            parse_request("all"),
            BackendRequest::Explicit(set(&["cuda", "hip", "metal", "vulkan", "opencl", "sycl"]))
        );
        assert_eq!(
            parse_request("ggml_cuda; Vulkan,,hip"),
            BackendRequest::Explicit(set(&["cuda", "vulkan", "hip"]))
        );
    }

    #[test]
    fn auto_selects_only_present_backends() {
        let probe = FakeProbe {
            cuda: true,
            hip: false,
            metal: false,
            vulkan: true,
        };
        assert_eq!(
            select_backends(&BackendRequest::Auto, &probe),
            set(&["cuda", "vulkan"])
        );
        assert!(select_backends(&BackendRequest::Cpu, &probe).is_empty());
        let none = FakeProbe {
            cuda: false,
            hip: false,
            metal: false,
            vulkan: false,
        };
        assert!(select_backends(&BackendRequest::Auto, &none).is_empty());
        assert_eq!(default_request(true), "cuda,vulkan,hip");
        assert_eq!(default_request(false), "auto");
    }

    #[test]
    fn cache_args_toggle_backends_and_architectures() {
        let args = cmake_cache_args(&set(&["vulkan"]), &ConfigureOptions::default());
        assert!(args.contains(&"-DGGML_VULKAN=ON".to_string()));
        assert!(args.contains(&"-DGGML_CUDA=OFF".to_string()));
        assert!(args.contains(&"-DBUILD_SHARED_LIBS=OFF".to_string()));
        assert!(args.contains(&"-DGGML_OPENMP=OFF".to_string()));

        let local = ConfigureOptions {
            cuda_architectures: vec!["86".into(), "89".into()],
            extra_args: vec!["-DFOO=1".into()],
            ..Default::default()
        };
        let args = cmake_cache_args(&set(&["cuda"]), &local);
        assert!(args.contains(&"-DCMAKE_CUDA_ARCHITECTURES=86;89".to_string()));
        assert_eq!(args.last().unwrap(), "-DFOO=1");

        let native = cmake_cache_args(&set(&["cuda"]), &ConfigureOptions::default());
        assert!(native.contains(&"-DCMAKE_CUDA_ARCHITECTURES=native".to_string()));

        let full = ConfigureOptions {
            full_profile: true,
            ..Default::default()
        };
        let args = cmake_cache_args(&set(&["cuda", "hip"]), &full);
        assert!(args.contains(&"-DCMAKE_CUDA_ARCHITECTURES=75;80;86;89;90".to_string()));
        assert!(args.contains(&"-DGGML_CUDA_NCCL=OFF".to_string()));
        assert!(args.contains(&"-DGGML_HIP_RCCL=OFF".to_string()));
    }

    #[test]
    fn splits_shell_words() {
        assert_eq!(
            split_shell_words(r#"-DA=1 "-DB=two words" '-DC=$x' -DD=e\ f"#).unwrap(),
            vec!["-DA=1", "-DB=two words", "-DC=$x", "-DD=e f"]
        );
        assert!(split_shell_words("'open").is_err());
        assert!(split_shell_words("").unwrap().is_empty());
    }

    #[test]
    fn parses_gpu_architectures() {
        assert_eq!(
            parse_cuda_architectures("8.9\n12.0\n8.9\nbad\n"),
            vec!["89", "120"]
        );
        assert_eq!(
            parse_hip_architectures(
                "  Name: gfx1100\n Name: AMD Ryzen\n Name: gfx1100\n Name: gfx90a"
            ),
            vec!["gfx1100", "gfx90a"]
        );
    }

    #[test]
    fn orders_archives_for_single_pass_linkers() {
        let names: Vec<String> = [
            "ggml-base",
            "ggml-cpu",
            "ggml-vulkan",
            "llama",
            "ggml",
            "qt_llama_shim",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        assert_eq!(
            order_archives(&names),
            vec![
                "qt_llama_shim",
                "llama",
                "ggml",
                "ggml-vulkan",
                "ggml-cpu",
                "ggml-base"
            ]
        );
        assert_eq!(
            built_backends([
                "libggml-cpu.a",
                "libggml-vulkan.a",
                "libggml-base.a",
                "libllama.a"
            ]),
            set(&["vulkan"])
        );
        assert_eq!(
            system_libraries(&set(&["vulkan", "cuda"])),
            vec!["vulkan", "cudart", "cublas", "cublasLt", "cuda"]
        );
    }
}
