# Unofficial official FM1 M-Vave Firmware

English · [简体中文](README.zh-CN.md)

8 tracks, 8 sample slots, 16 FX demos; binary SDK included.

[Workbench and development kit](https://open.m-vave.com/fm1-workbench) · [Official forum](https://forum.m-vave.com)

Select "Source + SDK" in the workbench to download the development kit matching the online release.

## Build

### macOS / Linux

The build runs the same PowerShell scripts inside a `linux/amd64` Docker container (`build.sh`, `tools/docker/Dockerfile`) and produces the same outputs as Windows.

1. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) and start it.
2. Download the JieLi Linux toolchain (~26 MB) once:

   ~~~sh
   mkdir -p ~/.jieli && curl -fL https://pkgman.jieliapp.com/s/linux-toolchain | tar -xJ -C ~/.jieli
   ~~~

   Install elsewhere by setting `FM1_LINUX_TOOLCHAIN` to the directory containing `pi32v2/`.
3. Set `FM1_SOURCE_URL` to the HTTPS download URL of this build's source archive, including its changes, license notices and build/install scripts. Web and package builds stop if it is missing (`export FM1_SOURCE_URL=...` or set per command as shown).
4. Build:

   ~~~sh
   ./build.sh --web       # browser preview -> build/preview/index.html
   ./build.sh             # firmware -> build/dx7/sdk.elf
   ./build.sh --package   # firmware + workbench -> build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc,
                          # build/official-web/FM1-Official-Open-Source.html
   ~~~

The first run builds the container image (pwsh + curl); WASI SDK 27 downloads into `build/tools/` on the first web build; set `FM1_WASI_SDK` to reuse an existing installation. The container build was verified to compile code identical to the official release, byte-for-byte except its 32-byte build identity (see `AGENTS.md`).

### Windows

Requires Windows x64, PowerShell, `curl.exe` and `tar.exe`. Firmware builds also require the [JieLi PI32v2 clang 4.0.1 r3 compiler](https://doc.zh-jieli.com/Tools/zh-cn/dev_tools/dev_env/jl_toolchain.html); its folder must contain `bin/clang.exe`.

Before running the commands below, set the `FM1_SOURCE_URL` environment variable to the HTTPS download URL of this build's source archive, including its changes, license notices and build/install scripts. Web and package builds stop if it is missing.

~~~powershell
.\build.bat --web
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
.\build.bat --package
~~~

The web version builds from the included C sources. The first build downloads WASI SDK 27; set `FM1_WASI_SDK` to reuse an existing installation.

### View the preview

~~~sh
open build/preview/index.html                  # macOS: opens in the default browser
python3 -m http.server -d build/preview 8000   # or serve and visit http://localhost:8000
~~~

The page is self-contained and runs the simulator compiled at build time; after changing sources, rebuild first (`./build.sh --web` or `.\build.bat --web`) so the page reflects them. It never runs on its own — it is a frozen snapshot of a build, labeled by its embedded build identity.

### Publish to GitHub Pages

~~~sh
./tools/publish.sh
~~~

Runs a fresh package build and pushes `build/official-web/FM1-Official-Open-Source.html` to the `gh-pages` branch as `index.html` (with `SHA256SUMS.txt`), printing the live URL and build identity. First publish only: enable Pages in the repo (Settings → Pages → Deploy from branch → `gh-pages`) — a one-time manual step; the workbench then appears at `https://<user>.github.io/<repo>/`. Re-publication is idempotent: no commit is made when the build is unchanged.

Outputs: `build/preview/index.html` (preview), `build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc` (firmware), `build/official-web/FM1-Official-Open-Source.html` (complete workbench).

Hardware drivers are in `app/board/`. The audio interrupt calls `fm1_engine_render()`; `app/audio_output.c` handles USB/Bluetooth mixing and USB recording.

The [per-track tuning API](app/instrument.h) supports custom tuning; a dedicated tuning editor is not included.

APP: [GPLv3 with an SDK linking exception](LICENSE.md); files with other license notices retain their terms. The native engine is supplied as a binary. See [SDK terms, third-party licenses and contributions](THIRD_PARTY.md).

Back up projects and samples, and keep the device powered during saves and firmware updates.
