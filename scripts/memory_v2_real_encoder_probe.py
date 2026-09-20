#!/usr/bin/env python3
"""Build and run the native real-encoder probe against the pinned PIO objects."""
from pathlib import Path
import subprocess
import sys
import argparse
import json
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", nargs="?", default="models/nomic-embed-text-v1.5.Q8_0.gguf")
    parser.add_argument("gpu_layers", nargs="?", default="0")
    parser.add_argument("scale_records", nargs="?", default="100")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--skip-build", action="store_true",
                        help="use existing synchronized native objects")
    args = parser.parse_args()
    if not args.skip_build:
        subprocess.run(["pio", "run", "-e", "native"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-real-encoder-") as temp:
        out = Path(temp) / "probe"
        obj_file = Path(temp) / "objects.rsp"
        src = ROOT / 'scripts/memory_v2_real_encoder_probe.cpp'
        native = ROOT / '.pio/build/native'
        objects = sorted(native.glob('src/**/*.o'))
        objects = [p for p in objects if p.name != 'main.o']
        objects += sorted(native.glob('lib*/*.a'))
        objects += sorted(native.glob('llama.cpp/**/*.a'))
        obj_file.write_text('\n'.join(str(p) for p in objects) + '\n')
        inc = ['-Iinclude', '-Isrc/index/tree_sitter_runtime',
               '-I.pio/build/native/generated/sysroot/usr/include', '-Ilib/boost_1_86_0',
               '-I.pio/libdeps/native/json/include', '-Ilib/llama.cpp/include',
               '-Ilib/llama.cpp/ggml/include', '-Ilib/llama.cpp/ggml/src', '-Ilib/llama.cpp/src']
        link = ['g++', '-std=c++17', *inc, '-pthread', str(src),
                '-Wl,--start-group', '@' + str(obj_file), '-Wl,--end-group',
                '-L.pio/build/native/generated/sysroot/usr/lib/x86_64-linux-gnu',
                '-lunistring', '-lsqlite3', '-lvulkan', '-lssl', '-lcrypto', '-lz', '-lzstd',
                '-ltinfo', '-ldl', '-lm', '-lrt', '-o', str(out)]
        subprocess.run(link, cwd=ROOT, check=True)
        result = subprocess.run([str(out), args.model, args.gpu_layers, args.scale_records], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='')
    if result.returncode != 0:
        return result.returncode
    if args.report:
        report_line = next((line for line in result.stdout.splitlines() if line.startswith('four_way_report=')), None)
        scale_line = next((line for line in result.stdout.splitlines() if line.startswith('scale_report=')), None)
        governed_line = next((line for line in result.stdout.splitlines() if line.startswith('governed_policy_report=')), None)
        if not report_line or not scale_line or not governed_line:
            raise RuntimeError("probe did not emit all required evaluation reports")
        report = json.loads(report_line.split('=', 1)[1])
        report["scale_workload"] = json.loads(scale_line.split('=', 1)[1])
        report["governed_policy"] = json.loads(governed_line.split('=', 1)[1])
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
