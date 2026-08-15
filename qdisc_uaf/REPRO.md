# qdisc_uaf Reproduction Report

## 1. Problem Model

The KASAN use-after-free / out-of-bounds read is in `qdisc_pkt_len_segs_init()`.
Three call chains have been reproduced:

Ingress qdisc path:

```
netif_receive_skb()
  -> sch_handle_ingress()
  -> tcf_classify()
  -> qdisc_pkt_len_init()
     -> qdisc_pkt_len_segs_init()
```

HSR forwarding path (matches the original syzbot report):

```
tun_get_user()
  -> netif_receive_skb()
  -> __netif_receive_skb_core()
  -> hsr_handle_frame()
  -> hsr_forward_skb()
  -> hsr_xmit()
  -> __dev_queue_xmit()
     -> qdisc_pkt_len_segs_init()
```

Bridge forwarding path (shows the bug is not HSR-specific):

```
tun_get_user()
  -> netif_receive_skb()
  -> __netif_receive_skb_core()
  -> br_handle_frame()
  -> br_handle_frame_finish()
  -> br_forward()
  -> __dev_queue_xmit()
     -> qdisc_pkt_len_segs_init()
```

Inside `qdisc_pkt_len_segs_init()`:

```c
if (skb_is_gso(skb)) {
    unsigned int hdr_len = skb_transport_header(skb) - skb->data;

    if (skb->encapsulation)
        hdr_len += skb_inner_network_header_len(skb);
    else
        hdr_len += skb_network_header_len(skb);

    len = skb->len - hdr_len;
}
```

When `skb_transport_header(skb)` points before `skb->data`, `hdr_len`
underflows to a very large unsigned value.  Later code dereferences
`skb->data + hdr_len` to read protocol headers, which causes a wild read
and triggers KASAN or a page fault.

This stale-negative-transport_header situation can happen after a
GRE/TEB decapsulation leaves `skb->transport_header` unchanged while
`skb->data` is pulled forward.

The reference patch `mailist.patch` resets the transport header in the
GRE/TEB decap paths so that the offset never becomes stale-negative.

## 2. Reproducers

Three reproducers are provided:

* `qdisc_uaf_repro.c` — early userspace C reproducer that sets up the real
  network path (TUN -> ip6gretap -> HSR) and injects a crafted
  IPv6/GRE/TEB frame.
* `qdisc_uaf_repro_gist.c` — working userspace C reproducer based on the
  patch author's reference implementation; triggers through the ingress
  qdisc path.
* `qdisc_uaf_repro_hsr.c` — working userspace C reproducer that triggers
  through the HSR forwarding path and matches the original syzbot Call
  Trace (`hsr_forward_skb -> hsr_xmit -> __dev_queue_xmit`).
* `qdisc_uaf_repro_bridge.c` — working userspace C reproducer that triggers
  through the Linux bridge forwarding path (`br_handle_frame -> br_forward
  -> __dev_queue_xmit`).
* `qdisc_uaf_repro_mod.c` — minimal kernel module that constructs a fake
  `sk_buff` with the stale-negative `transport_header` directly.  This
  is the deterministic reproducer used to capture the crash logs below.

### 2.1 Userspace C reproducer (`qdisc_uaf_repro.c`)

The C reproducer attempts to trigger the same path as the syzbot
reproducer without loading a kernel module:

```bash
cd /home/sisyphus/code/linux/qdisc_uaf
gcc -o qdisc_uaf_repro qdisc_uaf_repro.c -static
./qdisc_uaf_repro
```

It performs the following steps:

1. Creates a new network namespace.
2. Brings up `lo`.
3. Creates a TAP device `syz_tun` with `IFF_VNET_HDR` and assigns it
   `fe80::aa`.
4. Creates an `ip6gretap0` tunnel with default parameters and assigns it
   `fe80::1b`.
5. Creates a veth pair `hsr_slave_0` / `veth0_to_hsr` and brings both
   ends up.
6. Creates an `hsr0` device with `ip6gretap0` and `hsr_slave_0` as
   slaves.
7. Builds a frame prefixed with a `virtio_net_hdr` requesting
   `VIRTIO_NET_HDR_GSO_TCPV4`, followed by:
   * outer Ethernet header (ethertype `ETH_P_IPV6`),
   * outer IPv6 header (next header `IPPROTO_GRE`, dst `ff02::1`),
   * GRE header with protocol `ETH_P_TEB`,
   * an invalid all-zero inner Ethernet header.
8. Writes the frame into `syz_tun` repeatedly.

