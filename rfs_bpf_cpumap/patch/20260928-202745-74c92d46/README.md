# CPUMAP + BPF RFS policy v1

分支：`develop/rfs_bpf_cpumap`。
基线：`9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`。
末端：`9de54e62c380`。

0001–0004 为原 CPUMAP 基础设施 v1，保持不变。本轮追加：

5. BPF socket CPU observer、XDP RFS/哈希回退策略、加载器。
6. 复用实际样例的七个 RFS 自测试子用例。
7. 使用方法、控制协议、性能成本及支持范围文档。

已有基础设施 v1 的工作树仅追加 0005–0007；从上述基线开始则按 series
应用全部七个补丁。apply-check.txt 记录独立 index 的顺序应用校验。
这些是本地开发提交和 format-patch 输出，没有对外发送。

完整 QEMU 验证：CONFIG_RPS=n、KASAN、lockdep，4 组/22 子用例通过，
零跳过；加载器停流卸载及 RPS/RFS 排他检查专项回归通过。
详见 ../../dev/rfs-validation.md；源码中的使用说明为
Documentation/bpf/xdp-rfs.rst。

策略依赖现有 ordered CPUMAP kfunc，没有新增内核选核逻辑。
覆盖常规 IPv4/IPv6 TCP 与已连接 UDP，使用 sk_storage 保存应用 CPU 提示，
首包起保持相同 domain，缺失提示只改变候选 CPU。加载器使用静态 CPU
集合，加载/卸载需停流；分片等特殊路径及自动热插拔管理仍待扩展。
实机性能尚未测量。原始 checkpatch/maintainer 输出和验证清单见本目录。
