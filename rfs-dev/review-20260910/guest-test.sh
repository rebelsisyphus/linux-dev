#!/bin/bash
# Review-only validation on the isolated test-kernel VM.
set -euo pipefail
cd /mnt/shared
uname -a
./selftests/test_rfs.sh > selftests.log 2>&1
tail -4 selftests.log

ns_a=rfs-review-a
ns_b=rfs-review-b
pinroot=/run/rfs-review-bpf
old_entries=$(cat /proc/sys/net/core/rps_sock_flow_entries)
cleanup()
{
	./selftests/rfs_policy detach -p "$pinroot/old" >/dev/null 2>&1 || true
	./selftests/rfs_policy detach -p "$pinroot/new" >/dev/null 2>&1 || true
	ip netns del "$ns_a" 2>/dev/null || true
	ip netns del "$ns_b" 2>/dev/null || true
	echo "$old_entries" > /proc/sys/net/core/rps_sock_flow_entries
	umount "$pinroot" 2>/dev/null || true
	dmesg > dmesg.log
}
trap cleanup EXIT
ip netns add "$ns_a"
ip netns add "$ns_b"
ip -n "$ns_a" link add review0 index 1000 type dummy
ip -n "$ns_a" link add review1 index 1001 type dummy
mkdir -p "$pinroot"
mount -t bpf bpf "$pinroot"
echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
ip netns exec "$ns_a" sh -c 'echo 64 > /sys/class/net/review0/queues/rx-0/rps_flow_cnt'
ip netns exec "$ns_a" ./lifecycle-probe > lifecycle.log 2>&1
cat lifecycle.log
ip netns exec "$ns_a" ./selftests/rfs_policy attach -i review0 -s numa -c 1,5 -p "$pinroot/old"
ip -n "$ns_a" link set review0 netns "$ns_b"
ip netns exec "$ns_b" sh -c 'echo 64 > /sys/class/net/review0/queues/rx-0/rps_flow_cnt'
if ip netns exec "$ns_b" ./selftests/rfs_policy update -i review0 -s process -p "$pinroot/old" > moved-update.log 2>&1; then
	echo 'FAIL update after netns move unexpectedly succeeded'
	exit 1
fi
grep -q 'No such device' moved-update.log
echo 'PASS old link rejects update after device netns move: ENODEV'
./selftests/rfs_policy show -p "$pinroot/old"
ip netns exec "$ns_b" ./selftests/rfs_policy attach -i review0 -s numa -c 1,5 -p "$pinroot/new"
echo 'PASS new policy attaches to moved device'
timeout 20 ip netns del "$ns_b"
echo 'PASS netns deletion completes with pinned policy links'
./selftests/rfs_policy show -p "$pinroot/new"
ip -n "$ns_a" link add review0 index 1000 type dummy
ip netns exec "$ns_a" sh -c 'echo 64 > /sys/class/net/review0/queues/rx-0/rps_flow_cnt'
if ip netns exec "$ns_a" ./selftests/rfs_policy update -i review0 -s process -p "$pinroot/new" > reused-index.log 2>&1; then
	echo 'FAIL update on inactive link unexpectedly succeeded after ifindex reuse'
	exit 1
fi
grep -q 'No such device' reused-index.log
echo 'PASS ifindex reuse does not reactivate old link: ENODEV'
./selftests/rfs_policy detach -p "$pinroot/old"
./selftests/rfs_policy detach -p "$pinroot/new"
dmesg > dmesg.log
if grep -E 'BUG:|WARNING: CPU:|Oops:|KASAN:|rcu.*stall|possible circular locking' dmesg.log; then
	exit 1
fi
echo 'PASS no kernel failure signatures'
