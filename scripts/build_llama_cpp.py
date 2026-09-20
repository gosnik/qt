import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

Import("env")

PROJECT_DIR = Path(env["PROJECT_DIR"])
BUILD_DIR = Path(env.subst("$BUILD_DIR"))
PIOENV = env["PIOENV"]

LLAMA_SOURCE_DIR = PROJECT_DIR / "lib" / "llama.cpp"
LLAMA_BUILD_DIR = BUILD_DIR / "llama.cpp"


def command_succeeds(command):
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except (FileNotFoundError, OSError):
        return False
    return result.returncode == 0


def command_output(command):
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except (FileNotFoundError, OSError):
        return ""
    if result.returncode != 0:
        return ""
    return result.stdout


def first_existing(paths):
    for path in paths:
        candidate = Path(path)
        if candidate.exists():
            return candidate
    return None


def include_directories():
    directories = [Path("/usr/include"), Path("/usr/local/include")]
    for variable in ("VULKAN_SDK", "Vulkan_ROOT"):
        sdk = os.environ.get(variable)
        if sdk:
            sdk_root = Path(sdk)
            directories.extend([sdk_root / "include", sdk_root / "Include"])
    for variable in ("CPATH", "CPLUS_INCLUDE_PATH"):
        directories.extend(
            Path(path)
            for path in os.environ.get(variable, "").split(os.pathsep)
            if path
        )
    return directories


def include_exists(relative_paths):
    return any(
        (directory / relative_path).is_file()
        for directory in include_directories()
        for relative_path in relative_paths
    )


def spirv_header_present():
    return include_exists([
        "spirv/unified1/spirv.hpp",
        "spirv-headers/spirv.hpp",
        "spirv.hpp",
    ])


def library_exists(names):
    search_dirs = [
        Path("/usr/lib"),
        Path("/usr/lib64"),
        Path("/usr/lib/x86_64-linux-gnu"),
        Path("/usr/local/lib"),
        Path("/usr/local/cuda/lib64"),
        Path("/opt/rocm/lib"),
        Path("/opt/rocm/lib64"),
        Path("/opt/rocm-6.2.3/lib"),
        Path("/opt/rocm-6.2.3/lib64"),
    ]
    env_paths = os.environ.get("LD_LIBRARY_PATH", "")
    search_dirs.extend(Path(path) for path in env_paths.split(":") if path)
    for directory in search_dirs:
        for name in names:
            if any(directory.glob(name)):
                return True
    return False


def find_library_path(names):
    search_dirs = [
        Path("/usr/lib"),
        Path("/usr/lib64"),
        Path("/usr/lib/x86_64-linux-gnu"),
        Path("/usr/local/lib"),
        Path("/usr/local/cuda/lib64"),
        Path("/opt/rocm/lib"),
        Path("/opt/rocm/lib64"),
        Path("/opt/rocm-6.2.3/lib"),
        Path("/opt/rocm-6.2.3/lib64"),
    ]
    env_paths = os.environ.get("LD_LIBRARY_PATH", "")
    search_dirs.extend(Path(path) for path in env_paths.split(":") if path)
    for directory in search_dirs:
        for name in names:
            matches = sorted(directory.glob(name))
            if matches:
                return matches[0]
    return None


def opencl_locations():
    header = first_existing([
        "/usr/include/CL/cl.h",
        "/usr/local/include/CL/cl.h",
        "/opt/rocm/include/CL/cl.h",
        "/opt/rocm-6.2.3/include/CL/cl.h",
    ])
    library = find_library_path(["libOpenCL.so", "libOpenCL.so.*", "OpenCL.lib"])
    return header, library


def opencl_header_supports_llama_backend(header):
    if header is None:
        return False
    try:
        text = header.read_text(errors="ignore")
    except OSError:
        return False
    return "cl_mem_properties" in text and "clCreateBufferWithProperties" in text


def cuda_hardware_present():
    if command_succeeds(["nvidia-smi", "-L"]):
        return True
    return any(Path("/proc/driver/nvidia/gpus").glob("*"))


