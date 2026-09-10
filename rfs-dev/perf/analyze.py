#!/usr/bin/env python3
"""Validate and summarize the predeclared twelve timed runs."""
import csv
import json
from pathlib import Path
import statistics
from bench import OUT, cpu_stat, delta, metrics

def info(path):
    return dict(line.split(':', 1) for line in path.read_text().splitlines()
                if line and not line.startswith('#') and ':' in line)

def irqs(raw):
    rows = {}
    for line in raw.splitlines():
        if 'virtio' in line and ('-input.' in line or '-output.' in line):
            fields = line.split()
            rows[fields[-1]] = list(map(int, fields[1:9]))
    return rows

def proc_ticks(raw):
    fields = raw.rsplit(')', 1)[1].split()
    return int(fields[11]) + int(fields[12])

rows = []
for round_ in (1, 2, 3):
    for mode in ('baseline', 'rfs', 'rps', 'bpf-numa'):
        label = f'run{round_}-{mode}'
        prefix = OUT / label
        d = json.loads(prefix.with_suffix('.json').read_text())
        if d['metrics'].get('window_method') != 'central_half_of_samples':
            d['initial_window_metrics'] = d['metrics']
            d['metrics'] = metrics(Path(str(prefix) + '.guest.json'))
            prefix.with_suffix('.json').write_text(json.dumps(d, indent=2))
        before = info(Path(str(prefix) + '.info-before.txt'))
        after = info(Path(str(prefix) + '.info-after.txt'))
        get_a = int(before.get('cmdstat_get', 'calls=0,').split(',')[0].split('=')[1])
        get_b = int(after['cmdstat_get'].split(',')[0].split('=')[1])
        commands = get_b - get_a
        misses = int(after['keyspace_misses']) - int(before['keyspace_misses'])
        errors = int(after['total_error_replies']) - int(before['total_error_replies'])
        assert 20000000 <= commands <= 20000000 + 384 * 8, (label, commands)
        assert misses == errors == 0, (label, misses, errors)
        g = json.loads(Path(str(prefix) + '.guest.json').read_text())
        first, last = g['samples'][0], g['samples'][-1]
        irq_a, irq_b = irqs(first['files']['interrupts']), irqs(last['files']['interrupts'])
        irq_d = {name: delta(irq_a[name], irq_b[name]) for name in irq_a}
        assert irq_d and all(sum(v) > 0 and sum(v[1:]) == 0 for v in irq_d.values()), (label, irq_d)
        cpu = d['metrics']['cpus']
        assert cpu['cpu4']['busy'] >= 98, (label, cpu['cpu4'])
        host_a, host_b = d['host_samples'][1], d['host_samples'][-2]
        host_seconds = host_b['time'] - host_a['time']
        host_cpus = {}
        for name, va in cpu_stat(host_a['stat']).items():
            vb = cpu_stat(host_b['stat'])[name]
            v = delta(va, vb)
            host_cpus[name] = 100 * (sum(v[:3]) + v[5] + v[6]) / sum(v)
        client_cpu = 100 * (proc_ticks(host_b['client_stat']) - proc_ticks(host_a['client_stat'])) / 100 / host_seconds
        item = {'round': round_, 'mode': mode, 'qps': float(d['csv'][0]['rps']),
                'p99_ms': float(d['csv'][0]['p99_latency_ms']),
                'redis_cpu': d['metrics']['redis_cpu'],
                'cpu0_busy': cpu['cpu0']['busy'], 'cpu4_busy': cpu['cpu4']['busy'],
                'cpu5_busy': cpu['cpu5']['busy'],
                'cpu4_softirq': cpu['cpu4']['softirq'],
                'rx_pps': d['metrics']['rx_pps'],
                'commands_processed': commands, 'misses': misses, 'errors': errors,
                'irq_deltas': irq_d, 'client_cpu': client_cpu,
                'host_cpu12_busy': host_cpus['cpu12']}
        rows.append(item)
        print(json.dumps(item))

summary = {}
for mode in ('baseline', 'rfs', 'rps', 'bpf-numa'):
    group = [row for row in rows if row['mode'] == mode]
    summary[mode] = {key: statistics.median(row[key] for row in group)
                     for key in ('qps', 'p99_ms', 'redis_cpu', 'cpu0_busy', 'cpu4_busy',
                                 'cpu5_busy', 'cpu4_softirq', 'rx_pps', 'client_cpu', 'host_cpu12_busy')}
    summary[mode]['qps_min'] = min(row['qps'] for row in group)
    summary[mode]['qps_max'] = max(row['qps'] for row in group)
base = summary['baseline']['qps']
for mode in summary:
    summary[mode]['vs_baseline_percent'] = 100 * (summary[mode]['qps'] / base - 1)
result = {'runs': rows, 'median': summary,
          'bpf_vs_rfs_percent': 100 * (summary['bpf-numa']['qps'] / summary['rfs']['qps'] - 1),
          'bpf_vs_rps_percent': 100 * (summary['bpf-numa']['qps'] / summary['rps']['qps'] - 1)}
(OUT / 'summary.json').write_text(json.dumps(result, indent=2))
with (OUT / 'summary.csv').open('w') as out:
    flat = [{k: v for k, v in row.items() if k != 'irq_deltas'} for row in rows]
    writer = csv.DictWriter(out, fieldnames=flat[0].keys())
    writer.writeheader()
    writer.writerows(flat)
print(json.dumps(result['median'], indent=2))