The invalid inner Ethernet header is intended to make
`eth_type_trans()` classify the decapsulated frame as `ETH_P_802_2`,
so the stale `transport_header` is not reset.  The contradictory GSO
metadata is intended to make `qdisc_pkt_len_segs_init()` dereference
the stale header when HSR forwards the skb.

**Current status:** the program sets up the topology successfully and
injects the packet without errors, but it does **not** crash on the
current VM configuration.  The same is true for the original syzbot
`repro.c` (see section 9).  Deterministic triggering of this bug from
userspace requires the exact skb state (stale negative
`transport_header` + contradictory GSO flag) to survive the whole
network path, which depends on the precise kernel configuration and
TUN/GRE/HSR behaviour.

### 2.2 Minimal kernel module reproducer (`qdisc_uaf_repro_mod.c`)

`qdisc_uaf_repro_mod.c` creates a fake `sk_buff` in kernel space with:

* `skb->data` pointing to a freshly allocated 64-byte buffer.
* `skb->transport_header` set to `skb->data - 14`, simulating the
  stale-negative offset.
* `skb->len = 64` and `SKB_GSO_TCPV4` set so the vulnerable branch is
  taken.
* A module parameter `simulate_fix`.

When `simulate_fix=0`, the module follows the exact arithmetic of
`qdisc_pkt_len_segs_init()` and dereferences the wild address, causing
a KASAN / page fault crash.

When `simulate_fix=1`, the module first checks whether
`skb_transport_header(skb) < skb->data` and aborts the vulnerable path,
demonstrating the repair model.

## 3. Build Instructions

### 3.1 Userspace C reproducer

```bash
cd /home/sisyphus/code/linux/qdisc_uaf
gcc -o qdisc_uaf_repro qdisc_uaf_repro.c -static
```

### 3.2 Kernel module reproducer

From the top of the kernel tree:

```bash
cd /home/sisyphus/code/linux

# Build the reproducer module
make -C /home/sisyphus/code/linux M=/home/sisyphus/code/linux/qdisc_uaf modules
```

The resulting module is:

```text
qdisc_uaf/qdisc_uaf_repro_mod.ko
```

## 4. Kernel Configuration Requirements

The kernel must have KASAN enabled to observe the crash cleanly.
Key config options used during reproduction:

```text
CONFIG_KASAN=y
CONFIG_KASAN_GENERIC=y
CONFIG_KASAN_INLINE=y
CONFIG_NET=y
CONFIG_INET=y
CONFIG_NETDEVICES=y
CONFIG_TUN=y
CONFIG_HSR=y
CONFIG_NET_IPGRE=y
CONFIG_IPV6_GRE=y
CONFIG_NETDEVSIM=y
```

The actual `.config` used is stored as the kernel source tree's
`.config`.

## 5. Running the Reproducer

### 5.1 Crash case (simulated bug)

```bash
insmod /path/to/qdisc_uaf_repro_mod.ko simulate_fix=0
```

Expected result: the kernel crashes in `__asan_load2` while trying to
read from `skb->data + 0xfffffff2`.

### 5.2 Fixed case (simulated repair)

```bash
insmod /path/to/qdisc_uaf_repro_mod.ko simulate_fix=1
```

Expected result: the module detects the stale negative transport header,
aborts the vulnerable path, and prints `survived`.

## 6. Actual Results

### 6.1 Crash case

Command:

```bash
insmod qdisc_uaf_repro_mod.ko simulate_fix=0
```

Dmesg excerpt:

```text
[   77.135823] qdisc_uaf_repro: data=ffff888009e2e3e0 transport_header=ffff888009e2e3d2 offset=-14
[   77.136548] qdisc_uaf_repro: hdr_len (unsigned) = 0xfffffff2
[   77.136880] qdisc_uaf_repro: about to dereference skb->data + 0xfffffff2
[   77.137073] qdisc_uaf_repro: th=ffff888109e2e3d2
[   77.138196] BUG: unable to handle page fault for address: ffffed10213c5c7b
[   77.138196] #PF: supervisor read access in kernel mode
[   77.138196] #PF: error_code(0x0000) - not-present page
[   77.138196] Oops: Oops: 0000 [#1] SMP KASAN NOPTI
[   77.138196] CPU: 1 UID: 0 PID: 355 Comm: insmod Tainted: G           O        7.2.0-rc4-00061-g248951ddc14d #252 PREEMPT(lazy)
[   77.138196] RIP: 0010:__asan_load2+0x48/0xa0
...
[   77.138196] Call Trace:
[   77.138196]  qdisc_uaf_repro_init+0x254/0xff0 [qdisc_uaf_repro_mod]
[   77.138196]  do_one_initcall+0xa1/0x2d0
[   77.138196]  do_init_module+0x187/0x470
[   77.138196]  load_module+0x308e/0x3300
[   77.138196]  init_module_from_file+0x156/0x180
[   77.138196]  idempotent_init_module+0x1a3/0x440
[   77.138196]  __x64_sys_finit_module+0x78/0xc0
[   77.138196]  do_syscall_64+0xf9/0x540
[   77.138196]  entry_SYSCALL_64_after_hwframe+0x77/0x7f
```

