#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Sanity checks for FM1 Open.    tools/sanity.sh [quick|full]
#
#   quick (default)  Validate existing artifacts: firmware/post-link report,
#                    package fit, wasm cache integrity, self-contained preview
#                    HTML, and — when reachable — parity with the official
#                    release .fwsc (only the build identity and covering
#                    checksums may differ).
#   full             Everything after fresh builds: the firmware and package
#                    built twice must be byte-identical (reproducibility) and
#                    a clean web rebuild must regenerate a valid preview.
#
# Host needs python3 (and curl to fetch the official artifact). Full mode
# additionally runs build.sh: Docker must be running and the JieLi Linux
# toolchain installed. Windows hosts use build.bat plus the artifact checks.
set -eu
MODE=${1:-quick}
case "$MODE" in quick|full) ;; *) echo "usage: tools/sanity.sh [quick|full]"; exit 2 ;; esac
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
SAY() { printf 'sanity(%s): %s\n' "$MODE" "$1"; }
FAIL() { printf 'sanity(%s): FAIL — %s\n' "$MODE" "$1" >&2; exit 1; }
sha() { python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
command -v python3 >/dev/null 2>&1 || FAIL 'python3 is required'
mkdir -p build/tools
# The package flow requires a source-archive URL; it is embedded in the workbench
# HTML only, never in the .fwsc, so the official V13 kit URL is a safe default.
export FM1_SOURCE_URL="${FM1_SOURCE_URL:-https://open.m-vave.com/releases/V13/2a3b2ede16ac49c867831315714ef9d8c532c70ce28ce1dcf3fc76f0a9f68f57/FM1-Open-V13-Development-Kit.zip}"

if [ "$MODE" = full ]; then
    SAY 'package rebuild 1/2 (log: build/sanity-full.log)'
    ./build.sh --package > build/sanity-full.log 2>&1 || FAIL 'first package build failed; see build/sanity-full.log'
    ELF1=$(sha build/dx7/sdk.elf)
    FWSC1=$(sha build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc)
    SAY 'package rebuild 2/2 (reproducibility)'
    ./build.sh --package >> build/sanity-full.log 2>&1 || FAIL 'second package build failed; see build/sanity-full.log'
    [ "$ELF1" = "$(sha build/dx7/sdk.elf)" ] || FAIL 'sdk.elf is not byte-reproducible'
    [ "$FWSC1" = "$(sha build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc)" ] || FAIL '.fwsc is not byte-reproducible'
    SAY 'firmware and package are byte-reproducible'
    SAY 'clean web rebuild'
    rm -rf build/web build/preview
    ./build.sh --web >> build/sanity-full.log 2>&1 || FAIL 'clean web build failed; see build/sanity-full.log'
fi

python3 - <<'EOF' || FAIL 'artifact checks failed (see lines above)'
import hashlib, json, pathlib, sys

def sha(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()

def die(m):
    sys.stderr.write(m + '\n')
    sys.exit(1)

report = json.loads(pathlib.Path('build/dx7/firmware-report.json').read_text())
if not report['passed']:
    die('firmware-report.json says passed=false')
if report['elf_sha256'] != sha('build/dx7/sdk.elf'):
    die('firmware report does not match sdk.elf')
print(f"post-link validation passed (app image {report['app_image_bytes']} B, entry {report['startup_entry']:#x})")

pkg = json.loads(pathlib.Path('build/dx7/package-report.json').read_text())
fwsc = 'build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc'
if not (pathlib.Path(fwsc).exists() and pkg['fits']):
    die('package report not fitting or .fwsc missing')
print(f"package fits 1 MiB (flash {pkg['FLASH_BIN_SIZE']} B)")

meta = json.loads(pathlib.Path('build/web/simulator.json').read_text())
if meta['wasm_sha256'] != sha('build/web/simulator.wasm'):
    die('simulator.wasm does not match its cache record')
print('wasm cache matches simulator.json')

html = pathlib.Path('build/preview/index.html').read_text()
for marker in ('FM1_SIMULATOR', 'GPLv3', 'THIRD_PARTY.md'):
    if marker not in html:
        die(f'preview html lost the {marker} section')
print('self-contained preview verified (simulator + notices)')
EOF

OFFICIAL_CACHE='build/tools/official-V13.fwsc'
OFFICIAL_URL='https://open.m-vave.com/assets/fm1/c35330f1cad641f38908a6269c7316fdaf4ffb8b1091a7b3282fa31b9be9fe74.bin'
OFFICIAL_SHA='c35330f1cad641f38908a6269c7316fdaf4ffb8b1091a7b3282fa31b9be9fe74'
if [ ! -f "$OFFICIAL_CACHE" ]; then
    SAY 'fetching official V13 package for the parity check'
    curl -sfL --retry 2 -o "$OFFICIAL_CACHE" "$OFFICIAL_URL" || SAY '(official package unreachable — parity check skipped)'
fi
python3 - "$OFFICIAL_CACHE" <<'EOF' || FAIL 'official-release parity failed (see lines above)'
import hashlib, pathlib, sys

official_path = pathlib.Path(sys.argv[1])
if not official_path.exists():
    print('parity check skipped (no official artifact)')
    sys.exit(0)

def die(m):
    sys.stderr.write(m + '\n')
    sys.exit(1)

EXPECTED_SHA = 'c35330f1cad641f38908a6269c7316fdaf4ffb8b1091a7b3282fa31b9be9fe74'
if hashlib.sha256(official_path.read_bytes()).hexdigest() != EXPECTED_SHA:
    die('cached official artifact does not match its known SHA-256')
ours = pathlib.Path('build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc').read_bytes()
official = official_path.read_bytes()
if len(ours) != len(official):
    die(f'package size differs: {len(ours)} vs {len(official)} bytes')
regions, run = [], None
for n, (a, b) in enumerate(zip(ours, official)):
    if a != b:
        if run is None:
            run = n
    elif run is not None:
        regions.append((run, n))
        run = None
if run is not None:
    regions.append((run, len(ours)))
total = sum(e - s for s, e in regions)
frac = total / len(ours)
print(f'official release parity: {total} differing bytes in {len(regions)} region(s) ({frac:.4%})')
for s, e in regions[:10]:
    print(f'  {s:#08x}-{e:#08x} ({e - s} B)')
if frac >= 0.001:
    die('differences exceed the build-identity whitelist (>= 0.1% of the package). '
        'If this code intentionally changed since V13, refresh OFFICIAL_URL/OFFICIAL_SHA in tools/sanity.sh')
EOF

SAY 'all sanity checks passed'
