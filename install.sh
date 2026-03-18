#!/bin/bash
# One-liner: curl -fsSL https://raw.githubusercontent.com/kasaiarashi/poweroffd/main/install.sh | sudo bash
set -euo pipefail

REPO="kasaiarashi/poweroffd"
VERSION="${1:-latest}"
TMP="$(mktemp -d)"

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo "==> poweroffd installer"

# Detect architecture
ARCH="$(dpkg --print-architecture 2>/dev/null || echo "amd64")"

# Resolve version
if [ "$VERSION" = "latest" ]; then
    VERSION="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | head -1 | sed 's/.*"v\?\([^"]*\)".*/\1/')"
fi

if [ -z "$VERSION" ]; then
    echo "Error: Could not determine latest version."
    echo "Usage: $0 [version]  (e.g., $0 1.0.0)"
    exit 1
fi

DEB="poweroffd_${VERSION}_${ARCH}.deb"
URL="https://github.com/${REPO}/releases/download/v${VERSION}/${DEB}"

echo "==> Downloading ${DEB}..."
if ! curl -fsSL -o "${TMP}/${DEB}" "$URL"; then
    echo ""
    echo "Error: Failed to download ${URL}"
    echo ""
    echo "No prebuilt .deb for your architecture (${ARCH})?"
    echo "Build from source instead:"
    echo "  git clone https://github.com/${REPO}.git"
    echo "  cd poweroffd && sudo apt install -y build-essential libcap-dev libssl-dev"
    echo "  bash packaging/build-deb.sh ${VERSION}"
    echo "  sudo dpkg -i poweroffd_${VERSION}_${ARCH}.deb"
    exit 1
fi

echo "==> Installing..."
dpkg -i "${TMP}/${DEB}"

echo ""
echo "==> Done! Next steps:"
echo "  1. sudo vim /etc/poweroffd.conf   (set your MAC address)"
echo "  2. sudo systemctl enable --now poweroffd"
echo ""
