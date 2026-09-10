#!/usr/bin/env python3
"""Guest CPU and network evidence; keep sampling on an otherwise idle CPU."""
import json
import os
from pathlib import Path
import sys
import time

output = Path(sys.argv[1])
pid = int(Path('/run/redis-rfs.pid').read_text())
dev = Path('/mnt/shared/perf-device').read_text().strip()
samples = []

def snapshot():
    files = {name: Path('/proc', name).read_text() for name in
             ('stat', 'interrupts', 'softirqs', 'net/softnet_stat',
              'net/snmp', 'net/netstat')}
    files['redis_stat'] = Path(f'/proc/{pid}/stat').read_text()
    files['uptime'] = Path('/proc/uptime').read_text()
    return {'monotonic': time.monotonic(), 'files': files,
            'rx_packets': int(Path(f'/sys/class/net/{dev}/statistics/rx_packets').read_text()),
            'tx_packets': int(Path(f'/sys/class/net/{dev}/statistics/tx_packets').read_text())}

samples.append(snapshot())
Path(str(output) + '.ready').touch()
while not Path(str(output) + '.stop').exists():
    time.sleep(1)
    samples.append(snapshot())
samples.append(snapshot())
output.write_text(json.dumps({'hz': os.sysconf('SC_CLK_TCK'),
                             'pid': pid, 'samples': samples}, indent=2))