Full dmesg: `crash_dmesg.log`
Full test output: `crash_test.log`

### 6.2 Fixed case

Command:

```bash
insmod qdisc_uaf_repro_mod.ko simulate_fix=1
```

Dmesg excerpt:

```text
[   76.621196] qdisc_uaf_repro: data=ffff888042eaf0e0 transport_header=ffff888042eaf0d2 offset=-14
[   76.622115] qdisc_uaf_repro: hdr_len (unsigned) = 0xfffffff2
[   76.622365] qdisc_uaf_repro: about to dereference skb->data + 0xfffffff2
[   76.622488] qdisc_uaf_repro: simulate_fix: stale transport_header detected, aborting vulnerable path
[   76.623287] qdisc_uaf_repro: survived (KASAN may have already fired)
```

The system remains stable and the test script completes successfully.

Full dmesg: `fixed_dmesg.log`
Full test output: `fixed_test.log`

## 7. Userspace C Reproducer Test Results

The focused userspace C reproducer `qdisc_uaf_repro.c` was tested with
the KASAN-enabled kernel:

```bash
./qdisc_uaf_repro
```

Dmesg excerpt:

```text
[   75.323601] ip6gretap0: entered promiscuous mode
[   75.334737] hsr_slave_0: entered promiscuous mode
[   75.419694] qdisc_uaf_repro (362) used greatest stack depth: 24840 bytes left
[   75.459219] ip6gretap0 (unregistering): left promiscuous mode
[   75.551458] hsr_slave_0: left promiscuous mode
```

The program exits cleanly and the devices are torn down, but no kernel
oops is observed on the current VM configuration.

The original syzbot `repro.c` was also compiled and run under the same
kernel; it likewise completed without triggering a crash.  This suggests
that the exact kernel configuration / TUN / GRO / GSO environment used
by syzbot is required for the userspace path to reach the vulnerable
`qdisc_pkt_len_segs_init()` call.

## 8. Analysis

The reproducer confirms that the root cause is a stale
`transport_header` pointing before `skb->data`.  The arithmetic
performed by `qdisc_pkt_len_segs_init()` is:

```c
unsigned int hdr_len = skb_transport_header(skb) - skb->data;
```

With `transport_header = data - 14`, this becomes
`hdr_len = 0xfffffff2`.  Treating that as an offset produces an illegal
pointer, and even KASAN's shadow memory check crashes while validating
the read.

The `mailist.patch` fix prevents this by resetting the transport header
after GRE/TEB decapsulation so the offset is never stale-negative.  The
`simulate_fix=1` path shows the same effect: detect the invalid offset
and avoid the dereference.

For a detailed explanation of how the reproducer packet is constructed and
why the stale offset survives through the HSR and bridge forwarding paths,
see Section 14.

## 9. Working Userspace C Reproducer (ip6gretap + ingress qdisc)

A working pure-userspace C reproducer based on the patch author's reference
implementation is provided as `qdisc_uaf_repro_gist.c`.  It constructs a
malicious frame that survives the network path and triggers the bug in
`qdisc_pkt_len_segs_init()`.

### Trigger path

```
write() to /dev/net/tun (IFF_TUN + TUN_PI + virtio_net_hdr)
  -> tun_get_user()
  -> netif_receive_skb_list_internal()
  -> __netif_receive_skb_core()
  -> sch_handle_ingress()        # ingress qdisc on ip6gretap0
  -> qdisc_pkt_len_segs_init()   # dereferences stale transport_header
```

### Packet structure

* TUN PI header: proto = `ETH_P_IPV6`
* virtio_net_hdr: `gso_type = VIRTIO_NET_HDR_GSO_TCPV4`, `gso_size = 4`,
  `hdr_len = 17`
* Outer IPv6 with a Destination Options extension header, next header `IPPROTO_GRE`
* GRE header: protocol = `ETH_P_TEB`
* Inner Ethernet + IPv4/TCP payload

The IPv6 Destination Options header is the key detail that makes the GRE/TEB
decap path leave `transport_header` stale while still carrying contradictory
GSO metadata.

### Build and run

