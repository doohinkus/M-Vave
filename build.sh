#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Non-Windows build driver; mirrors build.bat (Windows). Runs the same PowerShell
# build scripts inside a linux/amd64 container with the JieLi Linux toolchain.
#   ./build.sh              Build firmware
#   ./build.sh --web        Build browser preview
#   ./build.sh --package    Build firmware and complete workbench
# Requires Docker (running), the JieLi Linux toolchain (default ~/.jieli/toolchain,
# override with FM1_LINUX_TOOLCHAIN) and, for --web/--package, FM1_SOURCE_URL.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IMAGE="${FM1_BUILD_IMAGE:-fm1-open-build}"
JIELI="${FM1_LINUX_TOOLCHAIN:-$HOME/.jieli/toolchain}"

case "${1:-}" in
--help)
    sed -n '4,8p' "$0"
    exit 0
    ;;
esac
command -v docker >/dev/null 2>&1 || { echo "build.sh: docker is required."; exit 2; }
docker info >/dev/null 2>&1 || { echo "build.sh: Docker is not running."; exit 2; }
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "build.sh: building $IMAGE image (one-time)"
    docker build -t "$IMAGE" "$ROOT/tools/docker"
fi
[ -x "$JIELI/pi32v2/bin/clang" ] || {
    echo "build.sh: no JieLi Linux toolchain at $JIELI. Install it:"
    echo "  mkdir -p \$(dirname '$JIELI') && curl -fL https://pkgman.jieliapp.com/s/linux-toolchain | tar -xJ -C '$JIELI/..'"
    echo "or point FM1_LINUX_TOOLCHAIN at the directory containing pi32v2/."
    exit 2
}

run() {
    docker run --rm --platform linux/amd64 -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -e FM1_SOURCE_URL -e FM1_WASI_SDK -e FM1_PI32_TOOLCHAIN=/opt/jieli \
        -v "$ROOT":/src -v "$JIELI":/opt/jieli:ro -w /src "$IMAGE" "$@"
}

case "${1:-}" in
"") run pwsh -NoProfile -File /src/sdk/Build.ps1 ;;
--web) run pwsh -NoProfile -File /src/tools/Build-Web.ps1 ;;
--package) run pwsh -NoProfile -File /src/sdk/Build.ps1 -Package ;;
*)
    echo "Use build.sh, build.sh --web or build.sh --package."
    exit 2
    ;;
esac
