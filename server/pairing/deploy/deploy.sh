#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the pairing server for the VM and install or update it there.
#
#   server/pairing/deploy/deploy.sh ubuntu@<vm-ip> [-i ~/.ssh/meleevs_pairing]
#
# Needs Go locally and sudo on the VM (Ubuntu with systemd). Safe to re-run:
# it replaces the binary and units and restarts the service, and keeps the
# signing key the service made on its first start. Prints that key's public
# half, which the client needs compiled in (PC_PAIRING_KEY).
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 user@host [ssh options...]" >&2
    exit 2
fi
target=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
port=27720

arch=$(ssh "$@" "$target" uname -m)
case "$arch" in
    x86_64) goarch=amd64 ;;
    aarch64) goarch=arm64 ;;
    *) echo "unsupported VM architecture: $arch" >&2; exit 1 ;;
esac

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
(cd "$here/.." && CGO_ENABLED=0 GOOS=linux GOARCH=$goarch \
    go build -trimpath -ldflags="-s -w" -o "$build/pairing" .)
cp "$here/pairing.service" "$here/pairing-health.service" "$here/pairing-health.timer" "$build/"

scp "$@" "$build/pairing" "$build/pairing.service" "$build/pairing-health.service" \
    "$build/pairing-health.timer" "$target:/tmp/"

ssh "$@" "$target" "sudo bash -s" <<EOF
set -euo pipefail
install -m 0755 /tmp/pairing /usr/local/bin/pairing
install -m 0644 /tmp/pairing.service /tmp/pairing-health.service /tmp/pairing-health.timer \
    /etc/systemd/system/
rm -f /tmp/pairing /tmp/pairing.service /tmp/pairing-health.service /tmp/pairing-health.timer

# Oracle's Ubuntu images reject everything but SSH in the VM's own iptables,
# on top of the cloud security list: open the pairing port and the NAT
# check's port + 1, and keep them open.
for p in $port $((port + 1)); do
    if ! iptables -C INPUT -p udp --dport \$p -j ACCEPT 2>/dev/null; then
        iptables -I INPUT -p udp --dport \$p -j ACCEPT
    fi
done
if command -v netfilter-persistent >/dev/null; then
    netfilter-persistent save >/dev/null
fi

systemctl daemon-reload
systemctl enable pairing.service pairing-health.timer >/dev/null 2>&1
systemctl restart pairing.service
systemctl start pairing-health.timer
sleep 1
/usr/local/bin/pairing -check 127.0.0.1:$port
echo "public key: \$(/usr/local/bin/pairing -key /var/lib/private/pairing/server.key -pubkey)"
EOF