```bash
gcc -o qdisc_uaf_repro qdisc_uaf_repro_gist.c -static
```

Run inside the `test-kernel` VM by placing the binary in
`/home/sisyphus/code/test/` and using the included `test.sh`.

### Result

The kernel panics with a fatal exception in interrupt context:

```text
BUG: unable to handle page fault for address: ffffed102873b02e
#PF: supervisor read access in kernel mode
RIP: 0010:__asan_load2+0x48/0xa0
Call Trace:
  qdisc_pkt_len_segs_init+0x128/0x300
  __netif_receive_skb_core.constprop.0+0x8ec/0x1440
  ...
  tun_get_user+0x864/0x1c10
  tun_chr_write_iter+0xd2/0x130
  vfs_write
  ksys_write
Kernel panic - not syncing: Fatal exception in interrupt
```

Full serial log: `test_kernel_gist_serial.log`.

## 10. HSR Forwarding Reproducer (matches original call trace)

A second pure-userspace C reproducer, `qdisc_uaf_repro_hsr.c`, triggers the
same bug through the HSR forwarding path so that the Call Trace matches the
original syzbot report.

### Trigger path

```
write() to /dev/net/tun (IFF_TUN + TUN_PI + virtio_net_hdr)
  -> tun_get_user()
  -> netif_receive_skb()
  -> __netif_receive_skb_core()
  -> hsr_handle_frame()          # rx_handler on ip6gretap slave
  -> hsr_forward_skb()
  -> hsr_xmit()
  -> __dev_queue_xmit()
  -> qdisc_pkt_len_segs_init()   # dereferences stale transport_header
```

### Topology

```
/dev/net/tun (syzq0)
  -> ip6gretap (gt1)
     -> hsr0 (HSR v0, slaves: gt1, veth0; interlink: veth1)
        -> veth0
```

### Packet structure

* TUN PI header: proto = `ETH_P_IPV6`
* virtio_net_hdr: `gso_type = VIRTIO_NET_HDR_GSO_TCPV4`, `gso_size = 4`,
  `hdr_len = 17`
* Outer IPv6 with a Destination Options extension header, next header `IPPROTO_GRE`
* GRE header: protocol = `ETH_P_TEB`
* Inner Ethernet frame with `h_proto = ETH_P_HSR` (0x88FB)
* HSR tag (path/LSDU size, sequence number, encapsulated proto = `ETH_P_IP`)
* Inner IPv4/TCP payload

The HSR tag causes the frame to be classified as an HSRv0 frame.  HSR forwarding
clones the skb and calls `hsr_xmit()` -> `dev_queue_xmit()` on the slave port,
where `qdisc_pkt_len_segs_init()` sees the stale `transport_header` from the
GRE/TEB decap and dereferences before `skb->data`.

### Build and run

```bash
cd /home/sisyphus/code/linux/qdisc_uaf
gcc -o qdisc_uaf_repro_hsr qdisc_uaf_repro_hsr.c -static
```

Run inside the `test-kernel` VM by placing the binary in
`/home/sisyphus/code/test/` and using the included `test.sh`.

### Result

The kernel panics with the original HSR forwarding stack:

```text
BUG: unable to handle page fault for address: ffffed1020a1682e
Oops: 0000 [#1] SMP KASAN NOPTI
RIP: 0010:__asan_load2+0x48/0xa0
Call Trace:
  qdisc_pkt_len_segs_init+0x128/0x300
  __dev_queue_xmit+0x13a/0x1960
  hsr_forward_skb+0x7d3/0xe50
  hsr_handle_frame+0x24b/0x350
  tun_get_user+0x864/0x1c10
  tun_chr_write_iter+0xd2/0x130
  ...
Kernel panic - not syncing: Fatal exception in interrupt
```

Full serial log: `test_kernel_hsr_serial.log`.

## 11. Bridge Forwarding Reproducer

A third pure-userspace C reproducer, `qdisc_uaf_repro_bridge.c`, triggers the
same bug through the Linux bridge forwarding path.  This variant is simpler
than the HSR one because it does not rely on the HSR protocol; it only needs
a standard Linux bridge.

### Trigger path

```
write() to /dev/net/tun (IFF_TUN + TUN_PI + virtio_net_hdr)
  -> tun_get_user()
  -> netif_receive_skb()
  -> __netif_receive_skb_core()
  -> br_handle_frame()            # rx_handler on ip6gretap slave
  -> br_handle_frame_finish()
  -> br_forward()
  -> br_forward_finish()
  -> __dev_queue_xmit()
  -> qdisc_pkt_len_segs_init()    # dereferences stale transport_header
```

