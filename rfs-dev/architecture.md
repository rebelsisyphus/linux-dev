# eBPF RPS/RFS Steering Architecture

## End-to-end block flow

```mermaid
flowchart TD
    subgraph Attach[Program attach points]
        AttachXDP[XDP attach on RX netdev]
        AttachCPUMap[CPUMAP secondary XDP program]
        AttachSockops[Sockops attach on cgroup]
        AttachRecv[Explicit kprobe attach on tcp_recvmsg]
    end

    subgraph RX[Packet receive data path]
        NIC[NIC or skb-mode RX] --> AttachXDP
        AttachXDP --> Parse[Parse Ethernet/VLAN/IP/TCP/UDP]
        Parse --> Key[Build flow_key and software flow hash]
        Key --> Lookup{flow_state entry exists?}
        Lookup -->|no| RPSHash[RPS fallback: hash % nr_cpus]
        RPSHash --> RPSSlot[cpu_slots lookup]
        RPSSlot --> InitState[Create flow_state active_cpu=desired_cpu=RPS CPU]
        Lookup -->|yes| ReadState[Read active_cpu, desired_cpu, last_qtail]
        InitState --> SelectActive[Select active CPU]
        ReadState --> Migration{desired_cpu differs?}
        Migration -->|no| SelectActive
        Migration -->|yes| Drain{old CPU queue drained?}
        Drain -->|no| KeepOld[Keep old active_cpu]
        Drain -->|yes| Commit[active_cpu = desired_cpu]
        KeepOld --> SelectActive
        Commit --> SelectActive
        SelectActive --> Tail[cpu_queue_tail++ and save last_qtail]
        Tail --> Redirect[bpf_redirect_map to cpu_map]
        Redirect --> AttachCPUMap
        AttachCPUMap --> Head[cpu_queue_head++ on target CPU dequeue]
        Head --> Stack[Continue kernel networking stack]
    end

    subgraph Record[Record flow CPU hints]
        Established[TCP established event] --> AttachSockops
        AttachSockops --> SockKey[Build reverse receive flow_key]
        AppRecv[Userspace recvmsg runs on current CPU] --> AttachRecv
        AttachRecv --> RecvEnter[recv_enter++]
        RecvEnter --> SockRead[Read struct sock family/addrs/ports]
        SockRead --> RecvKey[Build receive flow_key]
        SockRead -->|not IPv4/IPv6| BadFamily[recv_bad_family++]
        SockKey --> Choose[choose_rfs_cpu]
        RecvKey --> Choose
        Choose --> Mode{strategy}
        Mode -->|process| ProcessCPU[Use process CPU]
        Mode -->|cluster| ClusterCPU[Pick another allowed same-cluster CPU]
        Mode -->|numa| NUMACPU[Pick another allowed same-NUMA CPU]
        ProcessCPU --> UpdateDesired[flow_state.desired_cpu update]
        ClusterCPU --> UpdateDesired
        NUMACPU --> UpdateDesired
        UpdateDesired --> RecvUpdate[recv_update++ for tcp_recvmsg path]
    end

    UpdateDesired -. next packets use desired CPU after drain .-> Lookup
    Head -. proves old queue drained .-> Drain
```

The key split is deliberate:

- XDP is the packet steering point. It never switches a flow directly to a new
  CPU unless the old CPUMAP queue has drained.
- Sockops and `tcp_recvmsg` are record-flow points. They only update
  `desired_cpu`.
- `xdp/cpumap` is the dequeue progress point. It advances `cpu_queue_head`,
  which lets XDP commit a pending migration without reordering packets.

## Data plane

```mermaid
flowchart LR
    NIC[NIC RX queue] --> XDP[XDP program]
    XDP --> Parse[Parse L2/L3/L4 flow key]
    Parse --> FlowLookup{flow_state hit?}
    FlowLookup -->|yes| RFS[RFS target CPU]
    FlowLookup -->|no| Hash[Flow hash]
    Hash --> RPSSlot[cpu_slots lookup]
    RPSSlot --> RPS[RPS target CPU]
    RFS --> Drain{old CPU queue drained?}
    Drain -->|yes| Switch[Commit desired CPU]
    Drain -->|no| Stay[Keep active CPU]
    Switch --> CPUMAP[CPUMAP redirect]
    Stay --> CPUMAP
    RPS --> CPUMAP
    CPUMAP --> CM[CPUMAP program updates cpu_queue_head]
    CM --> CPU[Target CPU networking path]
```

