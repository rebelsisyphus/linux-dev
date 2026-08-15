#!/bin/bash
# TUN + xfrm reverse-path race reproducer for icmp_route_lookup ip_rt_bug.
# This matches the call trace from the second reference patch.

set -e

LOG="/mnt/shared/repro_xfrm_race.log"
REPRO_NAME="repro_xfrm_race"

{
    echo "=== xfrm reverse-path race reproducer (ip_options_compile -> __icmp_send) ==="
    echo "Test time: $(date)"
    echo "Kernel: $(uname -r)"

    if [ ! -f "/mnt/shared/${REPRO_NAME}" ]; then
        echo "ERROR: reproducer binary not found: /mnt/shared/${REPRO_NAME}"
        exit 1
    fi

    cp "/mnt/shared/${REPRO_NAME}" /tmp/${REPRO_NAME}
    chmod +x /tmp/${REPRO_NAME}

    echo "Running reproducer..."
    output=$(/tmp/${REPRO_NAME} 2>&1)
    repro_exit=$?
    echo "$output"
    echo "Reproducer exit code: ${repro_exit}"

    echo ""
    echo "=== Checking dmesg for ip_rt_bug / WARNING ==="
    if dmesg | grep -qE "ip_rt_bug|WARNING:"; then
        echo "RESULT: REPRODUCED - ip_rt_bug or WARNING in dmesg"
        dmesg | grep -E "ip_rt_bug|WARNING:" | head -20
    else
        echo "RESULT: NOT REPRODUCED - no ip_rt_bug/WARNING in dmesg"
    fi

    echo ""
    echo "=== Full dmesg tail (30 lines) ==="
    dmesg | tail -30
} | tee "$LOG"

exit 0