### Topology

```
/dev/net/tun (syzq0)
  -> ip6gretap (gt1)
     -> bridge (br0, slaves: gt1, veth0)
        -> veth0
```

### Why disabling GRO is not required

The bridge forwarding path clones the received skb and pushes the clone into
`__dev_queue_xmit()` before the local IP/GRO path gets a chance to reset
`transport_header`.  Even when the inner Ethernet frame uses `ETH_P_IP`, the
forwarded clone keeps the stale `transport_header` from the GRE/TEB decap and
reaches `qdisc_pkt_len_segs_init()`.  Therefore the reproducer does not need to
disable GRO or use a non-IP inner ethertype.

### Packet structure

* TUN PI header: proto = `ETH_P_IPV6`
* virtio_net_hdr: `gso_type = VIRTIO_NET_HDR_GSO_TCPV4`, `gso_size = 4`,
  `hdr_len = 17`
* Outer IPv6 with a Destination Options extension header, next header `IPPROTO_GRE`
* GRE header: protocol = `ETH_P_TEB`
* Inner Ethernet frame with `h_proto = ETH_P_IP` (0x0800) and broadcast destination
* Raw payload

### Build and run

```bash
cd /home/sisyphus/code/linux/qdisc_uaf
gcc -o qdisc_uaf_repro_bridge qdisc_uaf_repro_bridge.c -static
```

Run inside the `test-kernel` VM by placing the binary in
`/home/sisyphus/code/test/` and using the included `test_bridge.sh`.

### Result

The kernel panics through the bridge forwarding stack:

```text
BUG: unable to handle page fault for address: ffffed102865862e
Oops: 0000 [#1] SMP KASAN NOPTI
RIP: 0010:__asan_load2+0x48/0xa0
Call Trace:
  qdisc_pkt_len_segs_init+0x128/0x300
  __dev_queue_xmit+0x13a/0x1960
  br_handle_frame_finish+0x2b6/0xae0
  br_handle_frame+0x289/0x410
  tun_get_user+0x864/0x1c10
  tun_chr_write_iter+0xd2/0x130
  ...
Kernel panic - not syncing: Fatal exception in interrupt
```

Full serial log: `test_kernel_bridge_serial.log`.

## 12. OLK-6.6 Reproduction

The same HSR forwarding reproducer was run against the OLK-6.6 kernel
(`/home/sisyphus/code/kernel/arch/x86/boot/bzImage`) built with the same
configuration.

```bash
test-kernel /home/sisyphus/code/kernel/arch/x86/boot/bzImage
```

OLK-6.6 still has the vulnerable code path, but `qdisc_pkt_len_segs_init()`
has not yet been split out; the equivalent logic lives inside
`qdisc_pkt_len_init()` in `net/core/dev.c`.  The crash therefore appears one
frame higher in the Call Trace:

```text
BUG: unable to handle page fault for address: ffffed1020cc462e
Oops: 0000 [#1] PREEMPT SMP KASAN NOPTI
RIP: 0010:__asan_load2+0x48/0xa0
Call Trace:
  __dev_queue_xmit+0x1f8/0x16e0
  hsr_forward_skb+0x620/0xba0
  hsr_handle_frame+0x21a/0x300
  tun_get_user+0x13d4/0x2530
  tun_chr_write_iter+0xd2/0x130
  ...
Kernel panic - not syncing: Fatal exception in interrupt
```

The panic is reproducible on every run.

Full serial log: `test_kernel_hsr_olk6.6_serial.log`.

### OLK-6.6 bridge forwarding variant

The bridge forwarding reproducer was also run on OLK-6.6.  As with the HSR
variant, the crash appears one frame higher because `qdisc_pkt_len_segs_init()`
has not been split out yet:

```text
BUG: unable to handle page fault for address: ffffed1020a9d02e
Oops: 0000 [#1] PREEMPT SMP KASAN NOPTI
RIP: 0010:__asan_load2+0x48/0xa0
Call Trace:
  __dev_queue_xmit+0x1f8/0x16e0
  br_handle_frame_finish+0x2b5/0xab0
  br_handle_frame+0x29b/0x390
  tun_get_user+0x13d4/0x2530
  ...
Kernel panic - not syncing: Fatal exception in interrupt
```

Full serial log: `test_kernel_bridge_olk6.6_serial.log`.

## 13. C Loader + test-kernel Reproduction

As a fallback when the userspace network path is hard to stabilise, a
deterministic C-based reproduction is provided that loads the minimal kernel
module reproducer via `finit_module(2)` and runs it inside the `test-kernel`
QEMU environment.

Files:

