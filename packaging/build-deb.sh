#!/bin/bash
set -euo pipefail

VERSION="${1:-1.0.0}"
ARCH="$(dpkg --print-architecture)"
PKG="poweroffd"
BUILDDIR="$(mktemp -d)"
PKGDIR="${BUILDDIR}/${PKG}_${VERSION}_${ARCH}"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "Building ${PKG} ${VERSION} for ${ARCH}..."

# Build binaries
cd "$SRCDIR"
make clean
make

# Create package structure
mkdir -p "${PKGDIR}/DEBIAN"
mkdir -p "${PKGDIR}/usr/local/sbin"
mkdir -p "${PKGDIR}/usr/local/bin"
mkdir -p "${PKGDIR}/etc"
mkdir -p "${PKGDIR}/etc/systemd/system"

# Install binaries
install -m 0755 poweroffd     "${PKGDIR}/usr/local/sbin/poweroffd"
install -m 0755 poweroff-send "${PKGDIR}/usr/local/bin/poweroff-send"

# Config (marked as conffile so apt won't overwrite user edits)
install -m 0600 poweroffd.conf "${PKGDIR}/etc/poweroffd.conf"

# systemd unit
install -m 0644 poweroffd.service "${PKGDIR}/etc/systemd/system/poweroffd.service"

# Control file
cat > "${PKGDIR}/DEBIAN/control" <<EOF
Package: ${PKG}
Version: ${VERSION}
Section: admin
Priority: optional
Architecture: ${ARCH}
Depends: libssl3 (>= 3.0) | libssl3t64 (>= 3.0), libcap2 (>= 2.25)
Maintainer: Krishna Teja <kasaiarashi@users.noreply.github.com>
Homepage: https://github.com/kasaiarashi/poweroffd
Description: WoL-style remote shutdown daemon
 Production-grade Linux daemon that listens for Wake-on-LAN magic packets
 on a configurable UDP port and shuts down the system when a valid packet
 is received. Supports HMAC-SHA256 authentication, MAC filtering, privilege
 dropping, rate limiting, and systemd integration.
EOF

# conffiles — tells dpkg not to overwrite user-edited config
cat > "${PKGDIR}/DEBIAN/conffiles" <<EOF
/etc/poweroffd.conf
EOF

# postinst
cat > "${PKGDIR}/DEBIAN/postinst" <<'EOF'
#!/bin/bash
set -e
systemctl daemon-reload
echo ""
echo "poweroffd installed successfully!"
echo ""
echo "  1. Edit /etc/poweroffd.conf (set your MAC address)"
echo "  2. sudo systemctl enable --now poweroffd"
echo ""
EOF
chmod 0755 "${PKGDIR}/DEBIAN/postinst"

# prerm
cat > "${PKGDIR}/DEBIAN/prerm" <<'EOF'
#!/bin/bash
set -e
if systemctl is-active --quiet poweroffd 2>/dev/null; then
    systemctl stop poweroffd
fi
if systemctl is-enabled --quiet poweroffd 2>/dev/null; then
    systemctl disable poweroffd
fi
EOF
chmod 0755 "${PKGDIR}/DEBIAN/prerm"

# postrm
cat > "${PKGDIR}/DEBIAN/postrm" <<'EOF'
#!/bin/bash
set -e
systemctl daemon-reload
if [ "$1" = "purge" ]; then
    rm -f /etc/poweroffd.conf
fi
EOF
chmod 0755 "${PKGDIR}/DEBIAN/postrm"

# Build the .deb
dpkg-deb --build --root-owner-group "${PKGDIR}"

# Move to source dir
mv "${BUILDDIR}/${PKG}_${VERSION}_${ARCH}.deb" "${SRCDIR}/"

# Cleanup
rm -rf "${BUILDDIR}"

echo ""
echo "Package built: ${SRCDIR}/${PKG}_${VERSION}_${ARCH}.deb"
echo "Install with:  sudo dpkg -i ${PKG}_${VERSION}_${ARCH}.deb"
