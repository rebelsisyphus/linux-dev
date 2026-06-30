# `tcp_recvmsg_locked()` WARNING reproduction

## Environment

- Kernel: `7.1.0-rc5-00151-g78aa5e823bc3-dirty`
- VM: QEMU x86_64
- Test: `test_progs -t 'sockmap_basic/drop after partial read'`
- Kernel state: commit `1e5744162c4d` reverted locally so that
  `tcp_eat_skb()` used `copied_seq + skb->len`

The test performs a native partial read, inserts the TCP socket into a
sockmap with an `SK_DROP` verdict, sends data to run the verdict, removes
the socket from the map, and invokes `TCP_ZEROCOPY_RECEIVE` after sending
new native data.

The console log level did not emit the warning to `serial.log`. The test
therefore cleared the guest kernel ring buffer before running and exported
`dmesg` through the QEMU shared directory.

## Reproduction result

The selftest failed as expected:

```text
test_sockmap_drop_after_partial_read:PASS:xsend(native again) 0 nsec
test_sockmap_drop_after_partial_read:PASS:setsockopt(SO_ZEROCOPY) 0 nsec
test_sockmap_drop_after_partial_read:PASS:getsockopt(TCP_ZEROCOPY_RECEIVE) 0 nsec
test_sockmap_drop_after_partial_read:FAIL:recv_timeout(native again) unexpected recv_timeout(native again): actual -1 != expected 1
#1/34    sockmap_basic/sockmap drop after partial read:FAIL
Summary: 0/0 PASSED, 0 SKIPPED, 1 FAILED
```

The exported guest `dmesg` contains the target warning:

```text
[   14.208832] ------------[ cut here ]------------
[   14.208953] TCP recvmsg seq # bug 2: copied AA28C633, seq AA28C601, rcvnxt AA28C602, fl 40
[   14.209031] WARNING: net/ipv4/tcp.c:2745 at tcp_recvmsg_locked+0x45e/0x9f0, CPU#1: test_progs/307
[   14.209469] Modules linked in:
[   14.209679] CPU: 1 UID: 0 PID: 307 Comm: test_progs Not tainted 7.1.0-rc5-00151-g78aa5e823bc3-dirty #237 PREEMPT(lazy)
[   14.209712] Hardware name: QEMU Standard PC (Q35 + ICH9, 2009), BIOS 1.16.3-debian-1.16.3-2 04/01/2014
[   14.209764] RIP: 0010:tcp_recvmsg_locked+0x46f/0x9f0
[   14.209977] Call Trace:
[   14.210412]  <TASK>
[   14.210612]  tcp_zerocopy_receive+0x4d0/0xa10
[   14.210718]  do_tcp_getsockopt+0x52e/0x10e0
[   14.210844]  tcp_getsockopt+0x33/0x40
[   14.210878]  do_sock_getsockopt+0x3b5/0x460
[   14.210954]  __sys_getsockopt+0x72/0xb0
[   14.210966]  __x64_sys_getsockopt+0x1a/0x30
[   14.210981]  do_syscall_64+0xf9/0x540
[   14.210995]  entry_SYSCALL_64_after_hwframe+0x77/0x7f
[   14.211636]  </TASK>
[   14.211673] ---[ end trace 0000000000000000 ]---
```

A second warning confirms the same corrupted sequence state during receive
buffer cleanup:

```text
[   14.211840] cleanup rbuf bug: copied AA28C633 seq AA28C602 rcvnxt AA28C602
[   14.211846] WARNING: net/ipv4/tcp.c:1609 at tcp_cleanup_rbuf+0x37/0x50, CPU#1: test_progs/307
[   14.212015]  <TASK>
[   14.212019]  tcp_recvmsg_locked+0x1cf/0x9f0
[   14.212031]  tcp_zerocopy_receive+0x4d0/0xa10
[   14.212051]  do_tcp_getsockopt+0x52e/0x10e0
[   14.212268]  tcp_getsockopt+0x33/0x40
[   14.212292]  do_sock_getsockopt+0x3b5/0x460
[   14.212316]  __sys_getsockopt+0x72/0xb0
[   14.212324]  __x64_sys_getsockopt+0x1a/0x30
[   14.212330]  do_syscall_64+0xf9/0x540
[   14.212338]  entry_SYSCALL_64_after_hwframe+0x77/0x7f
[   14.212398]  </TASK>
```

The values show `copied_seq` is 49 bytes beyond `rcv_nxt`, matching the
50-byte partial read with one newly received byte.

## Artifacts

- `sockmapsyz/warning-repro-dmesg.log`: complete guest `dmesg`
- `sockmapsyz/warning-repro-test.log`: complete selftest output

## Fixed-kernel verification

With commit `1e5744162c4d` restored, the same test including the
`TCP_ZEROCOPY_RECEIVE` operation passes:

```text
#1/34    sockmap_basic/sockmap drop after partial read:OK
#1       sockmap_basic:OK
Summary: 1/1 PASSED, 0 SKIPPED, 0 FAILED
```

The guest `dmesg` captured after the fixed-kernel run contains no WARNING.
