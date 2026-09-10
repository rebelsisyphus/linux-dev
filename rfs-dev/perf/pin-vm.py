#!/usr/bin/env python3
"""Pin only this benchmark VM, giving the hot CPUs separate host cores."""
import json
import os
from pathlib import Path
import re
import time

shared = Path('/home/sisyphus/code/test/rfs-perf')
vm = Path((shared / 'vm-directory.txt').read_text().strip())
pid = int((vm / 'qemu.pid').read_text())
assert b'-name\x00rfs-bpf-test\x00' in Path(f'/proc/{pid}/cmdline').read_bytes()
mapping = {0: 0, 1: 14, 2: 14, 3: 15, 4: 2, 5: 4, 6: 15, 7: 14}
records = []
vcpu_fds = {}
for fd in Path(f'/proc/{pid}/fd').iterdir():
    match = re.fullmatch(r'anon_inode:kvm-vcpu:(\d+)', str(fd.readlink()))
    if match:
        vcpu_fds[int(fd.name)] = int(match[1])
vcpu_threads = {}
# QEMU need not have debug thread names enabled. Resolve KVM_RUN's fd.
for attempt in range(50):
    for task in Path(f'/proc/{pid}/task').iterdir():
        call = (task / 'syscall').read_text().split()
        if len(call) >= 3 and call[0] == '16' and int(call[2], 16) == 0xae80:
            fd = int(call[1], 16)
            if fd in vcpu_fds:
                vcpu_threads[int(task.name)] = vcpu_fds[fd]
    if set(vcpu_threads.values()) == set(mapping):
        break
    time.sleep(.1)
assert set(vcpu_threads.values()) == set(mapping), vcpu_threads
for task in Path(f'/proc/{pid}/task').iterdir():
    name = (task / 'comm').read_text().strip()
    vcpu = vcpu_threads.get(int(task.name))
    affinity = {mapping[vcpu]} if vcpu is not None else {14, 15}
    if name.startswith('vhost-'):
        affinity = {12}
    old = sorted(os.sched_getaffinity(int(task.name)))
    os.sched_setaffinity(int(task.name), affinity)
    records.append({'tid': int(task.name), 'name': name, 'vcpu': vcpu,
                    'before': old, 'after': sorted(affinity)})
for task in Path('/proc').iterdir():
    if not task.name.isdigit():
        continue
    try:
        name = (task / 'comm').read_text().strip()
        if name != f'vhost-{pid}':
            continue
        old = sorted(os.sched_getaffinity(int(task.name)))
        os.sched_setaffinity(int(task.name), {12})
        records.append({'tid': int(task.name), 'name': name,
                        'before': old, 'after': [12]})
    except FileNotFoundError:
        pass
print(json.dumps({'qemu_pid': pid, 'threads': records}, indent=2))
