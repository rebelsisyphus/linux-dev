#!/usr/bin/env python3
"""Guest-only CRUD, concurrent GET and dynset coverage for nft_elem_priv."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import subprocess

TABLE = 'opaque_set'
def nft(cmd, ok=True):
    p = subprocess.run(['nft', '-nn', '-f', '-'], input=cmd, text=True,
                       capture_output=True)
    assert (p.returncode == 0) == ok, (cmd, p.returncode, p.stderr)
    return p.stdout

subprocess.run(['nft', 'delete', 'table', 'ip', TABLE], capture_output=True)
try:
    nft(f'add table ip {TABLE}')
    cases = [
        ('bitmap', 'type inet_proto;', '6', '17'),
        ('hashfast', 'type ipv4_addr; size 128;', '10.1.1.1', '10.1.1.2'),
        ('hash', 'type ipv6_addr; size 128;', '2001:db8::1', '2001:db8::2'),
        ('rhash', 'type ipv4_addr; flags timeout;', '10.2.1.1', '10.2.1.2'),
        ('rbtree', 'type ipv4_addr; flags interval;', '10.3.1.1', '10.3.1.2'),
        ('pipapo', 'type ipv4_addr . inet_service; flags interval;',
         '127.0.0.1 . 20001', '127.0.0.1 . 20002'),
    ]
    for name, spec, first, second in cases:
        nft(f'add set ip {TABLE} {name} {{ {spec} }}\n'
            f'add element ip {TABLE} {name} {{ {first} }}')
        assert first in nft(f'get element ip {TABLE} {name} {{ {first} }}')
        nft(f'get element ip {TABLE} {name} {{ {second} }}', ok=False)
        nft(f'add element ip {TABLE} {name} {{ {second} }}\n'
            f'delete element ip {TABLE} {name} {{ {first} }}')
        nft(f'delete element ip {TABLE} {name} {{ {first} }}', ok=False)
        assert second in nft(f'get element ip {TABLE} {name} {{ {second} }}')
        # Abort after deactivation must reactivate the same opaque element.
        nft(f'delete element ip {TABLE} {name} {{ {second} }}\n'
            f'create set ip {TABLE} {name} {{ {spec} }}', ok=False)
        assert second in nft(f'get element ip {TABLE} {name} {{ {second} }}')
        nft(f'flush set ip {TABLE} {name}')
        assert 'elements =' not in nft(f'list set ip {TABLE} {name}')
        nft(f'delete set ip {TABLE} {name}')
        print(f'PASS: {name} insert/get/miss/delete/abort/flush/destroy', flush=True)

    def read_worker(i):
        value = f'127.0.0.1 . {21000 + i}'
        for _ in range(24):
            out = nft(f'get element ip {TABLE} concurrent{i} {{ {value} }}')
            assert value in out, (i, out)
    for i in range(4):
        nft(f'add set ip {TABLE} concurrent{i} '
            '{ type ipv4_addr . inet_service; flags interval; }\n'
            f'add element ip {TABLE} concurrent{i} {{ 127.0.0.1 . {21000+i} }}')
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(read_worker, range(4)))
    print('PASS: 96 concurrent GET requests across four PIPAPO sets', flush=True)

    nft(f'add set ip {TABLE} learned '
        '{ type ipv4_addr; flags dynamic,timeout; timeout 60s; size 128; }\n'
        f'add chain ip {TABLE} out {{ type filter hook output priority 0; }}\n'
        f'add rule ip {TABLE} out udp dport 45679 '
        'update @learned { ip daddr timeout 60s } counter')
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(32):
        s.sendto(b'opaque-dynset', ('127.0.0.1', 45679))
    s.close()
    assert '127.0.0.1' in nft(f'get element ip {TABLE} learned {{ 127.0.0.1 }}')
    assert 'counter packets 32 ' in nft(f'list chain ip {TABLE} out')
    nft(f'flush chain ip {TABLE} out\nflush set ip {TABLE} learned\n'
        f'delete set ip {TABLE} learned')
    print('PASS: dynset new and repeated update from packet path', flush=True)
    counts = [int(x) for x in Path('/sys/module/set_probe/parameters/counts').read_text().split(',')]
    assert len(counts) == 6 and all(x > 0 for x in counts), counts
    print('COVERAGE: bitmap_get,hash_get,rhash_get,rbtree_get,pipapo_get,rhash_update=' + str(counts), flush=True)
finally:
    subprocess.run(['nft', 'delete', 'table', 'ip', TABLE], capture_output=True)
