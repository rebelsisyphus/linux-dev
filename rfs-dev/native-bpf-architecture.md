# 原生 RFS 的 BPF CPU 策略架构

对应四个提交 `e37739322a0c` → `9f3cd6a8fd24`。BPF 提供期望 CPU；
原生 RFS 决定何时改变 active CPU，并负责实际入队及队列进度。

可直接查看 [SVG 架构图](native-bpf-architecture.svg) 或
[PNG 架构图](native-bpf-architecture.png)，也可编辑
[Mermaid 源文件](native-bpf-architecture.mmd)。

## 三种 CPU 各自表示什么

| 名称 | 来源 | 含义 |
| --- | --- | --- |
| `app_cpu_hint` | 原生全局 RFS 提示表 | 应用访问 socket 时记录的 CPU 提示；不被 BPF 策略覆盖 |
| desired CPU | BPF 返回值经过内核校验，或原生提示 | 当前希望使用的 CPU；返回 0 表示 DEFAULT，CPU n 编码为 n + 1 |
| `active_cpu` | RX 队列的原生 RFS 槽位 | 当前处理该槽位流量的 CPU；是否切换取决于原生迁移条件 |

拓扑策略使用已有提示判断应用所在 cluster / NUMA node，并可保留仍然合适的
active CPU。全局 hash 表和 RX 槽位仍可能碰撞，它们不是精确 socket 归属记录。

## 控制面与数据面

<!-- The diagram below is synchronized with native-bpf-architecture.mmd. -->

```mermaid
flowchart TB
    subgraph CONTROL["控制面：构建并发布完整策略版本"]
        TOPO["sysfs CPU 拓扑 + 用户配置<br/>process / cluster / NUMA / 候选 CPU / SMT 排除"]
        LOAD["rfs_policy 用户态工具<br/>加载程序 → 填充拓扑 map → freeze"]
        LINK["struct_ops BPF link<br/>每个 netdevice 一个绑定<br/>pin / expected-old 原子 update / detach"]
        TOPO --> LOAD --> LINK
    end

    subgraph NATIVE["原生状态：内核负责维护"]
        APP["应用访问 socket"]
        HINT["全局 RFS 提示表<br/>保留原始 app_cpu_hint"]
        FLOW["每个 RX 队列的 RFS 槽位<br/>active_cpu / last_qtail"]
        QUEUE["每 CPU backlog<br/>真实入队 tail / 已处理 head"]
        APP --> HINT
    end

    subgraph RX["数据面：get_rps_cpu() 及原生接收路径"]
        SKB["接收 skb"]
        LOOKUP{"原生 RFS 查询命中？"}
        GATE{"有绑定策略且提示有效？"}
        BPF["只读 select_cpu(ctx)<br/>读取本代拓扑 map<br/>返回 0 或 CPU + 1"]
        VAL["内核校验建议 CPU<br/>范围 / possible / online<br/>DEFAULT 或无效 → app_cpu_hint"]
        NATIVECPU["期望 CPU = app_cpu_hint"]
        GUARD["原生迁移检查<br/>新旧 CPU 不同，并且旧 CPU 未设置、<br/>离线或 head 已追上 last_qtail，才允许切换"]
        ACTIVE["允许切换：set_rps_cpu()<br/>否则：沿用 active_cpu"]
        FINAL{"最终原生 CPU 可用？"}
        FALLBACK["原有 RPS / 本地接收"]
        ENQ["enqueue_to_backlog()<br/>成功入队后保存真实 last_qtail"]
        STACK["原生协议栈处理"]
        SKB --> LOOKUP
        LOOKUP -->|命中| GATE
        LOOKUP -->|未命中或表未启用| FALLBACK
        GATE -->|是| BPF --> VAL --> GUARD
        GATE -->|否| NATIVECPU --> GUARD
        GUARD --> ACTIVE --> FINAL
        FINAL -->|可用| ENQ --> STACK
        FINAL -->|不可用| FALLBACK
        FALLBACK -->|RPS 选到 CPU| ENQ
        FALLBACK -->|未选到 CPU| STACK
    end

    HINT -.->|应用提示| LOOKUP
    FLOW -.->|读取槽位| LOOKUP
    FLOW -.->|迁移边界| GUARD
    QUEUE -.->|input_queue_head| GUARD
    LINK -.->|RTNL + RCU 发布策略指针| GATE
    LOAD -.->|本代只读配置| BPF
    ACTIVE -.->|原生更新| FLOW
    ENQ -.->|原生入队与进度| QUEUE

    classDef control fill:#eff6ff,stroke:#2563eb,color:#12305b;
    classDef policy fill:#f0fdfa,stroke:#0d9488,color:#134e4a;
    classDef native fill:#f8fafc,stroke:#64748b,color:#172b4d;
    classDef guard fill:#fff7ed,stroke:#d97706,color:#7c2d12;
    class TOPO,LOAD,LINK control;
    class BPF,VAL policy;
    class APP,HINT,FLOW,QUEUE,SKB,LOOKUP,GATE,NATIVECPU,ACTIVE,FINAL,FALLBACK,ENQ,STACK native;
    class GUARD guard;
```

实线表示主要执行路径，虚线表示配置发布或状态读写。图中的 RPS / 本地接收
是原有回退路径的概括：RPS 选到 CPU 时仍使用 backlog，否则保持本地接收。
原生迁移条件中的队列比较保持为有符号 `head - last_qtail >= 0`，图没有表示
一个新的、独立于原生 RFS 的保序机制。

控制面先准备完整程序及 map，再通过 link 发布；数据面不会读到一半更新的
拓扑。拓扑 map 同时使用 `BPF_F_RDONLY_PROG` 和用户态 freeze，统计 map 独立。
统计仅记录策略调用、建议和分支，不能解释为成功迁移或成功入队。

## 更新与设备删除

```mermaid
sequenceDiagram
    participant U as 用户态管理程序
    participant L as struct_ops link
    participant N as netdevice / RTNL
    participant R as 接收路径 / RCU reader
    R->>N: RCU 读取一个 ops 指针
    N-->>R: 旧版完整策略
    U->>U: 加载新程序，填充并冻结新 map
    U->>L: BPF_LINK_UPDATE + expected-old map
    L->>N: RTNL 下校验设备和权限，发布新 ops
    N-->>L: 释放 RTNL
    R->>R: 旧回调完成，退出 RCU 读侧
    L->>L: 等待 RCU 宽限期，随后释放旧 map 引用
    L-->>U: 更新完成
    R->>N: 后续报文读取 ops
    N-->>R: 新版完整策略
    Note over N,R: NETDEV_UNREGISTER 撤销设备侧指针
    Note over L,N: 不在通知中调用通用 link detach；不持设备引用
    U->>L: 再次 update 失效绑定
    L-->>U: ENODEV
    Note over U,L: 旧 link / map 保留到显式释放
```

关键实现见 [内核钩子](../net/core/bpf_rfs.c)、
[get_rps_cpu() 接入点](../net/core/dev.c)、
[只读接口](../include/net/bpf_rfs.h) 和
[示例策略](../samples/bpf/rfs_policy.bpf.c)。
实际测试范围见 [实现报告](implementation-report.md)。
