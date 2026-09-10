#!/bin/bash
# Run the installed test-kernel workflow in an isolated VM.
set -euo pipefail

kernel=$(realpath "${1:?usage: run-test-kernel.sh KERNEL TEST_DIRECTORY}")
tests=$(realpath "${2:?usage: run-test-kernel.sh KERNEL TEST_DIRECTORY}")
runner=$(command -v test-kernel)
vm=$(mktemp -d /tmp/rfs-kernel-vm.XXXXXX)
port=${RFS_SSH_PORT:-2242}
rootfs=${RFS_ROOTFS:-/home/sisyphus/code/qemu/rootfs.img}

test -f "$tests/test.sh"
if nc -z 127.0.0.1 "$port" 2>/dev/null; then
	echo "Port $port is already occupied" >&2
	exit 1
fi
qemu-img create -f qcow2 -F raw -b "$rootfs" "$vm/rootfs.img"

# The installed runner has global process cleanup and a negated-if status bug.
# Adapt a private copy, leaving the installed tool and other VMs untouched.
python3 - "$runner" "$vm" "$tests" "$port" <<'PY'
from pathlib import Path
import sys
import os

source, vm, tests, port = sys.argv[1:]
text = Path(source).read_text()
start = text.index('QEMU_PIDS=$(pgrep')
end = text.index('# Step 2:')
text = text[:start] + text[end:]
text = text.replace('QEMU_DIR="/home/sisyphus/code/qemu"', f'QEMU_DIR="{vm}"')
text = text.replace('TEST_DIR="/home/sisyphus/code/test"', f'TEST_DIR="{tests}"')
text = text.replace('SSH_PORT=2222', f'SSH_PORT={port}')
for name in ('qemu.pid', 'panic_monitor.pid', 'panic_detected.flag'):
    text = text.replace('/tmp/' + name, vm + '/' + name)
text = text.replace('-machine q35', '-name rfs-bpf-test -machine q35,accel=kvm -cpu host')
text = text.replace('size=1G', 'size=2G').replace('-m 2G', '-m 4G')
text = text.replace('-smp 4,sockets=2,cores=2', '-smp 8,sockets=2,cores=4')
text = text.replace('memdev=mem0,cpus=0-1', 'memdev=mem0,cpus=0-3')
text = text.replace('memdev=mem1,cpus=2-3', 'memdev=mem1,cpus=4-7')
text = text.replace('format=raw', 'format=qcow2')
text = text.replace('-nographic', '-display none -monitor none -serial file:"$SERIAL_LOG" '
                    '-daemonize -pidfile "$QEMU_DIR/qemu.pid"')
text = text.replace('> "$SERIAL_LOG" 2>&1 &', '> "$QEMU_DIR/launch.log" 2>&1')
text = text.replace('QEMU_PID=$!', 'QEMU_PID=$(cat "$QEMU_DIR/qemu.pid")')
if os.environ.get('RFS_TAP'):
    tap = os.environ['RFS_TAP']
    if not tap.replace('-', '').replace('_', '').isalnum():
        raise SystemExit('Invalid RFS_TAP interface name')
    text = text.replace('    -device e1000,netdev=net0 \\\n',
                        '    -device e1000,netdev=net0 \\\n'
                        f'    -netdev tap,id=perf,ifname={tap},script=no,downscript=no,vhost=on \\\n'
                        '    -device virtio-net-pci,netdev=perf,mac=52:54:00:7f:00:01,mq=off \\\n')
text = text.replace('TEST_EXIT_CODE=$?', 'TEST_EXIT_CODE=1')
mount = '''ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    root@localhost -p "$SSH_PORT" \
    'mkdir -p /mnt/shared; mountpoint -q /mnt/shared || mount -t 9p -o trans=virtio shared /mnt/shared'
'''
text = text.replace('# Step 6: 执行测试', mount + '\n# Step 6: 执行测试')
Path(vm, 'test-kernel').write_text(text)
PY

echo "RFS VM directory: $vm"
echo "RFS SSH port: $port"
echo "$vm" > "$tests/vm-directory.txt"
cleanup()
{
	ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		-p "$port" root@localhost dmesg > "$vm/dmesg.log" 2>/dev/null || true
	if [[ ${RFS_KEEP_VM:-0} != 1 ]]; then
		for name in panic_monitor qemu; do
			if [[ -f "$vm/$name.pid" ]]; then
				kill "$(cat "$vm/$name.pid")" 2>/dev/null || true
			fi
		done
	fi
}
trap cleanup EXIT
bash "$vm/test-kernel" "$kernel"
