#!/usr/bin/env python3
"""Guest-only regression for fb7fb4016300 and old set backends."""
import json
from pathlib import Path
import subprocess
import time

PARAM = Path('/sys/module/clone_probe/parameters')
TABLE = 'clone_flush'

def nft(text, ok=True):
    proc = subprocess.run(['nft', '-f', '-'], input=text, text=True,
                          capture_output=True)
    assert (proc.returncode == 0) == ok, (text, proc.returncode, proc.stderr)
    return proc

def counts():
    return {name: int((PARAM / name).read_text()) for name in
            ['arm', 'clones', 'failures', 'reads', 'updates', 'flushes']}

def arm(value):
    (PARAM / 'arm').write_text(str(value))

def setup():
    arm(0)
    subprocess.run(['nft', 'delete', 'table', 'ip', TABLE], capture_output=True)
    nft(f'add table ip {TABLE}\n'
        f'add chain ip {TABLE} target\n'
        f'add map ip {TABLE} m {{ type ipv4_addr . inet_service : verdict; '
        'flags interval; }\n'
        f'add element ip {TABLE} m {{ 10.0.0.1 . 80 : jump target }}')

def present():
    assert '10.0.0.1 . 80' in nft(f'list map ip {TABLE} m').stdout

Path('/sys/kernel/debug/failslab/ignore-gfp-wait').write_text('0')
Path('/sys/kernel/debug/failslab/verbose').write_text('0')

try:
    setup()
    before = counts()
    arm(1)
    nft(f'delete map ip {TABLE} m\ndelete chain ip {TABLE} target')
    after = counts()
    assert after['updates'] > before['updates'], (before, after)
    assert after['clones'] == before['clones'] and after['arm'] == 1
    print('PASS: deleting committed map requires no clone; chain references balance', flush=True)

    setup()
    before = counts()
    arm(1)
    nft(f'delete table ip {TABLE}')
    after = counts()
    assert after['updates'] > before['updates'], (before, after)
    assert after['clones'] == before['clones'] and after['arm'] == 1
    print('PASS: deleting table with verdict map requires no clone', flush=True)

    setup()
    before = counts()
    arm(1)
    nft(f'delete map ip {TABLE} m\ncreate chain ip {TABLE} target', ok=False)
    after = counts()
    assert after['updates'] >= before['updates'] + 2, (before, after)
    assert after['clones'] == before['clones'] and after['arm'] == 1
    present()
    arm(0)
    nft(f'delete map ip {TABLE} m\ndelete chain ip {TABLE} target')
    print('PASS: abort reactivates committed map without clone and preserves references', flush=True)

    setup()
    before = counts()
    arm(1)
    proc = nft(f'flush map ip {TABLE} m', ok=False)
    assert 'memory' in proc.stderr.lower(), proc.stderr
    after = counts()
    assert after['clones'] == before['clones'] + 1, (before, after)
    assert after['failures'] == before['failures'] + 1 and after['arm'] == 0
    assert after['flushes'] == before['flushes'] + 1
    present()
    nft(f'flush map ip {TABLE} m')
    assert 'elements =' not in nft(f'list map ip {TABLE} m').stdout
    nft(f'delete chain ip {TABLE} target')
    print('PASS: flush clone ENOMEM rolls back; next flush succeeds and releases references', flush=True)

    setup()
    before = counts()
    nft(f'add element ip {TABLE} m {{ 10.0.0.2 . 81 : jump target }}\n'
        f'delete map ip {TABLE} m\ndelete chain ip {TABLE} target')
    after = counts()
    assert after['clones'] == before['clones'] + 1, (before, after)
    assert after['updates'] > before['updates']
    print('PASS: map deletion after insertion reuses existing clone', flush=True)

    setup()
    before = counts()
    nft(f'add chain ip {TABLE} out {{ type filter hook output priority 0; }}\n'
        f'add rule ip {TABLE} out ip daddr . tcp dport vmap '
        '{ 10.0.0.1 . 80-81 : jump target }')
    added = counts()
    assert added['updates'] > before['updates'], (before, added)
    before = counts()
    arm(1)
    nft(f'flush chain ip {TABLE} out')
    after = counts()
    assert after['updates'] > before['updates']
    assert after['clones'] == before['clones'] and after['arm'] == 1
    print('PASS: anonymous interval verdict-map removal requires no clone', flush=True)

    arm(0)
    ns = 'clone_flush_ns'
    subprocess.run(['ip', 'netns', 'add', ns], check=True)
    try:
        subprocess.run(['ip', 'netns', 'exec', ns, 'nft', '-f', '-'],
                       input='add table ip t\nadd chain ip t target\n'
                       'add map ip t m { type ipv4_addr . inet_service : verdict; '
                       'flags interval; }\n'
                       'add element ip t m { 10.0.0.1 . 80 : jump target }',
                       text=True, check=True)
        before = counts()
        arm(1)
    finally:
        subprocess.run(['ip', 'netns', 'del', ns], check=True)
    for _ in range(100):
        if counts()['updates'] > before['updates']:
            break
        time.sleep(0.1)
    after = counts()
    assert after['updates'] > before['updates'], (before, after)
    assert after['clones'] == before['clones'] and after['arm'] == 1
    print('PASS: namespace teardown deactivates live map without clone', flush=True)

    arm(0)
    nft(f'add set ip {TABLE} hashset {{ type ipv4_addr; flags timeout; }}\n'
        f'add element ip {TABLE} hashset {{ 10.1.1.1 }}\n'
        f'add set ip {TABLE} tree {{ type ipv4_addr; flags interval; }}\n'
        f'add element ip {TABLE} tree {{ 10.1.1.0/24 }}\n'
        f'add set ip {TABLE} bitmap {{ type inet_proto; }}\n'
        f'add element ip {TABLE} bitmap {{ tcp, udp }}')
    for name in ['hashset', 'tree', 'bitmap']:
        nft(f'flush set ip {TABLE} {name}')
        assert 'elements =' not in nft(f'list set ip {TABLE} {name}').stdout
    print('PASS: legacy hash/rbtree/bitmap flush remains functional', flush=True)
    print('COUNTERS ' + json.dumps(counts(), sort_keys=True), flush=True)
finally:
    arm(0)
    subprocess.run(['nft', 'delete', 'table', 'ip', TABLE], capture_output=True)
