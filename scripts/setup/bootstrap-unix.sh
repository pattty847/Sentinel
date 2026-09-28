#!/usr/bin/env bash
set -e

echo "=== Sentinel Unix Bootstrap ==="

if [[ -z "$VCPKG_ROOT" ]]; then
    echo "VCPKG_ROOT not set."
    echo 'Set it, e.g.: export VCPKG_ROOT="$HOME/vcpkg"'
    exit 1
fi

UNAME=$(uname)
if [[ "$UNAME" == "Darwin" ]]; then
    if [[ -z "$QT_MAC" ]]; then
        echo "QT_MAC not set."
        echo 'Set it, e.g.: export QT_MAC="/opt/homebrew/opt/qt"'
        exit 1
    fi
    PRESET="mac-clang"
else
    if [[ -z "$QT_LINUX" ]]; then
        echo "QT_LINUX not set."
        echo 'Set it, e.g.: export QT_LINUX="/usr/lib/qt6"'
        exit 1
    fi
    PRESET="linux-gcc"
fi

echo "Environment OK."
echo "Configure with:"
echo "  cmake --preset $PRESET"
echo "Build with:"
echo "  cmake --build --preset $PRESET"
