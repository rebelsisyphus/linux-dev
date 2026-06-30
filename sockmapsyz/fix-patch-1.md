Subject: [PATCH bpf-next v9 0/3] bpf: Fix FIONREAD and copied_seq issues
Date: Sat, 24 Jan 2026 19:32:42 +0800
Message-ID: <20260124113314.113584-1-jiayuan.chen@linux.dev>
Precedence: bulk
X-Mailing-List: bpf@vger.kernel.org
List-Id: <bpf.vger.kernel.org>
List-Subscribe: <mailto:bpf+subscribe@vger.kernel.org>
List-Unsubscribe: <mailto:bpf+unsubscribe@vger.kernel.org>
MIME-Version: 1.0
Content-Transfer-Encoding: 8bit
X-Migadu-Flow: FLOW_OUT

syzkaller reported a bug [1] where a socket using sockmap, after being
unloaded, exposed incorrect copied_seq calculation. The selftest I
provided can be used to reproduce the issue reported by syzkaller.

TCP recvmsg seq # bug 2: copied E92C873, seq E68D125, rcvnxt E7CEB7C, fl 40
WARNING: CPU: 1 PID: 5997 at net/ipv4/tcp.c:2724 tcp_recvmsg_locked+0xb2f/0x2910 net/ipv4/tcp.c:2724
Call Trace:
 <TASK>
 receive_fallback_to_copy net/ipv4/tcp.c:1968 [inline]
 tcp_zerocopy_receive+0x131a/0x2120 net/ipv4/tcp.c:2200
 do_tcp_getsockopt+0xe28/0x26c0 net/ipv4/tcp.c:4713
 tcp_getsockopt+0xdf/0x100 net/ipv4/tcp.c:4812
 do_sock_getsockopt+0x34d/0x440 net/socket.c:2421
 __sys_getsockopt+0x12f/0x260 net/socket.c:2450
 __do_sys_getsockopt net/socket.c:2457 [inline]
 __se_sys_getsockopt net/socket.c:2454 [inline]
 __x64_sys_getsockopt+0xbd/0x160 net/socket.c:2454
 do_syscall_x64 arch/x86/entry/syscall_64.c:63 [inline]
 do_syscall_64+0xcd/0xfa0 arch/x86/entry/syscall_64.c:94
 entry_SYSCALL_64_after_hwframe+0x77/0x7f

A sockmap socket maintains its own receive queue (ingress_msg) which may
contain data from either its own protocol stack or forwarded from other
sockets.

                                                     FD1:read()
                                                     --  FD1->copied_seq++
                                                         |  [read data]
                                                         |
                                [enqueue data]           v
                  [sockmap]     -> ingress to self ->  ingress_msg queue
FD1 native stack  ------>                                 ^
-- FD1->rcv_nxt++               -> redirect to other      | [enqueue data]
                                       |                  |
                                       |             ingress to FD1
                                       v                  ^
                                      ...                 |  [sockmap]
                                                     FD2 native stack

The issue occurs when reading from ingress_msg: we update tp->copied_seq
by default, but if the data comes from other sockets (not the socket's
own protocol stack), tcp->rcv_nxt remains unchanged. Later, when
converting back to a native socket, reads may fail as copied_seq could
be significantly larger than rcv_nxt.

Additionally, FIONREAD calculation based on copied_seq and rcv_nxt is
insufficient for sockmap sockets, requiring separate field tracking.

[1] https://syzkaller.appspot.com/bug?extid=06dbd397158ec0ea4983

---
v7 -> v9: Address Jakub Sitnicki's feedback:
          - Remove sk_receive_queue check in tcp_bpf_ioctl, only report
            ingress_msg data length for FIONREAD
          - Minor nits fixes
          - Add Reviewed-by tag from John Fastabend
          - Fix ci error
          https://lore.kernel.org/bpf/20260113025121.197535-1-jiayuan.chen@linux.dev/

v5 -> v7: Some modifications suggested by Jakub Sitnicki, and added Reviewed-by tag.
https://lore.kernel.org/bpf/20260106051458.279151-1-jiayuan.chen@linux.dev/

v1 -> v5: Use skmsg.sk instead of extending BPF_F_XXX macro and fix CI
          failure reported by CI
v1: https://lore.kernel.org/bpf/20251117110736.293040-1-jiayuan.chen@linux.dev/

Jiayuan Chen (3):
  bpf, sockmap: Fix incorrect copied_seq calculation
  bpf, sockmap: Fix FIONREAD for sockmap
  bpf, selftest: Add tests for FIONREAD and copied_seq

 include/linux/skmsg.h                         |  70 ++++-
 net/core/skmsg.c                              |  30 +-
 net/ipv4/tcp_bpf.c                            |  25 +-
 net/ipv4/udp_bpf.c                            |  23 +-
 .../selftests/bpf/prog_tests/sockmap_basic.c  | 294 +++++++++++++++++-
 .../bpf/progs/test_sockmap_pass_prog.c        |  14 +
 6 files changed, 439 insertions(+), 17 deletions(-)

-- 
2.43.0

