#!/bin/bash
set -euo pipefail
export PATH=/mnt/shared/bin:$PATH
export LD_LIBRARY_PATH=/mnt/shared/numa/usr/lib/x86_64-linux-gnu
if [[ -e /proc/sys/kernel/numa_balancing ]]; then
	sysctl -qw kernel.numa_balancing=0
fi
sysctl -qw vm.overcommit_memory=1
if [[ -e /sys/kernel/mm/transparent_hugepage/enabled ]]; then
	echo never > /sys/kernel/mm/transparent_hugepage/enabled
fi
if [[ -s /run/redis-rfs.pid ]] && kill -0 "$(cat /run/redis-rfs.pid)" 2>/dev/null; then
	redis-cli -h 192.0.2.2 shutdown nosave
fi
cat > /run/redis-rfs.conf <<'EOF'
bind 192.0.2.2
port 6379
protected-mode no
save ""
appendonly no
io-threads 1
io-threads-do-reads no
daemonize yes
pidfile /run/redis-rfs.pid
logfile /run/redis-rfs.log
dir /run
EOF
/mnt/shared/numa/usr/bin/numactl --physcpubind=4 --membind=1 \
	redis-server /run/redis-rfs.conf
for n in $(seq 1 20); do
	if redis-cli -h 192.0.2.2 ping; then
		break
	fi
	sleep 0.1
done
redis-cli -h 192.0.2.2 set rfs-key "$(printf '%064d' 0)"
taskset -pc "$(cat /run/redis-rfs.pid)"
cat /proc/"$(cat /run/redis-rfs.pid)"/numa_maps
cat /run/redis-rfs.conf