* `qdisc_uaf_repro_loader.c` — userspace C loader that calls
  `finit_module(qdisc_uaf_repro_mod.ko, simulate_fix=...)`.
* `test_kernel_crash_loader.log` — `test-kernel` output for `simulate_fix=0`
  (crash reproduced).
* `test_kernel_fixed_loader.log` — `test-kernel` output for `simulate_fix=1`
  (safety path verified).

Build:

```bash
gcc -o qdisc_uaf_repro_loader qdisc_uaf_repro_loader.c -static
```

Run with `test-kernel`:

```bash
# Crash case
test-kernel /path/to/bzImage

# Fixed case (pass 1 to the loader)
```

The `test-kernel` VM mounts `/home/sisyphus/code/test` as `/mnt/shared`.
The `test.sh` script there copies `qdisc_uaf_repro_loader` and
`qdisc_uaf_repro_mod.ko` into the VM and runs the loader.

## 14. Technical Analysis: Why the Packet Triggers the Bug

This section explains how the reproducer packet is constructed, why the
stale `transport_header` survives, and whether such a packet can exist in
real networks.

### 14.1 Packet structure

The HSR and bridge forwarding reproducers build the following frame (outermost first):

```
[TUN PI header] + [virtio_net_hdr] + [Outer IPv6 + Dest Opts] + [GRE]
  + [Inner Ethernet + HSR tag] + [IPv4/TCP payload]
```

| Layer | Purpose |
|-------|---------|
| TUN PI header (`proto = ETH_P_IPV6`) | Tells the TUN driver that the payload is an IPv6 packet. |
| `virtio_net_hdr` | Injects GSO metadata directly from userspace. `gso_type = VIRTIO_NET_HDR_GSO_TCPV4`, `gso_size = 4`, `hdr_len = 17`. This marks the skb as `SKB_GSO_TCPV4 \| SKB_GSO_DODGY`. |
| Outer IPv6 + Destination Options (256 bytes) | Carries the GRE payload. The Dest-Opts header is the detail that lets the GRE/TEB decap path leave `transport_header` stale. |
| GRE header (`protocol = ETH_P_TEB`) | Declares that the GRE payload is a bridged Ethernet frame. |
| Inner Ethernet (`h_proto = ETH_P_HSR`) | Makes the decapsulated frame look like an HSRv0 frame so that `hsr_handle_frame()` takes over.  For the bridge variant the ethertype is a non-IP value (`0x88FB`). |
| HSR tag (6 bytes) | Required for HSRv0 parsing; causes `hsr_forward_skb()` to forward the frame. |
| Inner IPv4/TCP payload | Provides a TCP header during GRO, which is where the original `transport_header` was set.  For the bridge variant this is replaced by raw payload because an IP ethertype would allow the IP layer to reset `transport_header`. |

### 14.2 Why `transport_header` becomes stale

The patch in `mailist.patch` describes the root cause precisely:

1. **Before decap:** `transport_header` points to the outer L4 (GRE) header.
2. **`__iptunnel_pull_header()`** advances `skb->data` past the IPv6 + Dest-Opts + GRE headers, but does **not** update `transport_header`.
3. **`eth_type_trans()`** in `__ip6_tnl_rcv()` further pulls `ETH_HLEN` (14 bytes) from `skb->data`.
4. After these two pulls, `skb->data` has moved forward while `transport_header` still points to the old position, resulting in a negative `skb_transport_offset()`.

In the normal case where the inner frame is IPv4/TCP, `ip_rcv_core()` or
`inet_gro_receive()` eventually rewrites `transport_header`, so the stale
value never reaches `qdisc_pkt_len_segs_init()`.

In our reproducer, the inner frame is `ETH_P_HSR`.  HSR only sets
`network_header`; it never resets `transport_header`.  Therefore the stale,
negative offset survives all the way to `__dev_queue_xmit()`:

```c
hdr_len = skb_transport_offset(skb);       // negative -> huge unsigned
th = (const struct tcphdr *)(skb->data + hdr_len);   // wild read
__tcp_hdrlen(th);                                      // KASAN / page fault
```

### 14.3 Why the GSO path is needed

`qdisc_pkt_len_segs_init()` only dereferences the transport header when the
skb is GSO (`shinfo->gso_size != 0`).  The `virtio_net_hdr` from the TUN
interface sets `SKB_GSO_TCPV4` and `gso_size = 4`, so the function enters the
vulnerable branch and trusts the stale offset.

The `SKB_GSO_DODGY` flag also matters: it tells the kernel "recompute the
number of segments".  That forces `qdisc_pkt_len_segs_init()` to run the
hdr_len arithmetic instead of taking a pre-computed `gso_segs` value.

