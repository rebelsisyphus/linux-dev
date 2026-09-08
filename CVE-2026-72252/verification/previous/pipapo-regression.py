#!/usr/bin/env python3
"""Guest-only PIPAPO regression; pipapo_fail.ko targets scratch allocation."""
import ipaddress
import pathlib
import socket
import subprocess
import time

PARAM = pathlib.Path('/sys/module/pipapo_fail/parameters')

def nft(text, ok=True):
    p = subprocess.run(['nft', '-f', '-'], input=text, text=True, capture_output=True)
    if ok:
        assert p.returncode == 0, (text[:160], p.stderr)
    else:
        assert p.returncode != 0 and 'memory' in p.stderr.lower(), (p.returncode, p.stderr)
    return p.stdout

def count(name):
    return int((PARAM / name).read_text())

def arm(n):
    (PARAM / 'arm').write_text(str(n))

def fresh():
    subprocess.run(['nft', 'delete', 'table', 'ip', 'cve72252'], capture_output=True)
    nft('add table ip cve72252\nadd set ip cve72252 s { type ipv4_addr . inet_service; flags interval; }')

def empty():
    assert 'elements =' not in nft('list set ip cve72252 s')

def recovery():
    nft('add element ip cve72252 s { 127.0.0.1 . 45678 }')
    assert '127.0.0.1 . 45678' in nft('get element ip cve72252 s { 127.0.0.1 . 45678 }')
    nft('add chain ip cve72252 out { type filter hook output priority 0; policy accept; }\n'
        'add rule ip cve72252 out ip daddr . udp dport @s counter')
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(b'pipapo', ('127.0.0.1', 45678))
    s.close()
    assert 'counter packets 1 ' in nft('list chain ip cve72252 out')
    nft('delete element ip cve72252 s { 127.0.0.1 . 45678 }')
    empty()
    nft('add element ip cve72252 s { 127.0.0.1 . 45678 }\nflush set ip cve72252 s')
    empty()

pathlib.Path('/sys/kernel/debug/failslab/ignore-gfp-wait').write_text('0')
pathlib.Path('/sys/kernel/debug/failslab/verbose').write_text('0')
try:
    for i in range(8):
        fresh()
        fired = count('fired')
        arm(1)
        nft('add element ip cve72252 s { 10.1.0.1 . 1000 }', ok=False)
        assert count('fired') == fired + 1
        arm(0)
        empty()
        recovery()
    print('PASS: 8 NEW-clone failures after field insertion; query, packet lookup, delete, flush recover', flush=True)
    entries = ', '.join(f'{ipaddress.IPv4Address(0x0a010000+i)} . {2000+i}' for i in range(1, 1025))
    for i in range(4):
        fresh()
        fired = count('fired')
        arm(2)
        nft('add element ip cve72252 s { 10.0.0.1 . 1000 }\n'
            f'add element ip cve72252 s {{ {entries} }}\n'
            'add element ip cve72252 s { 10.2.0.1 . 9999 }\n'
            'delete element ip cve72252 s { 10.0.0.1 . 1000 }', ok=False)
        assert count('fired') == fired + 1
        arm(0)
        empty()
        recovery()
    print('PASS: 4 MOD-clone failures with prior transaction records; rollback and next transaction recover', flush=True)
    fresh()
    nft('delete set ip cve72252 s\nadd set ip cve72252 s { type ipv4_addr . inet_service; flags interval,timeout; timeout 100ms; gc-interval 100ms; }')
    nft('add element ip cve72252 s { 10.0.0.1 . 1000 }')
    time.sleep(0.3)
    nft('add element ip cve72252 s { 10.0.0.2 . 1001 }')
    output = nft('list set ip cve72252 s')
    assert '10.0.0.1 . 1000' not in output
    nft('flush set ip cve72252 s')
    print('PASS: timeout expiry and subsequent GC/flush', flush=True)
finally:
    arm(0)
    subprocess.run(['nft', 'delete', 'table', 'ip', 'cve72252'], capture_output=True)
print(f'PASS: targeted injection hits={count("hits")} failures={count("fired")}', flush=True)
