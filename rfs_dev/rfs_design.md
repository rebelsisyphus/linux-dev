1.目的
当前rps和rfs的mask配置规则不够灵活，希望使用bpf程序重新实现一套rps, rfs的框架，支持自定义选核策略
2.实现
2.1 流信息记录
使用bpf_map重新实现rps_sock_flow_table和rps_dev_flow_table, 对应更新table的函数改为bpf func，在原本调用点添加hook点，使用bpf程序更新map
2.2 选核功能
get_rps_cpu, set_rps_cpu, enqueue_to_backlog等关键函数都在bpf程序中重新实现
2.3 迁核功能
更换cpu后，将当前流迁移到新核上要考虑旧核上流的处理，避免出现乱序
2.4 考虑原本rps，rfs的实现细节，不要犯rps,rfs之前犯过的错误
3.设计原则
3.1 不依赖CONFIG_RPS， config关闭时可以使用bpf程序来设置rfs
3.2 尽量复用原生代码，尽量减少侵入式修改（如使用bpf func包装原本函数）
4.实现要求
修改符合linux社区规范，commit拆分合理，commit msg描述适当，激进开发，不要做回退操作

---

5.详细设计（实现归档）

5.1 整体架构

  ┌──────────────┐     ┌──────────────┐     ┌──────────────────┐
  │ BPF RPS prog │     │ BPF RFS prog │     │   BPF maps      │
  │ (选核策略)    │     │ (流表更新)    │     │ (sock_flow_map  │
  │              │     │              │     │  dev_flow_map)  │
  └──────┬───────┘     └──────┬───────┘     └────────┬────────┘
         │                    │                       │
         ▼                    ▼                       │
  ┌──────────────────────────────────────────────────┐│
  │              RPS BPF 框架 (内核侧)                ││
  │                                                    ││
  │  收包路径 hook:                    流记录 hook:    ││
  │  netif_rx_internal()              sock_rps_       ◄┘
  │  netif_receive_skb_internal()     record_flow()
  │  netif_receive_skb_list_internal() sock_rps_
  │                                    delete_flow()
  │                                                    │
  │  BPF kfuncs:                                       │
  │  - bpf_rps_cpu_online()                           │
  │  - bpf_rps_input_queue_head()                     │
  │  - bpf_rps_num_possible_cpus()                    │
  └────────────────────────────────────────────────────┘
         │
         ▼
  ┌────────────────────────────────────────────────────┐
  │             原生网络栈                              │
  │  enqueue_to_backlog()  →  目标CPU input_pkt_queue  │
  │  napi_schedule_rps()   →  IPI/softirq 唤醒         │
  └────────────────────────────────────────────────────┘

5.2 BPF_PROG_TYPE_RPS 程序类型

5.2.1 用户态上下文 (uapi)

  struct bpf_rps_ctx {
      __u32 hash;            /* skb flow hash */
      __u32 ifindex;         /* net_device ifindex */
      __u32 rx_queue_index;  /* RX queue index */
      __u32 cpu;             /* current CPU (smp_processor_id()) */
  };

  程序返回值:
  - >= 0: 目标CPU号，包被enqueue_to_backlog到该CPU
  - < 0:  跳过BPF选核，回落到原生RPS或默认处理

5.2.2 挂载方式

  通过 bpf_link 挂载到 net_device:
  - 程序指针: net_device->rps_prog (RCU保护)
  - 静态键: rps_bpf_needed (无程序时零开销)
  - link类型: BPF_LINK_TYPE_RPS
  - attach类型: BPF_RPS
  - target: target_ifindex (网卡索引)

  每个net_device仅允许挂载一个RPS程序(EBUSY)。
  支持bpf_link update原子替换程序。