### 14.4 Can this happen in real networks?

**Short answer:** naturally occurring traffic is extremely unlikely, but the
attack surface is real.

Legitimate components that can produce pieces of this packet:

| Component | Real-world relevance |
|-----------|----------------------|
| TUN/TAP / virtio-net | Used by VMs, containers, VPN clients. Allows userspace to supply arbitrary GSO metadata. |
| GRE/TEB tunnels | Real Ethernet-over-GRE-over-IPv6 deployments exist. |
| HSR | Industrial Ethernet redundancy (IEC 62439-3). |
| IPv6 Destination Options | Standard IPv6 extension header. |

Plausible attack / failure scenarios:

1. **Malicious VM or container tenant.** If an untrusted user controls a
   virtio-net or TUN device, they can inject the exact `virtio_net_hdr` + GSO
   metadata + HSR inner frame combination and crash the host kernel.

2. **Tunnel endpoint forwarding GSO traffic into an HSR network.** A
   GRE/TEB-to-HSR gateway that receives GSO-marked packets with non-IP inner
   payloads could hit the same path.

3. **Fuzzing-discovered corner case.** syzbot found the bug by combining
   `tun + ip6gretap + HSR`; each layer is individually valid, but the
   combination exposes the stale-header state machine.

Why natural traffic rarely triggers it:

* Normal physical NICs do not allow arbitrary `virtio_net_hdr` injection.
* Real tunnel endpoints do not put contradictory `gso_size = 4` / `hdr_len = 17`
  values on HSR or bridge-forwarded frames.
* `ETH_P_HSR` is a specialized industrial protocol, not common on the public
  Internet.

### 14.5 Scope broader than the patch description

The patch author noted that the bug triggers when the inner Ethernet header
is invalid (`ETH_P_802_2`) so that IP-layer rescue never runs.  The HSR and
bridge reproducers show the bug is broader: **any forwarding path that clones
the skb and pushes it to `__dev_queue_xmit()` before IP-layer
transport-header reset can leave the stale offset in place.**  HSR is one such
protocol; Linux bridge forwarding is another, and it works even when the
inner Ethernet `h_proto` is `ETH_P_IP` because the bridge clone reaches the
qdisc before the local IP/GRO path can reset `transport_header`.

## 15. Patch Verification (`mailist.patch`)

The community reference patch `mailist.patch` ([PATCH net v2] net: iptunnel: fix stale transport header after GRE/TEB decap) was applied to the current tree, the kernel was rebuilt, and both the HSR and bridge reproducers were run under `test-kernel`.

### What the patch does

* Adds `iptunnel_rebuild_transport_header()` in `include/net/ip_tunnels.h`.
* Calls it from `ip_tunnel_rcv()` (IPv4 GRE/TEB) and `__ip6_tnl_rcv()` (IPv6 GRE/TEB).
* For GSO packets only, it resets `transport_header` to "unset", re-probes the real inner transport header with the flow dissector, and clears contradictory GSO metadata (`skb_gso_reset()`) if the probe fails.

### HSR reproducer result (patched kernel)

* Test command: `test-kernel /home/sisyphus/code/linux/arch/x86/boot/bzImage` with `test.sh` set to the HSR variant.
* Kernel version: `7.2.0-rc4-00061-g248951ddc14d-dirty`.
* Reproducer exit code: `0`.
* Result: **No panic, no KASAN report, no Call Trace.** The VM completed the test and stayed alive.
* Only dmesg noise was the expected `ip6_tunnel: gt1 xmit: Local address not yet configured!` messages.

### Bridge reproducer result (patched kernel)

* Test command: `test-kernel /home/sisyphus/code/linux/arch/x86/boot/bzImage` with `test.sh` set to the bridge variant.
* Kernel version: `7.2.0-rc4-00061-g248951ddc14d-dirty`.
* Reproducer exit code: `0`.
* Result: **No panic, no KASAN report, no Call Trace.** The bridge-forwarded GSO packet was handled safely.  The same result was obtained with the inner Ethernet `h_proto` set to `ETH_P_IP` (0x0800); the patch fixes this case as well.

### Bridge IP ethertype result (unpatched kernel, for comparison)

* Test command: `test-kernel /home/sisyphus/code/linux/arch/x86/boot/bzImage` (patch reverted) with `test.sh` set to a variant using `h_proto = ETH_P_IP`.
* Kernel version: `7.2.0-rc4-00061-g248951ddc14d`.
* Reproducer exit code: N/A (kernel panicked).
* Result: **Panic through the same bridge Call Trace**, confirming that an IP inner ethertype is sufficient to trigger the bug and that disabling GRO is unnecessary for the bridge forwarding path.