def cuda_architectures_present():
    output = command_output([
        "nvidia-smi",
        "--query-gpu=compute_cap",
        "--format=csv,noheader",
    ])
    architectures = []
    for line in output.splitlines():
        arch = line.strip().replace(".", "")
        if arch.isdigit():
            architectures.append(arch)
    return sorted(set(architectures), key=int)


def hip_hardware_present():
    output = command_output(["rocminfo"])
    if "gfx" in output:
        return True
    return any(Path("/sys/class/kfd/kfd/topology/nodes").glob("*/properties"))


def hip_architectures_present():
    output = command_output(["rocminfo"])
    architectures = set()
    for line in output.splitlines():
        text = line.strip()
        if text.startswith("Name:"):
            value = text.split(":", 1)[1].strip()
            if value.startswith("gfx"):
                architectures.add(value)
    return sorted(architectures)


def vulkan_hardware_present():
    if command_succeeds(["vulkaninfo", "--summary"]):
        return True
    return any(Path("/dev/dri").glob("renderD*"))


def full_gpu_environment():
    return PIOENV == "native_full"


def default_requested_backends():
    if full_gpu_environment():
        return "cuda,vulkan,hip"
    return "auto"


def detect_gpu_backends():
    requested = os.environ.get(
        "QT_LLAMA_GPU_BACKENDS", default_requested_backends()
    ).strip().lower()
    if requested in {"", "auto"}:
        backends = set()
        if (shutil.which("nvcc") or Path("/usr/local/cuda").exists() or os.environ.get("CUDAToolkit_ROOT")) and cuda_hardware_present():
            backends.add("cuda")
        if (shutil.which("hipcc") or os.environ.get("ROCM_PATH")) and hip_hardware_present():
            backends.add("hip")
        if sys.platform == "darwin":
            backends.add("metal")
        if include_exists(["vulkan/vulkan.h"]) and spirv_header_present() and (
            shutil.which("glslc") or os.environ.get("Vulkan_GLSLC_EXECUTABLE")
        ) and library_exists(["libvulkan.so", "libvulkan.so.*", "libvulkan.dylib"]) and vulkan_hardware_present():
            backends.add("vulkan")
        return backends
    if requested in {"0", "false", "no", "off", "cpu", "none"}:
        return set()
    if requested == "all":
        return {"cuda", "hip", "metal", "vulkan", "opencl", "sycl"}
    return {
        token.strip().replace("ggml_", "").replace("-", "_")
        for token in requested.replace(";", ",").split(",")
        if token.strip()
    }


def on_off(enabled):
    return "ON" if enabled else "OFF"


def cmake_cache_path(name):
    cache = LLAMA_BUILD_DIR / "CMakeCache.txt"
    try:
        lines = cache.read_text(errors="ignore").splitlines()
    except OSError:
        return None
    prefix = name + ":"
    for line in lines:
        if not line.startswith(prefix) or "=" not in line:
            continue
        value = line.split("=", 1)[1].strip()
        if value and not value.endswith("-NOTFOUND"):
            return Path(value)
    return None


def append_gpu_link_flags(backends):
    linkflags = []
    if "opencl" in backends:
        for directory in (Path("/opt/rocm/lib"), Path("/opt/rocm-6.2.3/lib")):
            if (directory / "libOpenCL.so").exists():
                env.Append(LIBPATH=[str(directory)])
                break
    if "cuda" in backends:
        cudart = cmake_cache_path("CUDA_CUDART")
        directories = []
        if cudart:
            directories.append(cudart.parent)
        directories.extend(
            [
                Path("/usr/local/cuda/lib64"),
                Path("/usr/lib/x86_64-linux-gnu"),
            ]
        )
        for directory in dict.fromkeys(directories):
            if directory.exists():
                env.Append(LIBPATH=[str(directory)])
    if "hip" in backends:
        for directory in (Path("/opt/rocm/lib"), Path("/opt/rocm-6.2.3/lib")):
            if directory.exists():
                env.Append(LIBPATH=[str(directory)])
    if "sycl" in backends:
        linkflags.append("-fsycl")
    if sys.platform == "darwin" and "metal" in backends:
        linkflags.extend([
            "-framework", "Foundation",
            "-framework", "Metal",
            "-framework", "MetalKit",
        ])
    return linkflags


