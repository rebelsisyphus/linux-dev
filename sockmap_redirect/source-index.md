# 源码索引

基线：`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。以下为分析时函数/结构体定义起始行。

链接指向当前仓库；切换提交后行号可能变化。函数表中的间接调用还需对照主分析中的配置条件。

## net/core/sock_map.c

| 符号 | 定义位置 |
|---|---|
| `sock_map_link()` | [net/core/sock_map.c:217](../net/core/sock_map.c#L217) |
| `sock_map_init_proto()` | [net/core/sock_map.c:189](../net/core/sock_map.c#L189) |
| `sock_map_update_common()` | [net/core/sock_map.c:470](../net/core/sock_map.c#L470) |
| `sock_map_redirect_allowed()` | [net/core/sock_map.c:528](../net/core/sock_map.c#L528) |
| `sock_map_update_elem_sys()` | [net/core/sock_map.c:558](../net/core/sock_map.c#L558) |
| `bpf_sk_redirect_map()` | [net/core/sock_map.c:647](../net/core/sock_map.c#L647) |
| `bpf_msg_redirect_map()` | [net/core/sock_map.c:675](../net/core/sock_map.c#L675) |
| `bpf_sk_redirect_hash()` | [net/core/sock_map.c:1253](../net/core/sock_map.c#L1253) |
| `bpf_msg_redirect_hash()` | [net/core/sock_map.c:1281](../net/core/sock_map.c#L1281) |
| `sock_map_prog_link_lookup()` | [net/core/sock_map.c:1477](../net/core/sock_map.c#L1477) |
| `sock_map_close()` | [net/core/sock_map.c:1701](../net/core/sock_map.c#L1701) |

## net/core/skmsg.c

| 符号 | 定义位置 |
|---|---|
| `sk_msg_alloc()` | [net/core/skmsg.c:26](../net/core/skmsg.c#L26) |
| `sk_msg_memcopy_from_iter()` | [net/core/skmsg.c:369](../net/core/skmsg.c#L369) |
| `__sk_msg_recvmsg()` | [net/core/skmsg.c:413](../net/core/skmsg.c#L413) |
| `sk_msg_recvmsg()` | [net/core/skmsg.c:501](../net/core/skmsg.c#L501) |
| `sk_msg_is_readable()` | [net/core/skmsg.c:508](../net/core/skmsg.c#L508) |
| `sk_psock_create_ingress_msg()` | [net/core/skmsg.c:533](../net/core/skmsg.c#L533) |
| `sk_psock_skb_ingress_enqueue()` | [net/core/skmsg.c:545](../net/core/skmsg.c#L545) |
| `sk_psock_skb_ingress()` | [net/core/skmsg.c:591](../net/core/skmsg.c#L591) |
| `sk_psock_skb_ingress_self()` | [net/core/skmsg.c:625](../net/core/skmsg.c#L625) |
| `sk_psock_handle_skb()` | [net/core/skmsg.c:647](../net/core/skmsg.c#L647) |
| `sk_psock_backlog()` | [net/core/skmsg.c:671](../net/core/skmsg.c#L671) |
| `sk_psock_init()` | [net/core/skmsg.c:750](../net/core/skmsg.c#L750) |
| `sk_psock_stop()` | [net/core/skmsg.c:854](../net/core/skmsg.c#L854) |
| `sk_psock_destroy()` | [net/core/skmsg.c:864](../net/core/skmsg.c#L864) |
| `sk_psock_drop()` | [net/core/skmsg.c:889](../net/core/skmsg.c#L889) |
| `sk_psock_map_verd()` | [net/core/skmsg.c:907](../net/core/skmsg.c#L907) |
| `sk_psock_msg_verdict()` | [net/core/skmsg.c:920](../net/core/skmsg.c#L920) |
| `sk_psock_skb_redirect()` | [net/core/skmsg.c:958](../net/core/skmsg.c#L958) |
| `sk_psock_verdict_apply()` | [net/core/skmsg.c:996](../net/core/skmsg.c#L996) |
| `sk_psock_write_space()` | [net/core/skmsg.c:1057](../net/core/skmsg.c#L1057) |
| `sk_psock_strp_read()` | [net/core/skmsg.c:1075](../net/core/skmsg.c#L1075) |
| `sk_psock_strp_parse()` | [net/core/skmsg.c:1109](../net/core/skmsg.c#L1109) |
| `sk_psock_strp_data_ready()` | [net/core/skmsg.c:1127](../net/core/skmsg.c#L1127) |
| `sk_psock_init_strp()` | [net/core/skmsg.c:1143](../net/core/skmsg.c#L1143) |
| `sk_psock_verdict_recv()` | [net/core/skmsg.c:1198](../net/core/skmsg.c#L1198) |
| `sk_psock_verdict_data_ready()` | [net/core/skmsg.c:1230](../net/core/skmsg.c#L1230) |
| `sk_psock_start_verdict()` | [net/core/skmsg.c:1257](../net/core/skmsg.c#L1257) |

## net/ipv4/tcp_bpf.c

| 符号 | 定义位置 |
|---|---|
| `tcp_eat_skb()` | [net/ipv4/tcp_bpf.c:15](../net/ipv4/tcp_bpf.c#L15) |
| `bpf_tcp_ingress()` | [net/ipv4/tcp_bpf.c:33](../net/ipv4/tcp_bpf.c#L33) |
| `tcp_bpf_push()` | [net/ipv4/tcp_bpf.c:91](../net/ipv4/tcp_bpf.c#L91) |
| `tcp_bpf_push_locked()` | [net/ipv4/tcp_bpf.c:153](../net/ipv4/tcp_bpf.c#L153) |
| `tcp_bpf_sendmsg_redir()` | [net/ipv4/tcp_bpf.c:164](../net/ipv4/tcp_bpf.c#L164) |
| `tcp_msg_wait_data()` | [net/ipv4/tcp_bpf.c:181](../net/ipv4/tcp_bpf.c#L181) |
| `tcp_bpf_recvmsg_parser()` | [net/ipv4/tcp_bpf.c:221](../net/ipv4/tcp_bpf.c#L221) |
| `tcp_bpf_ioctl()` | [net/ipv4/tcp_bpf.c:335](../net/ipv4/tcp_bpf.c#L335) |
| `tcp_bpf_recvmsg()` | [net/ipv4/tcp_bpf.c:368](../net/ipv4/tcp_bpf.c#L368) |
| `tcp_bpf_send_verdict()` | [net/ipv4/tcp_bpf.c:418](../net/ipv4/tcp_bpf.c#L418) |
| `tcp_bpf_sendmsg()` | [net/ipv4/tcp_bpf.c:534](../net/ipv4/tcp_bpf.c#L534) |
| `tcp_bpf_rebuild_protos()` | [net/ipv4/tcp_bpf.c:640](../net/ipv4/tcp_bpf.c#L640) |
| `tcp_bpf_strp_read_sock()` | [net/ipv4/tcp_bpf.c:690](../net/ipv4/tcp_bpf.c#L690) |
| `tcp_bpf_update_proto()` | [net/ipv4/tcp_bpf.c:725](../net/ipv4/tcp_bpf.c#L725) |

## include/linux/skmsg.h

| 符号 | 定义位置 |
|---|---|
| `sk_msg` | [include/linux/skmsg.h:44](../include/linux/skmsg.h#L44) |
| `sk_psock` | [include/linux/skmsg.h:84](../include/linux/skmsg.h#L84) |
| `sk_msg_xfer()` | [include/linux/skmsg.h:199](../include/linux/skmsg.h#L199) |
| `sk_psock_queue_msg()` | [include/linux/skmsg.h:354](../include/linux/skmsg.h#L354) |
| `sk_psock_dequeue_msg()` | [include/linux/skmsg.h:373](../include/linux/skmsg.h#L373) |
| `kfree_sk_msg()` | [include/linux/skmsg.h:421](../include/linux/skmsg.h#L421) |
| `sk_psock_get()` | [include/linux/skmsg.h:495](../include/linux/skmsg.h#L495) |
| `sk_psock_put()` | [include/linux/skmsg.h:509](../include/linux/skmsg.h#L509) |
| `sk_psock_data_ready()` | [include/linux/skmsg.h:515](../include/linux/skmsg.h#L515) |
| `skb_bpf_strparser()` | [include/linux/skmsg.h:580](../include/linux/skmsg.h#L580) |
| `skb_bpf_set_redir()` | [include/linux/skmsg.h:604](../include/linux/skmsg.h#L604) |
| `skb_bpf_redirect_fetch()` | [include/linux/skmsg.h:612](../include/linux/skmsg.h#L612) |
| `skb_bpf_redirect_clear()` | [include/linux/skmsg.h:619](../include/linux/skmsg.h#L619) |

## include/net/sock.h

| 符号 | 定义位置 |
|---|---|
| `skb_set_owner_r()` | [include/net/sock.h:2474](../include/net/sock.h#L2474) |
| `skb_set_owner_sk_safe()` | [include/net/sock.h:2483](../include/net/sock.h#L2483) |
| `sk_is_readable()` | [include/net/sock.h:3175](../include/net/sock.h#L3175) |

## net/core/sock.c

| 符号 | 定义位置 |
|---|---|
| `sock_rfree()` | [net/core/sock.c:2795](../net/core/sock.c#L2795) |
| `sock_efree()` | [net/core/sock.c:2809](../net/core/sock.c#L2809) |
| `sock_def_readable()` | [net/core/sock.c:3649](../net/core/sock.c#L3649) |

## net/core/skbuff.c

| 符号 | 定义位置 |
|---|---|
| `sendmsg_unlocked()` | [net/core/skbuff.c:3301](../net/core/skbuff.c#L3301) |
| `__skb_send_sock()` | [net/core/skbuff.c:3311](../net/core/skbuff.c#L3311) |
| `skb_send_sock()` | [net/core/skbuff.c:3431](../net/core/skbuff.c#L3431) |

## net/ipv4/tcp.c

| 符号 | 定义位置 |
|---|---|
| `tcp_stream_is_readable()` | [net/ipv4/tcp.c:518](../net/ipv4/tcp.c#L518) |
| `tcp_poll()` | [net/ipv4/tcp.c:532](../net/ipv4/tcp.c#L532) |
| `tcp_sendmsg_locked()` | [net/ipv4/tcp.c:1116](../net/ipv4/tcp.c#L1116) |
| `tcp_sendmsg()` | [net/ipv4/tcp.c:1446](../net/ipv4/tcp.c#L1446) |
| `__tcp_cleanup_rbuf()` | [net/ipv4/tcp.c:1551](../net/ipv4/tcp.c#L1551) |
| `tcp_recv_skb()` | [net/ipv4/tcp.c:1626](../net/ipv4/tcp.c#L1626) |
| `__tcp_read_sock()` | [net/ipv4/tcp.c:1662](../net/ipv4/tcp.c#L1662) |
| `tcp_read_sock_noack()` | [net/ipv4/tcp.c:1748](../net/ipv4/tcp.c#L1748) |
| `tcp_read_skb()` | [net/ipv4/tcp.c:1755](../net/ipv4/tcp.c#L1755) |
| `tcp_recvmsg()` | [net/ipv4/tcp.c:2930](../net/ipv4/tcp.c#L2930) |

## include/net/tcp.h

| 符号 | 定义位置 |
|---|---|
| `tcp_epollin_ready()` | [include/net/tcp.h:1823](../include/net/tcp.h#L1823) |

## net/ipv4/af_inet.c

| 符号 | 定义位置 |
|---|---|
| `inet_sendmsg()` | [net/ipv4/af_inet.c:856](../net/ipv4/af_inet.c#L856) |
| `inet_recvmsg()` | [net/ipv4/af_inet.c:884](../net/ipv4/af_inet.c#L884) |

## net/socket.c

| 符号 | 定义位置 |
|---|---|
| `__sock_sendmsg()` | [net/socket.c:810](../net/socket.c#L810) |
| `sock_sendmsg()` | [net/socket.c:826](../net/socket.c#L826) |
| `sock_recvmsg()` | [net/socket.c:1169](../net/socket.c#L1169) |
| `__sys_sendto()` | [net/socket.c:2246](../net/socket.c#L2246) |
| `__sys_recvfrom()` | [net/socket.c:2306](../net/socket.c#L2306) |

## net/ipv4/tcp_ipv4.c

| 符号 | 定义位置 |
|---|---|
| `tcp_v4_do_rcv()` | [net/ipv4/tcp_ipv4.c:1830](../net/ipv4/tcp_ipv4.c#L1830) |
| `tcp_v4_rcv()` | [net/ipv4/tcp_ipv4.c:2070](../net/ipv4/tcp_ipv4.c#L2070) |

## net/ipv4/tcp_input.c

| 符号 | 定义位置 |
|---|---|
| `tcp_queue_rcv()` | [net/ipv4/tcp_input.c:5532](../net/ipv4/tcp_input.c#L5532) |
| `tcp_data_ready()` | [net/ipv4/tcp_input.c:5601](../net/ipv4/tcp_input.c#L5601) |
| `tcp_data_queue()` | [net/ipv4/tcp_input.c:5607](../net/ipv4/tcp_input.c#L5607) |
| `tcp_rcv_established()` | [net/ipv4/tcp_input.c:6500](../net/ipv4/tcp_input.c#L6500) |

## net/ipv4/tcp_output.c

| 符号 | 定义位置 |
|---|---|
| `__tcp_transmit_skb()` | [net/ipv4/tcp_output.c:1536](../net/ipv4/tcp_output.c#L1536) |
| `tcp_transmit_skb()` | [net/ipv4/tcp_output.c:1731](../net/ipv4/tcp_output.c#L1731) |
| `tcp_write_xmit()` | [net/ipv4/tcp_output.c:2964](../net/ipv4/tcp_output.c#L2964) |
| `__tcp_push_pending_frames()` | [net/ipv4/tcp_output.c:3235](../net/ipv4/tcp_output.c#L3235) |

## net/ipv4/ip_output.c

| 符号 | 定义位置 |
|---|---|
| `ip_local_out()` | [net/ipv4/ip_output.c:125](../net/ipv4/ip_output.c#L125) |
| `ip_output()` | [net/ipv4/ip_output.c:427](../net/ipv4/ip_output.c#L427) |
| `__ip_queue_xmit()` | [net/ipv4/ip_output.c:462](../net/ipv4/ip_output.c#L462) |
| `ip_queue_xmit()` | [net/ipv4/ip_output.c:545](../net/ipv4/ip_output.c#L545) |

## net/ipv4/ip_input.c

| 符号 | 定义位置 |
|---|---|
| `ip_protocol_deliver_rcu()` | [net/ipv4/ip_input.c:189](../net/ipv4/ip_input.c#L189) |
| `ip_local_deliver()` | [net/ipv4/ip_input.c:250](../net/ipv4/ip_input.c#L250) |
| `ip_rcv()` | [net/ipv4/ip_input.c:603](../net/ipv4/ip_input.c#L603) |

## net/strparser/strparser.c

| 符号 | 定义位置 |
|---|---|
| `__strp_recv()` | [net/strparser/strparser.c:97](../net/strparser/strparser.c#L97) |
| `strp_read_sock()` | [net/strparser/strparser.c:353](../net/strparser/strparser.c#L353) |
| `strp_data_ready()` | [net/strparser/strparser.c:380](../net/strparser/strparser.c#L380) |
| `do_strp_work()` | [net/strparser/strparser.c:407](../net/strparser/strparser.c#L407) |

## net/ipv4/udp.c

| 符号 | 定义位置 |
|---|---|
| `udp_read_skb()` | [net/ipv4/udp.c:1995](../net/ipv4/udp.c#L1995) |

## net/ipv4/udp_bpf.c

| 符号 | 定义位置 |
|---|---|
| `udp_bpf_recvmsg()` | [net/ipv4/udp_bpf.c:63](../net/ipv4/udp_bpf.c#L63) |
| `udp_bpf_rebuild_protos()` | [net/ipv4/udp_bpf.c:135](../net/ipv4/udp_bpf.c#L135) |

## tools/testing/selftests/bpf/prog_tests/sockmap_basic.c

| 符号 | 定义位置 |
|---|---|
| `test_sockmap_same_sock()` | [tools/testing/selftests/bpf/prog_tests/sockmap_basic.c:923](../tools/testing/selftests/bpf/prog_tests/sockmap_basic.c#L923) |
| `test_sockmap_copied_seq()` | [tools/testing/selftests/bpf/prog_tests/sockmap_basic.c:1139](../tools/testing/selftests/bpf/prog_tests/sockmap_basic.c#L1139) |
| `test_sockmap_multi_channels()` | [tools/testing/selftests/bpf/prog_tests/sockmap_basic.c:1248](../tools/testing/selftests/bpf/prog_tests/sockmap_basic.c#L1248) |
| `test_sockmap_basic()` | [tools/testing/selftests/bpf/prog_tests/sockmap_basic.c:1365](../tools/testing/selftests/bpf/prog_tests/sockmap_basic.c#L1365) |

## tools/testing/selftests/bpf/progs/test_sockmap_pass_prog.c

| 符号 | 定义位置 |
|---|---|
| `prog_skb_verdict_ingress()` | [tools/testing/selftests/bpf/progs/test_sockmap_pass_prog.c:48](../tools/testing/selftests/bpf/progs/test_sockmap_pass_prog.c#L48) |
| `prog_skb_verdict_ingress_strp()` | [tools/testing/selftests/bpf/progs/test_sockmap_pass_prog.c:56](../tools/testing/selftests/bpf/progs/test_sockmap_pass_prog.c#L56) |

## 文档与配置

- [本仓库 sockmap 文档](../Documentation/bpf/map_sockmap.rst)。
- [net/Kconfig](../net/Kconfig)：`BPF_STREAM_PARSER`、`NET_SOCK_MSG`。
- [MAINTAINERS](../MAINTAINERS)：`BPF [L7 FRAMEWORK] (sockmap)`。
- [官方 sockmap 文档](https://docs.kernel.org/bpf/map_sockmap.html)：辅助核对 helper 方向和挂载接口；具体实现以本仓库为准。

## backlog 锁与记账补充索引

| 符号 | 定义位置 |
|---|---|
| `sk_forward_alloc_add()` | [include/net/sock.h:1125](../include/net/sock.h#L1125) |
| `sk_has_account()` | [include/net/sock.h:1571](../include/net/sock.h#L1571) |
| `sk_wmem_schedule()` | [include/net/sock.h:1577](../include/net/sock.h#L1577) |
| `sk_rmem_schedule()` | [include/net/sock.h:1600](../include/net/sock.h#L1600) |
| `sk_mem_reclaim()` | [include/net/sock.h:1618](../include/net/sock.h#L1618) |
| `sk_mem_charge()` | [include/net/sock.h:1637](../include/net/sock.h#L1637) |
| `sk_mem_uncharge()` | [include/net/sock.h:1644](../include/net/sock.h#L1644) |
| `lock_sock()` | [include/net/sock.h:1711](../include/net/sock.h#L1711) |
| `sock_owned_by_user()` | [include/net/sock.h:1810](../include/net/sock.h#L1810) |
| `__sk_mem_schedule()` | [net/core/sock.c:3451](../net/core/sock.c#L3451) |
| `__sk_mem_reclaim()` | [net/core/sock.c:3490](../net/core/sock.c#L3490) |
| `lock_sock_nested()` | [net/core/sock.c:3824](../net/core/sock.c#L3824) |
| `release_sock()` | [net/core/sock.c:3854](../net/core/sock.c#L3854) |
| `__release_sock()` | [net/core/sock.c:3243](../net/core/sock.c#L3243) |
| `skb_orphan()` | [include/linux/skbuff.h:3389](../include/linux/skbuff.h#L3389) |
| `schedule_delayed_work()` | [include/linux/workqueue.h:853](../include/linux/workqueue.h#L853) |

## 普通 TCP backlog 与最终引用释放补充索引

| 符号 | 定义位置 |
|---|---|
| `__sk_flush_backlog()` | [net/core/sock.c:3280](../net/core/sock.c#L3280) |
| `tcp_add_backlog()` | [net/ipv4/tcp_ipv4.c:1901](../net/ipv4/tcp_ipv4.c#L1901) |
| `tcp_eat_recv_skb()` | [net/ipv4/tcp.c:1614](../net/ipv4/tcp.c#L1614) |
| `tcp_try_coalesce()` | [net/ipv4/tcp_input.c:5244](../net/ipv4/tcp_input.c#L5244) |
| `skb_release_head_state()` | [net/core/skbuff.c:1169](../net/core/skbuff.c#L1169) |
| `__kfree_skb()` | [net/core/skbuff.c:1209](../net/core/skbuff.c#L1209) |
| `consume_skb()` | [net/core/skbuff.c:1439](../net/core/skbuff.c#L1439) |
| `__skb_clone()` | [net/core/skbuff.c:1607](../net/core/skbuff.c#L1607) |
| `skb_attempt_defer_free()` | [net/core/skbuff.c:7306](../net/core/skbuff.c#L7306) |
| `skb_unref()` | [include/linux/skbuff.h:1286](../include/linux/skbuff.h#L1286) |
| `skb_get()` | [include/linux/skbuff.h:2012](../include/linux/skbuff.h#L2012) |

## 共享状态、引用绑定与拆除清理补充索引

| 符号 | 定义位置 |
|---|---|
| `sock_map_del_link()` | [net/core/sock_map.c:142](../net/core/sock_map.c#L142) |
| `sock_map_unref()` | [net/core/sock_map.c:179](../net/core/sock_map.c#L179) |
| `sock_map_psock_get_checked()` | [net/core/sock_map.c:197](../net/core/sock_map.c#L197) |
| `__sock_map_delete()` | [net/core/sock_map.c:415](../net/core/sock_map.c#L415) |
| `sock_map_delete_elem()` | [net/core/sock_map.c:442](../net/core/sock_map.c#L442) |
| `maybe_wait_bpf_programs()` | [kernel/bpf/syscall.c:152](../kernel/bpf/syscall.c#L152) |
| `map_delete_elem()` | [kernel/bpf/syscall.c:1900](../kernel/bpf/syscall.c#L1900) |
| `__sk_psock_purge_ingress_msg()` | [net/core/skmsg.c:818](../net/core/skmsg.c#L818) |
| `__sk_psock_zap_ingress()` | [net/core/skmsg.c:833](../net/core/skmsg.c#L833) |
| `sk_psock_stop()` | [net/core/skmsg.c:854](../net/core/skmsg.c#L854) |
| `sk_psock_put()` | [include/linux/skmsg.h:509](../include/linux/skmsg.h#L509) |
| `sk_unused_reserved_mem()` | [include/net/sock.h:1605](../include/net/sock.h#L1605) |
| `__sk_mem_raise_allocated()` | [net/core/sock.c:3333](../net/core/sock.c#L3333) |
| `__sk_mem_reduce_allocated()` | [net/core/sock.c:3470](../net/core/sock.c#L3470) |
| `sock_error()` | [include/net/sock.h:2555](../include/net/sock.h#L2555) |
| `refcount_dec_and_test()` | [include/linux/refcount.h:448](../include/linux/refcount.h#L448) |
| `do_strp_work()` | [net/strparser/strparser.c:407](../net/strparser/strparser.c#L407) |
