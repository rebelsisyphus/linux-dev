#!/bin/bash
# Executed by test-kernel inside the disposable guest.
set -euo pipefail
cd /mnt/shared
uname -r
[[ $(uname -r) == *rfs-bpf* ]]

echo 'Running concurrent fork/exec smoke test (8 workers, 512 execs)'
pids=()
for worker in {1..8}; do
	(for iteration in {1..64}; do /bin/true; done) &
	pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done

./bpf/rfs/test_rfs.sh

echo 'Testing pinned-link CLI attach/update/failure/detach'
ns=rfs_cli_$$
bpffs=/run/rfs_bpf_$$
pin=$bpffs/policy
tool=/mnt/shared/bpf/rfs/rfs_policy
old_entries=$(cat /proc/sys/net/core/rps_sock_flow_entries)
cleanup()
{
	[[ ! -e "$pin" ]] || "$tool" detach -p "$pin"
	ip netns del "$ns" || true
	echo "$old_entries" > /proc/sys/net/core/rps_sock_flow_entries
	if mountpoint -q "$bpffs"; then umount "$bpffs"; fi
	rmdir "$bpffs"
}
trap cleanup EXIT
# ip netns exec remounts /sys and hides bpffs mounted below /sys/fs/bpf.
mkdir "$bpffs"
mount -t bpf bpf "$bpffs"
ip netns add "$ns"
ip -n "$ns" link set lo up
echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
ip netns exec "$ns" sh -c 'echo 4096 > /sys/class/net/lo/queues/rx-0/rps_flow_cnt'
ip netns exec "$ns" "$tool" attach -i lo -s process -p "$pin"
before=$("$tool" show -p "$pin")
if ip netns exec "$ns" "$tool" attach -i lo -s numa -p "$pin"; then
	echo 'duplicate attach unexpectedly succeeded' >&2
	exit 1
fi
[[ $("$tool" show -p "$pin") == "$before" ]]
ip netns exec "$ns" "$tool" update -i lo -s cluster -c 1 --exclude-smt -p "$pin"
after=$("$tool" show -p "$pin")
[[ "$before" != "$after" && ${before%% map_id=*} == "${after%% map_id=*}" ]]
ip -n "$ns" link add rfsdummy type dummy
ip netns exec "$ns" sh -c 'echo 4096 > /sys/class/net/rfsdummy/queues/rx-0/rps_flow_cnt'
if ip netns exec "$ns" "$tool" update -i rfsdummy -s numa -p "$pin"; then
	echo 'update to a different device unexpectedly succeeded' >&2
	exit 1
fi
[[ $("$tool" show -p "$pin") == "$after" ]]
"$tool" detach -p "$pin"
[[ ! -e "$pin" ]]
cleanup
trap - EXIT

dmesg > /mnt/shared/dmesg-after-rfs.log
if grep -E 'BUG:|KASAN:|kernel BUG|possible circular locking|rcu:.*stall|WARNING: CPU:' \
	/mnt/shared/dmesg-after-rfs.log; then
	exit 1
fi
echo 'RFS native BPF tests: PASS'
