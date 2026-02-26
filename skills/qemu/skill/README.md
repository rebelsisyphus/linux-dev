# QEMU测试环境

完整的QEMU虚拟化测试环境，支持SSH免密登录和共享目录。

## 快速开始

```bash
# 1. 一键部署（需要root权限）
sudo ./install.sh

# 2. 确保有内核镜像
# 如果没有，可以从当前系统复制或下载：
# cp /boot/vmlinuz-$(uname -r) ./bzImage

# 3. 启动QEMU
./qemu.sh

# 4. 等待30-40秒VM启动完成

# 5. 连接SSH（免密码）
ssh root@localhost -p 2222
```

## 文件说明

| 文件 | 说明 |
|------|------|
| `qemu.sh` | 启动QEMU VM |
| `monitor.sh` | Panic监控脚本 |
| `test.sh` | 一键执行测试 |
| `mkext.sh` | 创建rootfs镜像 |
| `setup-rootfs.sh` | 配置rootfs（SSH、共享目录） |
| `install.sh` | 一键部署所有内容 |
| `bzImage` | 内核镜像（需用户提供） |
| `rootfs.img` | 20GB Ubuntu rootfs（自动生成） |
| `shared/` | 共享目录（宿主机<->VM） |

## VM配置

- **OS**: Ubuntu 24.04 LTS
- **用户名**: root
- **密码**: 空（免密码登录）
- **SSH**: localhost:2222
- **共享目录**: `./shared` <-> `/mnt/shared`
- **磁盘**: 20GB

## 常用命令

```bash
# 启动VM
./qemu.sh

# 启动panic监控
./monitor.sh

# 一键执行测试
./test.sh

# 停止VM
kill $(cat /tmp/qemu.pid)

# 查看串口日志
tail -f serial.log
```

## 故障排查

### QEMU无法启动
```bash
# 检查进程
pkill -9 qemu
rm -f /tmp/qemu.pid

# 重新启动
./qemu.sh
```

### SSH连接失败
```bash
# 等待更长时间（首次启动需要30-40秒）
sleep 40

# 检查端口
ss -tlnp | grep 2222

# 查看日志
tail -f serial.log
```

### 共享目录未挂载
```bash
# 在VM中手动挂载
mount -t 9p -o trans=virtio shared /mnt/shared
```

## 自定义测试

在 `shared/` 目录中创建测试脚本：

```bash
# 在宿主机创建测试脚本
cat > shared/mytest.sh << 'EOF'
#!/bin/bash
echo "Running my test..."
# 你的测试代码
echo "Test completed!"
EOF
chmod +x shared/mytest.sh

# 在VM中执行
ssh root@localhost -p 2222 "/mnt/shared/mytest.sh"
```

## 网络配置

VM使用QEMU user模式网络：
- VM IP: 10.0.2.15
- 网关: 10.0.2.2
- DNS: 10.0.2.3
- 端口转发: localhost:2222 -> VM:22

## 注意事项

1. **rootfs.img 20GB** - 确保磁盘空间充足
2. **首次启动较慢** - 需要30-40秒启动时间
3. **需要内核镜像** - 提供你自己的bzImage或从系统复制
4. **共享目录** - 使用9p virtio协议

## 许可证

MIT License
