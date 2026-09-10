#!/usr/bin/env python3
"""Separate, deliberately instrumented validation; never a QPS result."""
import json
from pathlib import Path
import subprocess
import time
from bench import BIN, OUT, SSH, remote

for mode in ('baseline', 'rfs', 'rps', 'bpf-numa'):
    prefix = OUT / ('verify-' + mode)
    with prefix.with_suffix('.mode.txt').open('w') as output:
        remote('bash /mnt/shared/set-mode.sh ' + mode, stdout=output)
    command = ['taskset', '-c', '6,8,10', str(BIN / 'redis-benchmark'),
               '-h', '192.0.2.2', '--threads', '3', '-c', '384', '-P', '8',
               '-n', '100000000', '--csv', 'GET', 'rfs-key']
    with prefix.with_suffix('.client.txt').open('w') as output:
        load = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            time.sleep(2)
            if load.poll() is not None:
                raise RuntimeError('Diagnostic load exited unexpectedly')
            with prefix.with_suffix('.interrupts-before.txt').open('w') as out:
                remote('cat /proc/interrupts', stdout=out)
            with prefix.with_suffix('.rx.json').open('w') as out, prefix.with_suffix('.trace.log').open('w') as err:
                remote('/mnt/shared/bin/rx_cpu /mnt/shared/bin/rx_cpu.bpf.o 5', stdout=out, stderr=err)
            with prefix.with_suffix('.interrupts-after.txt').open('w') as out:
                remote('cat /proc/interrupts', stdout=out)
            if mode == 'bpf-numa':
                with prefix.with_suffix('.policy-stats.json').open('w') as out:
                    remote('/mnt/shared/bin/bpftool -j map dump name stats', stdout=out)
        finally:
            load.terminate()
            load.wait(timeout=10)
    with prefix.with_suffix('.programs-after.json').open('w') as out:
        remote('/mnt/shared/bin/bpftool -j prog show', stdout=out)
    counts = json.loads(prefix.with_suffix('.rx.json').read_text())
    expected = {'baseline': 0, 'rfs': 4, 'rps': 5, 'bpf-numa': 5}[mode]
    total = sum(counts)
    result = {'mode': mode, 'tcp_rx_skb_counts': counts, 'expected_cpu': expected,
              'on_expected_percent': 100 * counts[expected] / total if total else 0}
    prefix.with_suffix('.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result), flush=True)
    if not total or result['on_expected_percent'] < 99:
        raise RuntimeError('TCP receive CPU validation failed')
