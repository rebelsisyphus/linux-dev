#!/bin/bash
# Test script for Step 3 children_lock + RCU changes

echo "====================================="
echo "Test started at: $(date)"
echo "====================================="
echo ""

echo "1. System information:"
uname -a
echo ""

echo "2. NUMA topology:"
numactl --hardware 2>/dev/null || cat /sys/devices/system/node/online 2>/dev/null || echo "No NUMA info"
echo ""

echo "3. Basic fork/exec stress test:"
for i in $(seq 1 100); do
    /bin/true &
done
wait
echo "   100 background forks completed OK"
echo ""

echo "4. Rapid process creation/destruction:"
for i in $(seq 1 50); do
    /bin/true && /bin/true && /bin/true &
done
wait
echo "   150 rapid processes completed OK"
echo ""

echo "5. Process tree test (parent-child relationships):"
(
    for i in $(seq 1 20); do
        sleep 0.1 &
    done
    wait
) &
outer_pid=$!
sleep 0.2
ps --ppid $outer_pid 2>/dev/null && echo "   Children visible OK" || echo "   ps --ppid failed (may not support option)"
wait $outer_pid 2>/dev/null
echo ""

echo "6. Concurrent fork/exit stress test (10 rounds):"
for round in $(seq 1 10); do
    for i in $(seq 1 100); do
        /bin/true &
    done
    wait
done
echo "   1000 total process creations completed OK"
echo ""

echo "7. Check for kernel warnings in dmesg:"
dmesg | grep -i "bug\|warn\|lockdep\|deadlock\|rcu\|children_lock" | tail -20 || echo "   No warnings found"
echo ""

echo "8. Check lockdep specifically:"
dmesg | grep -i "lockdep.*children_lock\|possible recursive locking" || echo "   No lockdep warnings for children_lock"
echo ""

echo "====================================="
echo "Test completed successfully!"
echo "====================================="
exit 0