#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-vcan0}"

if ! ip link show "$IFACE" >/dev/null 2>&1; then
    sudo ip link add dev "$IFACE" type vcan
fi
sudo ip link set up "$IFACE"

echo "$IFACE is up"
echo "For a real CAN interface use:"
echo "  sudo ip link set can0 type can bitrate 500000"
echo "  sudo ip link set up can0"
