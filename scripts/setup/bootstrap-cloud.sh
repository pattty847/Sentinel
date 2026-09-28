#!/usr/bin/env bash
# bootstrap-cloud.sh — dependencies for Claude Code on the web (Ubuntu 24.04 container).
#
# Idempotent: every step is skipped when its output already exists, so a cached
# container re-runs this in seconds. Called by .claude/hooks/session-start.sh.
#
# Why not vcpkg / Qt online installer: the cloud egress policy blocks
# github.com archive downloads and download.qt.io, but allows apt, PyPI,
# conda.anaconda.org and `git clone` from GitHub. So:
#   - Boost, OpenSSL-free system libs, yaml-cpp, nlohmann-json, Xvfb: apt
#   - Qt 6.9 (with private headers + CMake configs): conda-forge via py-rattler
#   - msdfgen, jwt-cpp, googletest: git clone (+ build msdfgen)
# Ubuntu's apt Qt is 6.4, too old: HeatmapIntensityNode needs <rhi/qrhi.h> (Qt >= 6.6).
set -euo pipefail

DEPS=${SENTINEL_DEPS:-/opt/sentinel-deps}
QT_SPEC=${SENTINEL_QT_SPEC:-"qt6-main >=6.9,<6.10"}
SUDO=""
[[ $(id -u) -ne 0 ]] && SUDO="sudo -n"

log() { echo "[bootstrap-cloud] $*" >&2; }
mkdir -p "$DEPS"

# 1) apt: toolchain, system libs, headless X + Mesa for the GUI
APT_PKGS=(
    ninja-build ccache pkg-config
    libboost1.83-dev libboost-system1.83-dev nlohmann-json3-dev libyaml-cpp-dev
    libfreetype-dev libpng-dev libtinyxml2-dev
    xvfb xauth libgl1-mesa-dri libegl1 libgl1-mesa-dev
)
missing=()
for p in "${APT_PKGS[@]}"; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" || missing+=("$p")
done
if ((${#missing[@]})); then
    log "apt install ${missing[*]}"
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update -qq
    $SUDO apt-get install -y -qq --no-install-recommends "${missing[@]}" >/dev/null
fi

# 2) header-only / source deps via git (github.com tarballs are blocked, git is not)
clone() { # <dir> <tag> <url>
    [[ -d "$DEPS/$1/.git" ]] && return
    log "clone $1@$2"
    git -c advice.detachedHead=false clone -q --depth 1 -b "$2" "$3" "$DEPS/$1"
}
clone googletest v1.14.0 https://github.com/google/googletest.git
clone jwt-cpp    v0.7.1  https://github.com/Thalhammer/jwt-cpp.git
clone msdfgen    v1.13   https://github.com/Chlumsky/msdfgen.git

# Upstream installs headers under include/msdfgen/; the linux-cloud preset adds
# include/ as a standard include dir so <msdfgen/msdfgen.h> resolves like vcpkg's layout.
if [[ ! -f "$DEPS/prefix/lib/cmake/msdfgen/msdfgenConfig.cmake" ]]; then
    log "build msdfgen"
    cmake -S "$DEPS/msdfgen" -B "$DEPS/msdfgen/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DMSDFGEN_USE_VCPKG=OFF -DMSDFGEN_USE_SKIA=OFF -DMSDFGEN_BUILD_STANDALONE=OFF \
        -DMSDFGEN_INSTALL=ON -DCMAKE_INSTALL_PREFIX="$DEPS/prefix" >/dev/null
    cmake --build "$DEPS/msdfgen/build" --target install >/dev/null
fi

# 3) Qt 6.9 from conda-forge (py-rattler solves + installs without conda/micromamba,
#    whose own download hosts are blocked)
if [[ ! -f "$DEPS/qt/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    if [[ ! -x "$DEPS/rattler-venv/bin/python" ]]; then
        log "py-rattler venv"
        python3 -m venv "$DEPS/rattler-venv"
        "$DEPS/rattler-venv/bin/pip" install -q py-rattler
    fi
    log "install Qt ($QT_SPEC) from conda-forge (~1.2 GB, first run only)"
    "$DEPS/rattler-venv/bin/python" - "$DEPS/qt" "$QT_SPEC" <<'EOF'
import asyncio, sys
from rattler import install, solve, VirtualPackage

prefix, qt_spec = sys.argv[1], sys.argv[2]

async def main():
    recs = await solve(
        sources=["conda-forge"],
        specs=[qt_spec, "qt6-charts"],
        platforms=["linux-64", "noarch"],
        virtual_packages=VirtualPackage.detect(),
    )
    await install(recs, target_prefix=prefix, show_progress=False)
    print(f"installed {len(recs)} packages into {prefix}", file=sys.stderr)

asyncio.run(main())
EOF
fi

log "deps ready in $DEPS"
