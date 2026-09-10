#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# Fixed command count, clients, pipeline and warmup in every measured run.
# Reverse/rotate mode order to reduce correlation with elapsed wall time.
for round in 1 2 3; do
	case "$round" in
	1) modes='baseline rfs rps bpf-numa' ;;
	2) modes='bpf-numa rps rfs baseline' ;;
	3) modes='rps baseline bpf-numa rfs' ;;
	esac
	for mode in $modes; do
		python3 bench.py "$mode" "run$round-$mode" \
			-n 20000000 -c 384 -P 8 --warmup 500000
	done
done