5.2.3 verifier ops

  - get_func_proto: 支持 BPF_FUNC_get_smp_processor_id,
    BPF_FUNC_get_numa_node_id, 以及 bpf_base_func_proto 基础helper
  - is_valid_access: 仅允许4字节读访问hash/ifindex/rx_queue_index/cpu
  - convert_ctx_access: 直接字段偏移映射(用户态/内核态使用同一结构)

5.3 Hook点设计

5.3.1 收包路径 hook (选核)

  在3个收包入口添加hook，处理顺序:

  BPF RPS hook → 原生RPS(CONFIG_RPS) → 默认smp_processor_id()

  │ netif_rx_internal():
  │   if (rps_bpf_needed)
  │     cpu = rps_bpf_skb_steering(skb)
  │     if (cpu >= 0) → enqueue_to_backlog(skb, cpu, &qtail)
  │   #ifdef CONFIG_RPS
  │     cpu = get_rps_cpu()  (原有逻辑)
  │   #else
  │     cpu = smp_processor_id()
  │
  │ netif_receive_skb_internal():
  │   if (rps_bpf_needed)
  │     cpu = rps_bpf_skb_steering(skb)
  │     if (cpu >= 0) → enqueue_to_backlog, return
  │   #ifdef CONFIG_RPS
  │     cpu = get_rps_cpu()  (原有逻辑)
  │   → __netif_receive_skb()
  │
  │ netif_receive_skb_list_internal():
  │   if (rps_bpf_needed)
  │     逐skb调用 rps_bpf_skb_steering
  │     匹配的skb从list移除并enqueue_to_backlog
  │   #ifdef CONFIG_RPS
  │     原有逻辑

  关键设计: enqueue_to_backlog() 是原生函数，BPF不重新实现，
  而是复用原生实现(满足设计原则3.2)。

5.3.2 RFS流记录 hook

  在sock_rps_record_flow / sock_rps_record_flow_hash /
  sock_rps_delete_flow的调用点添加BPF hook:

  │ sock_rps_record_flow(sk):
  │   rfs_bpf_record_flow(sk)        ← BPF hook (不依赖CONFIG_RPS)
  │   #ifdef CONFIG_RPS
  │     _sock_rps_record_flow(sk)    ← 原生实现
  │
  │ sock_rps_record_flow_hash(hash):
  │   rfs_bpf_record_flow_hash(hash) ← BPF hook
  │   #ifdef CONFIG_RPS
  │     _sock_rps_record_flow_hash(hash)
  │
  │ sock_rps_delete_flow(sk):
  │   rfs_bpf_delete_flow(sk)        ← BPF hook
  │   #ifdef CONFIG_RPS
  │     _sock_rps_delete_flow(sk)

  BPF hook位于CONFIG_RPS guard之前，确保CONFIG_RPS关闭时仍可使用。
  两者共存时BPF先调用，允许渐进迁移。

  调用点(TCP/UDP/SCTP recvmsg等)无需修改，自动触发BPF hook。

5.4 BPF kfuncs

  为RPS BPF程序提供以下kfuncs，用于实现迁核逻辑:

  │ u32 bpf_rps_cpu_online(u32 cpu)
  │   检查CPU是否在线，选核时必须验证
  │
  │ u32 bpf_rps_input_queue_head(u32 cpu)
  │   读取per-CPU softnet_data.input_queue_head
  │   用于迁核时判断旧CPU上的包是否已处理完:
  │   if (input_queue_head - last_qtail >= 0) → 可安全迁核
  │   (仅在CONFIG_RPS启用时返回有效值，否则返回0)
  │
  │ int bpf_rps_num_possible_cpus(void)
  │   返回num_possible_cpus()，用于BPF程序中的CPU遍历

