#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Publish the workbench to the gh-pages branch.    tools/publish.sh
# Rebuilds fresh via ./build.sh --package (never republishes stale artifacts),
# copies the integrated workbench HTML onto a gh-pages worktree (build/gh-pages,
# gitignored) as index.html, and pushes it. GitHub Pages serves:
#   https://<owner>.github.io/<repo>/   (enable once in Settings → Pages)
# Host needs python3, curl and git; uses build.sh (Docker + JieLi toolchain).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
SAY() { printf 'publish: %s\n' "$1"; }
FAIL() { printf 'publish: FAIL — %s\n' "$1" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || FAIL 'python3 is required'
command -v curl >/dev/null 2>&1 || FAIL 'curl is required'

PUSH_URL=$(git remote get-url doohinkus 2>/dev/null || git remote get-url origin)
LIVE=$(printf '%s' "$PUSH_URL" | sed -E 's#\.git$##' | sed -E 's#https://github\.com/([^/]+)/([^/]+)$#https://\1.github.io/\2/#' | tr '[:upper:]' '[:lower:]')

export FM1_SOURCE_URL="${FM1_SOURCE_URL:-https://open.m-vave.com/releases/V13/2a3b2ede16ac49c867831315714ef9d8c532c70ce28ce1dcf3fc76f0a9f68f57/FM1-Open-V13-Development-Kit.zip}"
SAY 'building firmware + integrated workbench (log: build/publish.log)'
./build.sh --package > build/publish.log 2>&1 || FAIL 'package build failed; see build/publish.log'
tail -n 3 build/publish.log | sed 's/^/  /'

VERSION=$(python3 -c 'import json;d=json.load(open("build/dx7/build-identity.json"));print(d["release_version"],d["build_id"][:12])')
set -- $VERSION; RELEASE=$1; BUILD_ID=$2

PAGES=build/gh-pages
if [ ! -d "$PAGES/.git" ]; then
    SAY 'preparing gh-pages branch (first publish)'
    rm -rf "$PAGES"
    git clone -q --no-checkout "$ROOT" "$PAGES"
    git -C "$PAGES" switch --orphan gh-pages
fi
git -C "$PAGES" pull -q --ff-only "$PUSH_URL" gh-pages 2>/dev/null || true

cp build/official-web/FM1-Official-Open-Source.html "$PAGES/index.html"
: > "$PAGES/.nojekyll"
cd "$PAGES"
INDEX_SHA=$(python3 -c 'import hashlib;print(hashlib.sha256(open("index.html","rb").read()).hexdigest())')
printf '%s  index.html\n' "$INDEX_SHA" > SHA256SUMS.txt
git add -A
if git diff --cached --quiet; then
    SAY "gh-pages already at this build ($RELEASE $BUILD_ID)"
else
    git commit -qm "Publish FM1 workbench $RELEASE (build $BUILD_ID)"
    git push "$PUSH_URL" gh-pages
    SAY "pushed gh-pages: $RELEASE (build $BUILD_ID)"
fi
cd "$ROOT"
SAY "live URL: $LIVE (after Pages is enabled in repo Settings — one-time manual step)"
