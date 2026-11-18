# Packaging

Everything needed to turn a release build of `qt` (GUI, crate `qt-app`) and `qt-cli`
into installable packages. Both binaries always ship together: subagent workers, the
memory pre-handler and Dreaming start the `qt-cli` that sits next to the running `qt`.

| Platform | Artifacts (in `target/dist/`) | Tool |
| --- | --- | --- |
| Linux | `qt-<ver>-<triple>.tar.gz`, `.AppImage`, `.deb` | `package.sh` (tarball), cargo-packager (`appimage`, `deb`) |
| macOS | `qt-<ver>-<triple>.app.zip` (unsigned `qt.app`) | cargo-packager (`app`), `ditto` |
| Windows | `qt-<ver>-<triple>.zip`, `.msi` | `package.sh` (zip), cargo-packager (`wix`) |

Every run also writes `SHA256SUMS-<triple>.txt`.

## Layout

| Path | Purpose |
| --- | --- |
| `scripts/package.sh` | Builds `-p qt-app -p qt-cli` in release mode and produces the artifacts above (bash; Git Bash on Windows). `--help` lists the options. |
| `icons/generate.py` | Draws the icon (original artwork: a "q" ring and an amber block cursor) and writes `icons/qt.svg`, `icons/png/qt-{16..1024}.png` and `icons/qt.ico`. Needs Pillow. |
| `linux/qt.desktop` | Desktop entry shipped in the tarball (`Icon=qt`, `StartupWMClass=qt`). |
| `linux/qt.desktop.hbs` | Same entry as a cargo-packager template, used for the AppImage and `.deb`. |
| `linux/install.sh` | Installs an unpacked tarball into `PREFIX` (default `~/.local`); `--uninstall` removes it. |

The packager configuration lives in `crates/qt-app/Cargo.toml` under
`[package.metadata.packager]` (product name `qt`, identifier `dev.qt-agent.qt`, both
binaries, icons, desktop template, macOS minimum 10.15.7, WiX `en-US`).

The app id is `qt` on every platform: the GPUI window sets `app_id = "qt"`, the desktop
file is `qt.desktop`, and the icon is installed as `hicolor/*/apps/qt.png` plus
`hicolor/scalable/apps/qt.svg`, so Wayland and X11 docks match the window to its entry.

## Local build

Prerequisites: the toolchain and system packages from the top-level README's "Build"
section, the llama.cpp submodule (`git submodule update --init third_party/llama.cpp`),
and cargo-packager:

```sh
cargo install cargo-packager --version 0.11.8 --locked
```

Then, from the repository root:

```sh
QT_LLAMA_GPU_BACKENDS=cpu packaging/scripts/package.sh            # host defaults
packaging/scripts/package.sh --skip-build --formats deb           # reuse target/release
packaging/scripts/package.sh --formats none                       # tarball / zip only
CARGO_PACKAGER=/path/to/cargo-packager packaging/scripts/package.sh
```

`QT_LLAMA_GPU_BACKENDS` (and the other `QT_LLAMA_*` variables) are forwarded to the
llama.cpp build. Pick a backend the target machines have: `cpu` is portable, `vulkan`
needs `libvulkan-dev`, `spirv-headers` and `glslc` at build time and a Vulkan driver at
run time; macOS `auto` builds Metal. `GGML_NATIVE` is always off, so CPU code is not tied
to the build machine.

Notes per format:

- **AppImage:** cargo-packager downloads `linuxdeploy`, its AppImage plugin and `AppRun`
  from GitHub on first use (cached in the user cache directory, e.g.
  `~/.cache/.cargo-packager/`) and runs them. Set `APPIMAGE_EXTRACT_AND_RUN=1` where
  FUSE is unavailable (containers, CI). linuxdeploy bundles the shared libraries `qt`
  links (libxcb, libxkbcommon, libxkbcommon-x11, libstdc++); the Vulkan/EGL loader, the
  Wayland client library and GPU drivers are loaded from the host at run time. Build on the oldest distribution
  you want to support (the release workflow uses Ubuntu 22.04).
- **.deb:** pure cargo-packager, no downloads. Installs `/usr/bin/qt`, `/usr/bin/qt-cli`,
  `/usr/share/applications/qt.desktop` and the hicolor icons, and depends on the
  libraries above (`libvulkan1 | libegl1`, `libwayland-client0`, ...).
- **macOS .app:** unsigned and not notarized. After unzipping, Gatekeeper blocks the
  first launch; use Finder's *Open* or `xattr -dr com.apple.quarantine qt.app`.
  `qt.app/Contents/MacOS/` contains both `qt` and `qt-cli`; the `.icns` is generated
  from the PNG set.
- **Windows .msi:** cargo-packager downloads the WiX 3 toolset on first use. The MSI
  installs `qt.exe` and `qt-cli.exe` into one directory and adds Start-menu and desktop
  shortcuts. Release builds of `qt.exe` use the Windows GUI subsystem (no console window).

## Changing the icon

Edit the geometry or colours at the top of `icons/generate.py`, run
`python3 packaging/icons/generate.py`, and commit the regenerated files. CI checks that
`qt.svg` matches the script.

## CI

- `.github/workflows/ci.yml`: rustfmt; clippy (`-D warnings`), build and test of the whole
  workspace on Ubuntu 24.04, macOS 14 (arm64) and Windows 2022 with the llama.cpp submodule
  (`QT_LLAMA_REQUIRE=1`, CPU backend on Linux/Windows, Metal on macOS); validation of the
  desktop entry, packaging scripts (shellcheck) and icon source.
- `.github/workflows/release.yml`: on `v*` tags or manual dispatch, runs `package.sh` on
  the three platforms, smoke-tests the packaged `qt-cli --version`, and uploads
  `target/dist/*` as workflow artifacts. It does not create a GitHub release.
