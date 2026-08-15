#!/bin/bash
# Reproducer for: ipv4: icmp: reject broadcast/multicast routes
#
# In an unpatched kernel, a crafted IPv4 packet with a strict source route
# option whose first hop is 255.255.255.255 causes __icmp_send() to generate an
# ICMP error reply addressed to that broadcast address.  This test creates a
# veth pair in a new network namespace, injects such a packet, and captures the
# reply.  If the reply is sent to 255.255.255.255, the vulnerability is present.

set -e

LOG="/mnt/shared/repro_ip_rt_bug_test.log"
REPRO_NAME="repro_veth"

{
    echo "=== IPv4 ICMP broadcast/multicast route reproducer ==="
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
    echo "=== Checking for broadcast ICMP reply ==="
    if echo "$output" | grep -q "REPRODUCED: ICMP reply sent to 255.255.255.255"; then
        echo "RESULT: VULNERABLE - kernel emits ICMP reply to 255.255.255.255"
    else
        echo "RESULT: NOT VULNERABLE (patched) - no broadcast ICMP reply emitted"
    fi

    echo ""
    echo "=== Checking dmesg for warnings ==="
    if dmesg | grep -q "WARNING:"; then
        echo "WARNINGS detected in dmesg:"
        dmesg | grep -iE "WARNING|ip_rt_bug" | head -5
    else
        echo "No dmesg warnings."
    fi
} | tee "$LOG"

exit 0
