# AGENTS.md

Guidance for AI agents working in this repo. `README.md`/`README.zh-CN.md` document both build flows (Windows and the `build.sh` container); this file holds the build internals and traps agents would otherwise rediscover.

## What this is

Firmware (APP) for the M-VAVE FM-1 groovebox plus a browser simulator/workbench, linked against a binary JieLi SDK in `sdk/`. APP: GPLv3 + SDK linking exception; SDK binaries/boot resources have separate terms in `THIRD_PARTY.md`. No test suite, no CI — the builds are the only automated verification. The official "Source + SDK" kit from https://open.m-vave.com/fm1-workbench was verified byte-identical to this git tree (all 150 files) except `sdk/Build.ps1`, `sdk/manifest.json` and `tools/Build-Web.ps1`, which carry this repo's platform-portability work.

## Two flows, one build implementation

- **Windows:** `build.bat` (plain `--web` `--package`) — the documented flow in README.md.
- **macOS/Linux:** `./build.sh <same flags>` — runs the same PowerShell scripts via pwsh inside a `linux/amd64` Docker container (`tools/docker/Dockerfile`, built once as image `fm1-open-build`; first `./build.sh` builds it). Container runs as host UID, so `build/` files stay user-owned.

Verification ladder: `tools/sanity.sh quick` (default) validates existing artifacts (post-link report, package fit, wasm cache, preview self-containment, official-release parity — only the build identity + its covering checksums may differ); `tools/sanity.sh full` adds rebuilds first (firmware/package built twice, byte-identical required, clean web rebuild).

Publishing: `tools/publish.sh` always builds fresh via `build.sh --package` (never republishes stale artifacts — the published page is a static snapshot labeled by its embedded build identity) and pushes the integrated workbench to the `gh-pages` branch: `https://doohinkus.github.io/m-vave/`. It is idempotent (no commit when unchanged). Enabling Pages in the repo's Settings is a one-time manual owner step the script cannot do.

Prerequisites for `build.sh`: Docker **running**; JieLi *Linux* toolchain (repo does not bundle it):

    mkdir -p ~/.jieli && curl -fL https://pkgman.jieliapp.com/s/linux-toolchain | tar -xJ -C ~/.jieli

Directory containing `pi32v2/bin/clang` (default `~/.jieli/toolchain`, override `FM1_LINUX_TOOLCHAIN`). Based on the pattern proven by the Felucca fork working on this machine.

- Outputs: `build/preview/index.html` (`--web`), `build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc` (`--package` firmware), `build/official-web/FM1-Official-Open-Source.html` (integrated workbench).
- Env: `FM1_SOURCE_URL` (required for `--web`/`--package`; HTTPS, stops builds without it), `FM1_PI32_TOOLCHAIN` (Windows), `FM1_WASI_SDK` (reuse WASI SDK 27 instead of re-downloading; cached under `build/tools/` either way).
- Version contract: `app/version.h` `FM1_RELEASE_VERSION "V13"` ↔ `FM1_UPDATE_PRODUCT "FM-1_8000013"` are regex-validated and must stay in sync.
- Builds are full rebuilds (object dir wiped); firmware seconds-level-to-minutes in the container on an Intel Mac.
- The container build's `.fwsc` matched the official V13 release byte-for-byte except its 32-byte build identity and the checksum fields covering it (the identity hashes `sdk/manifest.json`, which legitimately changed when Linux toolchain pins were added). No codegen differences.

## `sdk/` is sealed — do not modify

- `sdk/manifest.json` pins SHA-256 of every file under `sdk/`; the firmware build rejects modified files *and* unexpected extra files. Exception: after editing `sdk/Build.ps1`, update its pin in `sdk/manifest.json` — the error "SDK payload changed: sdk/Build.ps1" means exactly that.
- `sdk/Build.ps1` pins the compiler too: Windows binaries (`external_toolchain`, `bin/*.exe` + `LLVMgold.dll`) on Windows, Linux binaries (`external_toolchain_linux`, `pi32v2/bin/*` + clang resource headers) elsewhere. A different toolchain drop means re-pinning every listed file.
- `sdk/synth.json` seals `sdk/include/fm1_block.h` and `sdk/lib/libfm1_engine.a`; `app/synth/engine.h` must keep `#define FM1_DSP_ABI 3U`.
- `.gitattributes` sets `sdk/** -text`: never renormalize line endings there — byte changes break the hashes.
- Web builds embed `LICENSE.md`/`THIRD_PARTY.md` verbatim into the output HTML — do not reword them casually.