## Control plane

```mermaid
flowchart TD
    Loader[Userspace loader] --> CPUSet[Parse -c CPU set]
    Loader --> Sysfs[Read sysfs topology]
    Sysfs --> Cluster[cluster candidate slots]
    Sysfs --> NUMA[NUMA candidate slots]
    CPUSet --> Cluster
    CPUSet --> NUMA
    Loader --> CPUMAP[cpu_map]
    Loader --> Slots[cpu_slots]
    Loader --> Config[config]
    Cluster --> ClusterMaps[cluster_cpu_counts / slots]
    NUMA --> NUMAMaps[numa_cpu_counts / slots]

    Cgroup[cgroup TCP sockets] --> Sockops[sockops program]
    Config --> Sockops
    ClusterMaps --> Sockops
    NUMAMaps --> Sockops
    Recv[tcp_recvmsg explicit kprobe] --> FlowState[flow_state desired CPU]
    Sockops --> FlowState
```

## RFS strategy selection

```mermaid
flowchart TD
    Event[TCP established or tcp_recvmsg event] --> AppCPU[Current CPU as process CPU]
    AppCPU --> Mode{RFS strategy}
    Mode -->|process| Same[Use process CPU]
    Mode -->|cluster| Cluster{Alternate cluster CPU exists?}
    Mode -->|numa| NUMA{Alternate NUMA CPU exists?}
    Cluster -->|yes| ClusterCPU[Pick same-cluster CPU by flow hash]
    Cluster -->|no| Same
    NUMA -->|yes| NUMACPU[Pick same-NUMA CPU by flow hash]
    NUMA -->|no| Same
    Same --> Allowed{CPU allowed by -c?}
    ClusterCPU --> Store[Update flow_state desired_cpu]
    NUMACPU --> Store
    Allowed -->|yes| Store
    Allowed -->|no| Fallback[Use first RPS CPU]
    Fallback --> Store
```

## In-order migration

```mermaid
sequenceDiagram
    participant XDP as XDP ingress
    participant Old as old CPU CPUMAP queue
    participant State as flow_state
    participant New as new CPU CPUMAP queue

    XDP->>State: read active_cpu, desired_cpu, last_qtail
    alt desired_cpu differs and old head < last_qtail
        XDP->>Old: enqueue packet to active_cpu
        XDP->>State: update last_qtail
    else old head >= last_qtail
        XDP->>State: active_cpu = desired_cpu
        XDP->>New: enqueue packet to new active_cpu
        XDP->>State: update last_qtail
    end
    Old->>State: cpumap prog advances cpu_queue_head
    New->>State: cpumap prog advances cpu_queue_head
```

## Map relationships

```text
config
  nr_cpus
  rfs_strategy

cpu_slots[slot] -> cpu
cpu_map[cpu] -> cpumap entry
cpu_queue_head[cpu] -> dequeue progress
cpu_queue_tail[cpu] -> enqueue progress

allowed_cpus[cpu] -> bool

cluster_cpu_counts[process_cpu] -> count
cluster_cpu_slots[process_cpu * MAX_CPUS + slot] -> cpu

numa_cpu_counts[process_cpu] -> count
numa_cpu_slots[process_cpu * MAX_CPUS + slot] -> cpu

flow_state[flow_key] -> active_cpu, desired_cpu, last_qtail
stats[counter] -> per-CPU counter
```

## Receive-hook diagnostics

```mermaid
flowchart TD
    Recv[tcp_recvmsg kprobe entry] --> Enter[recv_enter++]
    Enter --> Family{IPv4 or IPv6 sock?}
    Family -->|no| Bad[recv_bad_family++]
    Family -->|yes| Key[Build receive flow key]
    Key --> Strategy[choose_rfs_cpu]
    Strategy --> Update[flow_state.desired_cpu update]
    Update --> Done[recv_update++]
```

The test expectation for normal IPv4/IPv6 TCP receive traffic is:

```text
recv_enter > 0
recv_update == recv_enter
recv_bad_family == 0
```
