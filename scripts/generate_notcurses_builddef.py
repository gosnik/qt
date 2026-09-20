import json
from pathlib import Path
import re
import shutil
import subprocess

Import("env")

PROJECT_DIR = Path(env["PROJECT_DIR"])
BUILD_DIR = Path(env.subst("$BUILD_DIR"))
ENV_NAME = env["PIOENV"]

candidate_sources = [
    PROJECT_DIR / ".pio" / "libdeps" / ENV_NAME / "notcurses" / "tools" / "builddef.h.in",
    PROJECT_DIR / "lib" / "notcurses" / "tools" / "builddef.h.in",
]

src = next((path for path in candidate_sources if path.exists()), None)
if src is None:
    searched = ", ".join(str(path) for path in candidate_sources)
    raise FileNotFoundError(f"notcurses builddef.h.in not found; searched {searched}")

out_dir = BUILD_DIR / "generated"
builddef_out = out_dir / "builddef.h"
version_out = out_dir / "version.h"
sysroot_dir = out_dir / "sysroot"
out_dir.mkdir(parents=True, exist_ok=True)


def ensure_libunistring_headers():
    if Path("/usr/include/unigbrk.h").exists():
        return
    if (sysroot_dir / "usr" / "include" / "unigbrk.h").exists():
        return
    if shutil.which("apt-get") is None or shutil.which("dpkg-deb") is None:
        raise FileNotFoundError(
            "notcurses requires libunistring development headers; install "
            "libunistring-dev or provide unigbrk.h on the compiler include path"
        )

    cache_dir = out_dir / "apt"
    cache_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["apt-get", "download", "libunistring-dev"],
        cwd=cache_dir,
        check=True,
    )
    packages = sorted(cache_dir.glob("libunistring-dev_*.deb"))
    if not packages:
        raise FileNotFoundError("apt-get did not download libunistring-dev")
    subprocess.run(["dpkg-deb", "-x", str(packages[-1]), str(sysroot_dir)], check=True)


ensure_libunistring_headers()

defines = {
    "DFSG_BUILD": False,
    "USE_ASAN": False,
    "USE_DEFLATE": False,
    "USE_GPM": False,
    "USE_QRCODEGEN": False,
    "USE_FFMPEG": False,
    "USE_OIIO": False,
}

vars = {
    "CMAKE_INSTALL_FULL_DATADIR": "/usr/local/share",
}

text = src.read_text(encoding="utf-8")


def replace_cmakedefine(match):
    name = match.group(1)
    if defines.get(name, False):
        return f"#define {name}"
    return f"/* #undef {name} */"


text = re.sub(
    r"^#cmakedefine\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
    replace_cmakedefine,
    text,
    flags=re.MULTILINE,
)

for key, value in vars.items():
    text = text.replace(f"@{key}@", value)

builddef_out.write_text(text, encoding="utf-8")

version_template = src.parent / "version.h.in"
version_text = version_template.read_text(encoding="utf-8")
version_vars = {
    "notcurses_VERSION_MAJOR": "3",
    "notcurses_VERSION_MINOR": "0",
    "notcurses_VERSION_PATCH": "17",
    "notcurses_VERSION_TWEAK": "0",
}
for key, value in version_vars.items():
    version_text = version_text.replace(f"@{key}@", value)
version_out.write_text(version_text, encoding="utf-8")

notcurses_root = src.parents[1]
library_json = notcurses_root / "library.json"
library_json.write_text(
    json.dumps(
        {
            "name": "notcurses",
            "version": "0.0.0+20260513120043",
            "build": {
                "flags": ["-w"],
                "srcFilter": [
                    "-<*>",
                    "+<compat/*.c>",
                    "+<lib/*.c>",
                    "+<media/none.c>",
                ]
            },
        },
        indent=2,
    )
    + "\n",
    encoding="utf-8",
)

print(f"Generated {builddef_out}")
print(f"Generated {version_out}")
print(f"Configured {library_json}")