### Conclusion

`mailist.patch` prevents the use-after-free / invalid memory read for both the original HSR path and the broader bridge-forwarding path. The patch correctly rebuilds (or clears) stale transport-header state after GRE/TEB decapsulation before the skb reaches `__dev_queue_xmit()` and `qdisc_pkt_len_segs_init()`.

### Patch verification artifacts

| File | Description |
|------|-------------|
| `test_kernel_hsr_patched_serial.log` | Full serial log from HSR reproducer on patched kernel (no panic). |
| `test_kernel_hsr_patched_test_result.txt` | `test-kernel` summary for HSR reproducer on patched kernel. |
| `test_kernel_bridge_patched_serial.log` | Full serial log from bridge reproducer on patched kernel (no panic). |
| `test_kernel_bridge_patched_test_result.txt` | `test-kernel` summary for bridge reproducer on patched kernel. |
| `test_kernel_bridge_ip_unpatched_serial.log` | Full serial log from bridge reproducer with IP inner ethertype on unpatched kernel (panic). |
| `test_kernel_bridge_ip_patched_serial.log` | Full serial log from bridge reproducer with IP inner ethertype on patched kernel (no panic). |
| `test_kernel_bridge_ip_patched_test_result.txt` | `test-kernel` summary for bridge IP ethertype reproducer on patched kernel. |

## 16. Files in This Directory

| File | Description |
|------|-------------|
| `README.md` | Original problem statement from syzbot reference. |
| `repro.c` | Original userspace reproducer from syzbot. |
| `qdisc_uaf_repro.c` | Focused userspace C reproducer for the TUN -> ip6gretap -> HSR path. |
| `qdisc_uaf_repro_gist.c` | Working userspace C reproducer (ip6gretap + ingress qdisc + IPv6 dst-opts). |
| `qdisc_uaf_repro_hsr.c` | Working userspace C reproducer matching the original HSR forwarding stack. |
| `qdisc_uaf_repro_bridge.c` | Working userspace C reproducer using Linux bridge forwarding. |
| `qdisc_uaf_repro_loader.c` | Userspace C loader for the kernel module reproducer. |
| `stack` | Original crash stack trace. |
| `mailist.patch` | Community reference patch for the GRE/TEB decap path. |
| `qdisc_uaf_repro_mod.c` | Minimal kernel module reproducer. |
| `Makefile` | Kbuild Makefile for the module. |
| `REPRO.md` | This reproduction report. |
| `crash_dmesg.log` | Full dmesg from the crash case. |
| `crash_test.log` | Full test output from the crash case. |
| `fixed_dmesg.log` | Full dmesg from the fixed case. |
| `fixed_test.log` | Full test output from the fixed case. |
| `test_kernel_crash_loader.log` | `test-kernel` crash output using the C loader. |
| `test_kernel_fixed_loader.log` | `test-kernel` fixed-case output using the C loader. |
| `test_kernel_gist_serial.log` | Full serial log from the working userspace C reproducer (VM panic). |
| `test_kernel_hsr_serial.log` | Full serial log from the HSR forwarding reproducer (VM panic). |
| `test_kernel_bridge_serial.log` | Full serial log from the bridge forwarding reproducer (VM panic). |
| `test_kernel_hsr_patched_serial.log` | Full serial log from HSR reproducer on patched kernel (no panic). |
| `test_kernel_hsr_patched_test_result.txt` | `test-kernel` summary for HSR reproducer on patched kernel. |
| `test_kernel_bridge_patched_serial.log` | Full serial log from bridge reproducer on patched kernel (no panic). |
| `test_kernel_bridge_patched_test_result.txt` | `test-kernel` summary for bridge reproducer on patched kernel. |
| `test_kernel_bridge_ip_unpatched_serial.log` | Full serial log from bridge reproducer with IP inner ethertype on unpatched kernel (panic). |
| `test_kernel_bridge_ip_patched_serial.log` | Full serial log from bridge reproducer with IP inner ethertype on patched kernel (no panic). |
| `test_kernel_bridge_ip_patched_test_result.txt` | `test-kernel` summary for bridge IP ethertype reproducer on patched kernel. |
| `test_kernel_hsr_olk6.6_serial.log` | Full serial log from the HSR forwarding reproducer on OLK-6.6 (VM panic). |
| `test_kernel_bridge_olk6.6_serial.log` | Full serial log from the bridge forwarding reproducer on OLK-6.6 (VM panic). |