## One source tree, two compilation targets

- **Firmware** compiles all `.c`/`.S` under `app/` recursively **except `app/synth/`** — the DSP engine ships prebuilt in `sdk/lib/libfm1_engine.a` (native ELF, not bitcode). New `app/` sources are picked up automatically.
- **Web simulator** compiles `app/` (except `app/board/`), `web/`, `third_party/synth` with WASI SDK under `-DFM1_HOST_TEST -DFM1_OPEN_WEB -Wall -Wextra -Werror`: warnings are errors, and any edit to those trees invalidates the wasm cache. This is the fastest full-compile check of shared code in the container — run `./build.sh --web` after C changes.
- Hardware drivers exist only in `app/board/`; host/device code splits on `#ifdef FM1_HOST_TEST`.
- Audio-path code/data must be marked `FM1_AUDIO_CODE` / `FM1_AUDIO_DATA` (`sdk/include/fm1.h`; no-ops on host): audio keeps rendering during Flash programming, and the build verifies audio callbacks sit in internal SRAM (0x01c00000–0x01c80000).
- Per-file compile rules in `sdk/Build.ps1`: `app/screen.c`, `app/transfer.c`, `app/board/panel.c` compile `-fno-lto` (the PI32 LTO plugin can drop their definitions against weak SDK defaults); `project.c`/`transfer.c`/`samples.c`/`screen.c`/`presets.c`/`capture.c` compile `-Oz`, everything else `-O2`.

## Live capture (app/capture.c)

EDIT+REC on a SAMPLER track opens the GRAB page: records Bluetooth audio or the master mix into the sample bank. Internals an agent must know:

- The ISR entry is `fm1_capture_block` (listed in `sdk/Build.ps1`'s audio-SRAM verification set); it copies the BT block or converts the Q23 master mix into an SPSC ring, dropping its own block when the control task stalls. Everything else runs on the control task from `groove_transfer_poll`.
- The polyphase table (320 phases × 32 taps, 44.1→32 kHz) is generated at boot into BSS by `fm1_capture_arm` from `app/capture_sin.h` (regenerate with `tools/gen-resample-table.py`). Do not move it to flash rodata — see the flash budget note below.
- Takes stream through `web/synth/sea_encode.h` into a heap staging buffer sized from real malloc slack at arm time (~4–8 s; RAM is plentiful, flash is not).
- The commit machine lives in `app/samples.c` (`groove_samples_capture_*`): bank data lands beyond the current image while audio plays (first free pad; a full bank replaces the last pad), sector 0 — header plus entry 0's first bytes — is rewritten last under unbind via read-modify-write, then the full-bank CRC and `bind()`. The RAM `header`/`total` must be synced to the new image before `bind()`. `groove_samples_begin` is rejected while the machine holds the bank.
- `web/synth/sea_encode.h`'s whole-buffer wrapper must stay byte-identical with the streaming path — browser WAV imports depend on it. The encoder DQT is `web/synth/sample_dqt.inc`; `capture.c` defines `FM1_DSP_DATA` empty before including it so the table stays in flash rodata, not the tight audio SRAM region. `web/synth` is on the firmware include path for this.
- Device-only checks (needs hardware): A2DP capture quality, commit-time output blips, real free-heap staging sizes, control-task latency (the GRAB page shows INPUT OVERLOAD on ring drops).

## Post-link validation is the safety net

`Test-Firmware` in `sdk/Build.ps1` fails the build if: the ELF carries sections outside the whitelist (`.text/.data/.bss/ram0_data/ram0_bss/boot_info/cache_ram_data/cache_ram_bss`); required runtime symbols are missing or SRAM mis-placed; LVGL, extra filesystems or unsupported SDK link members appear; runtime heap < 48 KiB; the startup entry is not the fixed 0x02000120. Such failures mean a layout/placement change broke a platform invariant — fix the placement (e.g. use `FM1_AUDIO_CODE`), never weaken the check.

Flash budget: `sdk/Package.cs` rejects app images over 453,921 bytes (1 MiB flash minus BT/VM/user partitions); the pre-capture baseline was 445,244, leaving ~8 KiB slack for new features. BSS is cheap — RAM has ~270 KB of heap headroom — so large constant tables belong in BSS, generated at boot (see Live capture).

## Conventions

- Every C/H source starts with `/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */` — match it in new files.
- `build/` is disposable and gitignored; the `sdk/` tree must stay byte-identical.
- USB/BLE behavior cannot be verified on the host — rely on the simulator and firmware validation, and flag what still needs a real FM-1 test.
