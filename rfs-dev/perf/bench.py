#!/usr/bin/env python3
"""Run one controlled Redis workload against the test-kernel TAP interface."""
import argparse
import csv
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

SHARED = Path('/home/sisyphus/code/test/rfs-perf')
BIN = SHARED / 'bin'
OUT = SHARED / 'results'
SSH = ['ssh', '-o', 'LogLevel=ERROR', '-o', 'StrictHostKeyChecking=no',
       '-o', 'UserKnownHostsFile=/dev/null', '-p', '2246', 'root@localhost']

def remote(command, **kwargs):
    return subprocess.run(SSH + [command], check=True, text=True, **kwargs)

def cpu_stat(raw):
    return {f[0]: list(map(int, f[1:9])) for line in raw.splitlines()
            if (f := line.split()) and f[0].startswith('cpu')}

def delta(a, b):
    return [y - x for x, y in zip(a, b)]

def metrics(path):
    data = json.loads(path.read_text())
    samples = data['samples']
    # Client and sampler polling can add two idle seconds during shutdown.
    # Take the same central half for every mode, independent of CPU values.
    first, last = ((samples[len(samples) // 4], samples[3 * (len(samples) - 1) // 4])
                   if len(samples) >= 8 else (samples[0], samples[-1]))
    seconds = last['monotonic'] - first['monotonic']
    a, b = cpu_stat(first['files']['stat']), cpu_stat(last['files']['stat'])
    cpus = {}
    for cpu in a:
        d = delta(a[cpu], b[cpu])
        total = sum(d)
        cpus[cpu] = {'busy': 100 * (sum(d[:3]) + d[5] + d[6]) / total,
                     'user': 100 * d[0] / total,
                     'system': 100 * d[2] / total,
                     'irq': 100 * d[5] / total,
                     'softirq': 100 * d[6] / total,
                     'steal': 100 * d[7] / total}
    def proc_ticks(sample):
        # Fields after the final ')' start at field 3 (process state).
        fields = sample['files']['redis_stat'].rsplit(')', 1)[1].split()
        return int(fields[11]) + int(fields[12])
    return {'window_seconds': seconds, 'window_method': 'central_half_of_samples',
            'window_start': first['monotonic'], 'window_end': last['monotonic'],
            'redis_cpu': 100 * (proc_ticks(last) - proc_ticks(first)) / data['hz'] / seconds,
            'cpus': cpus,
            'rx_pps': (last['rx_packets'] - first['rx_packets']) / seconds}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['baseline', 'rfs', 'rps', 'bpf-numa'])
    parser.add_argument('label')
    parser.add_argument('-n', type=int, default=5000000)
    parser.add_argument('-c', type=int, default=192)
    parser.add_argument('-P', type=int, default=8)
    parser.add_argument('--warmup', type=int, default=200000)
    args = parser.parse_args()
    prefix = OUT / args.label
    if prefix.with_suffix('.json').exists():
        raise SystemExit('Refusing to overwrite an existing result')
    with prefix.with_suffix('.mode.txt').open('w') as out:
        remote('bash /mnt/shared/set-mode.sh ' + shlex.quote(args.mode), stdout=out)
    command = ['taskset', '-c', '6,8,10', str(BIN / 'redis-benchmark'),
               '-h', '192.0.2.2', '--threads', '3', '-c', str(args.c),
               '-P', str(args.P), '-n', str(args.n), '--csv', 'GET', 'rfs-key']
    warmup = command.copy()
    warmup[warmup.index('-n') + 1] = str(args.warmup)
    with prefix.with_suffix('.warmup.csv').open('w') as out:
        subprocess.run(warmup, check=True, stdout=out, timeout=60)
    # IRQ migration can be deferred until the first interrupt after the write.
    with Path(str(prefix) + '.irq-affinity.txt').open('w') as out:
        remote('set -e; for irq in /sys/class/net/$(cat /mnt/shared/perf-device)/device/../msi_irqs/*; '
               'do n=$(basename "$irq"); cpu=$(cat /proc/irq/$n/effective_affinity_list); '
               'echo "$n: $cpu"; '
               'if grep -qE "^[ ]*$n:.*virtio.*(input|output)" /proc/interrupts; '
               'then test "$cpu" = 0; fi; done', stdout=out)
    with Path(str(prefix) + '.info-before.txt').open('w') as out:
        subprocess.run([str(BIN / 'redis-cli'), '-h', '192.0.2.2', 'INFO', 'ALL'],
                       check=True, stdout=out)
    guest_path = str(prefix) + '.guest.json'
    monitor = subprocess.Popen(SSH + ['taskset -c 2 python3 /mnt/shared/snapshot.py '
                                      '/mnt/shared/results/' + args.label + '.guest.json'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    ready = Path(guest_path + '.ready')
    deadline = time.monotonic() + 15
    while not ready.exists():
        if monitor.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError('Guest monitor failed: ' + str(monitor.communicate(timeout=5)))
        time.sleep(.1)
    host_samples = []
    vm = Path((SHARED / 'vm-directory.txt').read_text().strip())
    qemu_pid = int((vm / 'qemu.pid').read_text())
    try:
        with prefix.with_suffix('.csv').open('w') as out, prefix.with_suffix('.stderr').open('w') as err:
            bench = subprocess.Popen(command, stdout=out, stderr=err)
            start = time.monotonic()
            while bench.poll() is None:
                host_samples.append({'time': time.monotonic(),
                                     'stat': Path('/proc/stat').read_text(),
                                     'client_stat': Path(f'/proc/{bench.pid}/stat').read_text(),
                                     'qemu_tasks': {task.name: (task / 'stat').read_text()
                                                    for task in Path(f'/proc/{qemu_pid}/task').iterdir()}})
                if time.monotonic() - start > 180:
                    bench.kill()
                    raise RuntimeError('Benchmark exceeded 180 seconds')
                time.sleep(1)
            if bench.returncode:
                raise RuntimeError('redis-benchmark failed')
    finally:
        Path(guest_path + '.stop').touch()
        monitor.communicate(timeout=15)
    if monitor.returncode:
        raise RuntimeError('Guest sampler failed')
    with Path(str(prefix) + '.info-after.txt').open('w') as out:
        subprocess.run([str(BIN / 'redis-cli'), '-h', '192.0.2.2', 'INFO', 'ALL'],
                       check=True, stdout=out)
    rows = list(csv.DictReader(io.StringIO(prefix.with_suffix('.csv').read_text())))
    result = {'mode': args.mode, 'label': args.label, 'command': command,
              'csv': rows, 'metrics': metrics(Path(guest_path)), 'host_samples': host_samples}
    prefix.with_suffix('.json').write_text(json.dumps(result, indent=2))
    m = result['metrics']
    print(json.dumps({'label': args.label, 'csv': rows,
                      'redis_cpu': round(m['redis_cpu'], 2),
                      'busy': {c: round(m['cpus'][c]['busy'], 2) for c in ['cpu0', 'cpu4', 'cpu5']},
                      'rx_pps': round(m['rx_pps'])}), flush=True)

if __name__ == '__main__':
    main()
