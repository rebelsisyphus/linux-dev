#!/bin/bash
# Disposable guest only; the host rootfs is protected by a QEMU snapshot.
set -euo pipefail
SHARE=/mnt/shared
if ! command -v nft >/dev/null; then
    apt-get update > "$SHARE/guest-tools.log" 2>&1
    DEBIAN_FRONTEND=noninteractive apt-get install -y nftables iproute2 python3 >> "$SHARE/guest-tools.log" 2>&1
fi
cp -a "$SHARE/modules/lib/modules/." /lib/modules/
depmod -a
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
insmod "$SHARE/set_probe.ko"
python3 "$SHARE/opaque-set-test.py" | tee "$SHARE/runtime-opaque-set.log"
rmmod set_probe
insmod "$SHARE/clone_probe.ko" clone_symbol=pipapo_maybe_clone.part.0
python3 "$SHARE/clone-flush-test.py" | tee "$SHARE/runtime-clone-flush.log"
rmmod clone_probe
insmod "$SHARE/pipapo_fail.ko"
python3 "$SHARE/pipapo-regression.py" | tee "$SHARE/runtime-cve72252.log"
rmmod pipapo_fail
dmesg > "$SHARE/guest-dmesg.log"
cat /proc/sys/kernel/tainted > "$SHARE/guest-taint.txt"
if grep -E 'BUG:|WARNING: CPU:|KASAN:|Oops:|general protection fault|refcount_t:|list_del corruption|possible circular locking dependency|suspicious RCU usage|sleeping function called' "$SHARE/guest-dmesg.log"; then
    exit 1
fi
echo 'PASS: clone-on-flush, map reference rollback, legacy backends and CVE-2026-72252 regression'