5.5 迁核功能

  原生RFS的迁核保证(参考get_rps_cpu):
    - 检查目标CPU与当前CPU不同
    - 检查当前CPU的input_queue_head已超过流的last_qtail
      (旧CPU上该流的包已全部出队，保证不乱序)
    - 更新rflow->cpu和rflow->last_qtail

  BPF框架中的迁核实现方式:
    BPF程序通过bpf_map维护 per-flow 的 {cpu, last_qtail} 状态。
    选核时:
    1. 从sock_flow_map查出流期望CPU (由RFS记录程序更新)
    2. 从dev_flow_map查出流当前CPU和last_qtail
    3. 调用bpf_rps_input_queue_head(current_cpu)判断是否可迁移
    4. 若可迁移: 更新dev_flow_map，返回期望CPU
    5. 若不可迁移: 返回当前CPU(保证有序)

  示例BPF程序逻辑:
    SEC("rps")
    int rps_select_cpu(struct bpf_rps_ctx *ctx)
    {
        struct flow_entry *fe;
        __u32 hash = ctx->hash;
        __u32 cur_cpu = ctx->cpu;
        int target_cpu;

        // 查sock flow表: 应用期望的CPU
        fe = bpf_map_lookup_elem(&sock_flow_map, &hash);
        if (!fe)
            return -1; // 跳过，走原生逻辑

        target_cpu = fe->cpu;

        // 查dev flow表: 当前CPU
        fe = bpf_map_lookup_elem(&dev_flow_map, &hash);
        if (fe && fe->cpu != target_cpu) {
            // 检查旧CPU队列是否已排空
            __u32 qhead = bpf_rps_input_queue_head(fe->cpu);
            if ((__s32)(qhead - fe->last_qtail) >= 0) {
                // 安全迁核
                fe->cpu = target_cpu;
                fe->last_qtail = bpf_rps_input_queue_head(target_cpu);
            } else {
                target_cpu = fe->cpu; // 仍用旧CPU
            }
        }

        if (bpf_rps_cpu_online(target_cpu))
            return target_cpu;

        return -1;
    }

5.6 文件变更清单

  新增文件:
  │ include/net/rps_bpf.h    RPS BPF接口声明 + 空操作fallback
  │ net/core/rps_bpf.c       核心实现: verifier ops, link,
                            hook函数, kfuncs, RFS记录

  修改文件:
  │ include/uapi/linux/bpf.h  BPF_PROG_TYPE_RPS, BPF_RPS,
                             BPF_RFS_RECORD, BPF_LINK_TYPE_RPS,
                             struct bpf_rps_ctx, bpf_link_info.rps
  │ include/linux/bpf_types.h BPF_PROG_TYPE/BPF_LINK_TYPE注册
  │ include/linux/netdevice.h net_device添加rps_prog字段,
                             CACHELINE_ASSERT_GROUP_SIZE更新
  │ include/net/rps.h         RFS流记录hook调用点
  │ net/core/dev.c            3个收包入口hook, rps_bpf.h include
  │ net/core/Makefile         rps_bpf.o编译
  │ kernel/bpf/syscall.c      link_create RPS case分发

5.7 Commit拆分

  Commit 1: bpf: Add BPF_PROG_TYPE_RPS program type and uapi definitions
    - include/uapi/linux/bpf.h
    - include/linux/bpf_types.h

  Commit 2: bpf: Implement RPS BPF verifier ops, hook infrastructure and link support
    - include/linux/netdevice.h
    - include/net/rps_bpf.h (新增)
    - net/core/rps_bpf.c (新增)
    - net/core/Makefile
    - net/core/dev.c
    - kernel/bpf/syscall.c

  Commit 3: bpf: Add RFS flow recording BPF hook points independent of CONFIG_RPS
    - include/net/rps.h

5.8 待完善

  - RFS记录程序的per-CPU prog挂载机制(当前使用DEFINE_PER_CPU)
  - RFS记录程序的bpf_link attach接口
  - bpf_rps_input_queue_head在CONFIG_RPS关闭时的替代方案
    (需将input_queue_head从CONFIG_RPS guard中移出)
  - bpf_prog_test_run for BPF_PROG_TYPE_RPS
  - selftests
