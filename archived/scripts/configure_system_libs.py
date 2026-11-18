import ctypes.util
import glob
import os
import platform
import subprocess

Import("env")


def _ldconfig_lib(name):
    try:
        result = subprocess.run(
            ["ldconfig", "-p"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (FileNotFoundError, OSError):
        return None

    for line in result.stdout.splitlines():
        fields = line.strip().split()
        if fields and fields[0] == name:
            return fields[-1] if len(fields) >= 4 else name
    return None


def _find_soname(name):
    candidates = []
    found = ctypes.util.find_library(name)
    if found:
        candidates.append(found)

    arch_globs = [
        "/lib/x86_64-linux-gnu/lib{}.so.*".format(name),
        "/usr/lib/x86_64-linux-gnu/lib{}.so.*".format(name),
    ]
    if platform.machine() not in ("x86_64", "AMD64"):
        arch_globs = []

    candidates.extend(
        os.path.basename(path)
        for pattern in (
            *arch_globs,
            "/lib/*/lib{}.so.*".format(name),
            "/usr/lib/*/lib{}.so.*".format(name),
            "/usr/local/lib/lib{}.so.*".format(name),
        )
        for path in glob.glob(pattern)
    )

    for candidate in candidates:
        if candidate.startswith("lib{}.so".format(name)):
            return candidate
    return None


def _link_lib(name, generated_libdir):
    generated_shared = os.path.join(generated_libdir, "lib{}.so".format(name))
    if os.path.exists(generated_shared):
        return generated_shared

    unversioned = "lib{}.so".format(name)
    unversioned_path = _ldconfig_lib(unversioned)
    if unversioned_path:
        return name

    soname = _find_soname(name)
    if soname:
        path_globs = [
            "/lib/x86_64-linux-gnu/{}".format(soname),
            "/usr/lib/x86_64-linux-gnu/{}".format(soname),
        ]
        if platform.machine() not in ("x86_64", "AMD64"):
            path_globs = []
        path_globs.extend(
            [
                "/lib/*/{}".format(soname),
                "/usr/lib/*/{}".format(soname),
                "/usr/local/lib/{}".format(soname),
            ]
        )
        for pattern in path_globs:
            matches = glob.glob(pattern)
            if matches:
                return env.File(matches[0])

    return name


build_dir = env.subst("$BUILD_DIR")
generated_libdir = os.path.join(build_dir, "generated", "sysroot", "usr", "lib", "x86_64-linux-gnu")
if os.path.isdir(generated_libdir):
    env.Append(LIBPATH=[generated_libdir])

unistring_lib = _link_lib("unistring", generated_libdir)
if isinstance(unistring_lib, str) and os.path.sep in unistring_lib:
    unistring_lib = env.File(unistring_lib)
ssl_lib = _link_lib("ssl", generated_libdir)
if isinstance(ssl_lib, str) and os.path.sep in ssl_lib:
    ssl_lib = env.File(ssl_lib)

crypto_lib = _link_lib("crypto", generated_libdir)
if isinstance(crypto_lib, str) and os.path.sep in crypto_lib:
    crypto_lib = env.File(crypto_lib)

env.Append(LIBS=[unistring_lib, ssl_lib, crypto_lib])