def gpu_system_libraries(backends):
    libs = []
    if "vulkan" in backends:
        libs.append("vulkan")
    if "opencl" in backends:
        libs.append("OpenCL")
    if "cuda" in backends:
        libs.extend(["cudart", "cublas", "cublasLt", "cuda"])
    if "hip" in backends:
        libs.extend(["amdhip64", "rocblas", "hipblas"])
    return libs


def require_tool(name):
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(
            f"llama.cpp integration requires `{name}` for the native build"
        )
    return path


if not (LLAMA_SOURCE_DIR / "include" / "llama.h").exists():
    raise FileNotFoundError(
        "llama.cpp submodule is missing; run `git submodule update --init lib/llama.cpp`"
    )

cmake = require_tool("cmake")
generator = os.environ.get("QT_LLAMA_CMAKE_GENERATOR")
if not generator and shutil.which("ninja"):
    generator = "Ninja"

gpu_backends = detect_gpu_backends()
if "vulkan" in gpu_backends and not spirv_header_present():
    raise RuntimeError(
        "llama.cpp Vulkan support requires SPIR-V Headers (`spirv.hpp`); "
        "install the `spirv-headers` system package or provide a Vulkan SDK "
        "through VULKAN_SDK, Vulkan_ROOT, CPATH, or CPLUS_INCLUDE_PATH"
    )

configure_cmd = [
    cmake,
    "-S",
    str(LLAMA_SOURCE_DIR),
    "-B",
    str(LLAMA_BUILD_DIR),
    "-DCMAKE_BUILD_TYPE=Release",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DLLAMA_BUILD_COMMON=OFF",
    "-DLLAMA_BUILD_TESTS=OFF",
    "-DLLAMA_BUILD_TOOLS=OFF",
    "-DLLAMA_BUILD_EXAMPLES=OFF",
    "-DLLAMA_BUILD_SERVER=OFF",
    "-DLLAMA_BUILD_UI=OFF",
    "-DLLAMA_USE_PREBUILT_UI=OFF",
    "-DLLAMA_ALL_WARNINGS=OFF",
    "-DGGML_ALL_WARNINGS=OFF",
    "-DGGML_NATIVE=OFF",
    "-DGGML_LLAMAFILE=OFF",
    "-DGGML_OPENMP=OFF",
    "-DGGML_BLAS=OFF",
    f"-DGGML_CUDA={on_off('cuda' in gpu_backends)}",
    f"-DGGML_HIP={on_off('hip' in gpu_backends)}",
    f"-DGGML_METAL={on_off('metal' in gpu_backends)}",
    f"-DGGML_VULKAN={on_off('vulkan' in gpu_backends)}",
    f"-DGGML_SYCL={on_off('sycl' in gpu_backends)}",
    "-DGGML_RPC=OFF",
    f"-DGGML_OPENCL={on_off('opencl' in gpu_backends)}",
]
if generator:
    configure_cmd.extend(["-G", generator])

if "opencl" in gpu_backends:
    opencl_header, opencl_library = opencl_locations()
    if opencl_header and opencl_library:
        configure_cmd.extend([
            f"-DOpenCL_INCLUDE_DIR={opencl_header.parent.parent}",
            f"-DOpenCL_LIBRARY={opencl_library}",
            "-DGGML_OPENCL_TARGET_VERSION=220",
            "-DGGML_OPENCL_USE_ADRENO_KERNELS=OFF",
        ])

if full_gpu_environment() and "cuda" in gpu_backends:
    configure_cmd.append("-DCMAKE_CUDA_ARCHITECTURES=75;80;86;89;90")
elif "cuda" in gpu_backends:
    cuda_architectures = cuda_architectures_present()
    if cuda_architectures:
        configure_cmd.append(
            "-DCMAKE_CUDA_ARCHITECTURES=" + ";".join(cuda_architectures)
        )
    else:
        configure_cmd.append("-DCMAKE_CUDA_ARCHITECTURES=native")
if full_gpu_environment() and "cuda" in gpu_backends:
    configure_cmd.append("-DGGML_CUDA_NCCL=OFF")
if full_gpu_environment() and "hip" in gpu_backends:
    configure_cmd.append("-DGGML_HIP_RCCL=OFF")
