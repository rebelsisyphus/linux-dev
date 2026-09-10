#!/bin/bash
set -euo pipefail
dev=''
for path in /sys/class/net/*/address; do
	if [[ $(cat "$path") == 52:54:00:7f:00:01 ]]; then
		dev=$(basename "$(dirname "$path")")
	fi
done
test -n "$dev"
ip address replace 192.0.2.2/30 dev "$dev"
ip link set "$dev" up
echo "$dev" > /mnt/shared/perf-device
systemctl stop irqbalance 2>/dev/null || true
for irq in /sys/class/net/"$dev"/device/../msi_irqs/*; do
	echo 0 > "/proc/irq/$(basename "$irq")/smp_affinity_list"
done
mkdir -p /run/rfs-bpf
mountpoint -q /run/rfs-bpf || mount -t bpf bpf /run/rfs-bpf
uname -a
cat /etc/os-release
cat /sys/devices/system/node/node*/cpulist
ip -brief address
cat /proc/interrupts
for cmd in python3 numactl redis-server redis-benchmark ethtool gcc; do
	command -v "$cmd" || true
done
ldd --version | head -1
echo 'RFS performance VM ready'
