#!/bin/bash
set -euo pipefail
mode=${1:?mode required}
dev=$(cat /mnt/shared/perf-device)
pin=/run/rfs-bpf/redis-policy
policy=/mnt/shared/bin/rfs_policy
# CPU hotplug in the functional selftest makes virtio recompute affinity.
# Reassert and verify the benchmark layout after those tests.
for irq in /sys/class/net/"$dev"/device/../msi_irqs/*; do
	number=$(basename "$irq")
	echo 0 > "/proc/irq/$number/smp_affinity_list"
	[[ $(cat "/proc/irq/$number/smp_affinity_list") == 0 ]]
done
if [[ -e "$pin" ]]; then
	"$policy" detach -p "$pin"
fi
for queue in /sys/class/net/"$dev"/queues/rx-*; do
	echo 0 > "$queue/rps_flow_cnt"
	echo 0 > "$queue/rps_cpus"
done
sysctl -qw net.core.rps_sock_flow_entries=0
case "$mode" in
baseline) ;;
rfs|bpf-numa)
	sysctl -qw net.core.rps_sock_flow_entries=32768
	echo 4096 > /sys/class/net/"$dev"/queues/rx-0/rps_flow_cnt
	# Misses stay on IRQ CPU0 in both RFS modes.
	if [[ "$mode" == bpf-numa ]]; then
		"$policy" attach -i "$dev" -p "$pin" -s numa -c 5
	fi
	;;
rps)
	echo 20 > /sys/class/net/"$dev"/queues/rx-0/rps_cpus
	;;
*) echo "Unknown mode: $mode" >&2; exit 1 ;;
esac
sysctl net.core.rps_sock_flow_entries
for queue in /sys/class/net/"$dev"/queues/rx-*; do
	echo "$queue"
	cat "$queue/rps_cpus" "$queue/rps_flow_cnt"
done
if [[ -e "$pin" ]]; then
	"$policy" show -p "$pin"
fi