elif "hip" in gpu_backends:
    hip_architectures = hip_architectures_present()
    if hip_architectures:
        configure_cmd.append(
            "-DCMAKE_HIP_ARCHITECTURES=" + ";".join(hip_architectures)
        )

extra_cmake_args = os.environ.get("QT_LLAMA_CMAKE_ARGS", "")
if extra_cmake_args:
    configure_cmd.extend(shlex.split(extra_cmake_args))

build_cmd = [
    cmake,
    "--build",
    str(LLAMA_BUILD_DIR),
    "--target",
    "llama",
]

jobs = os.environ.get("QT_LLAMA_BUILD_JOBS")
if jobs:
    build_cmd.extend(["--parallel", jobs])

vulkan_prebuild_cmd = [
    cmake,
    "--build",
    str(LLAMA_BUILD_DIR),
    "--target",
    "ggml-vulkan",
    "--parallel",
    "1",
]

print(f"Configuring llama.cpp static library for {PIOENV}")
print(
    "llama.cpp GPU backends: "
    + (", ".join(sorted(gpu_backends)) if gpu_backends else "none")
)
subprocess.run(configure_cmd, cwd=PROJECT_DIR, check=True)
if "vulkan" in gpu_backends:
    print("Prebuilding llama.cpp Vulkan shader sources serially")
    subprocess.run(vulkan_prebuild_cmd, cwd=PROJECT_DIR, check=True)
print(f"Building llama.cpp static library for {PIOENV}")
subprocess.run(build_cmd, cwd=PROJECT_DIR, check=True)

required_llama_libraries = [
    LLAMA_BUILD_DIR / "src" / "libllama.a",
    LLAMA_BUILD_DIR / "ggml" / "src" / "libggml.a",
    LLAMA_BUILD_DIR / "ggml" / "src" / "libggml-cpu.a",
    LLAMA_BUILD_DIR / "ggml" / "src" / "libggml-base.a",
]
missing = [str(path) for path in required_llama_libraries if not path.exists()]
if missing:
    raise FileNotFoundError(
        "llama.cpp build did not produce expected static libraries: "
        + ", ".join(missing)
    )

llama_libraries = []
seen_libraries = set()
for path in [
    LLAMA_BUILD_DIR / "src" / "libllama.a",
    LLAMA_BUILD_DIR / "ggml" / "src" / "libggml.a",
]:
    llama_libraries.append(path)
    seen_libraries.add(path.resolve())
for path in sorted((LLAMA_BUILD_DIR / "ggml" / "src").rglob("libggml-*.a")):
    resolved = path.resolve()
    if resolved not in seen_libraries and path.name not in {
        "libggml-base.a",
        "libggml-cpu.a",
    }:
        llama_libraries.append(path)
        seen_libraries.add(resolved)
for path in [
    LLAMA_BUILD_DIR / "ggml" / "src" / "libggml-cpu.a",
    LLAMA_BUILD_DIR / "ggml" / "src" / "libggml-base.a",
]:
    resolved = path.resolve()
    if resolved not in seen_libraries:
        llama_libraries.append(path)
        seen_libraries.add(resolved)

llama_libpaths = sorted({str(path.parent) for path in llama_libraries})
llama_libs = [path.name[3:-2] for path in llama_libraries]
built_gpu_backends = {
    path.name.removeprefix("libggml-").removesuffix(".a")
    for path in llama_libraries
    if path.name.startswith("libggml-")
}
built_gpu_backends.difference_update({"cpu", "base", "blas", "rpc"})
gpu_linkflags = append_gpu_link_flags(gpu_backends | built_gpu_backends)
gpu_libs = gpu_system_libraries(gpu_backends | built_gpu_backends)

env.Append(
    CPPPATH=[
        str(LLAMA_SOURCE_DIR / "include"),
        str(LLAMA_SOURCE_DIR / "ggml" / "include"),
        str(LLAMA_SOURCE_DIR / "ggml" / "src"),
        str(LLAMA_SOURCE_DIR / "src"),
    ],
    LIBPATH=[
        *llama_libpaths,
    ],
    LINKFLAGS=["-pthread", "-ldl", *gpu_linkflags],
    LIBS=[*llama_libs, *gpu_libs],
)
