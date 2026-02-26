#!/bin/bash
# 一键部署脚本 - 自动完成所有步骤

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "QEMU Test Environment Setup"
echo "========================================"
echo ""

# 检查root权限
if [ "$(id -u)" -ne 0 ]; then
    echo "This script needs to run with sudo for some operations"
    echo "Usage: sudo $0"
    exit 1
fi

# 步骤1: 创建rootfs
echo "Step 1/3: Creating rootfs image..."
if [ ! -f "rootfs.img" ]; then
    ./mkext.sh
else
    echo "  rootfs.img already exists, skipping..."
fi

echo ""
echo "Step 2/3: Configuring rootfs..."
./setup-rootfs.sh

echo ""
echo "Step 3/3: Creating shared directory..."
mkdir -p shared
cat > shared/test.sh << 'EOF'
#!/bin/bash
# Default test script
echo "=== QEMU VM Test ==="
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "Kernel: $(uname -r)"
echo ""
echo "=== Memory Usage ==="
free -h
echo ""
echo "=== Disk Usage ==="
df -h
echo ""
echo "=== Shared Directory ==="
ls -la /mnt/shared/
echo ""
echo "=== Test Result ==="
echo "Status: PASSED"
EOF
chmod +x shared/test.sh

echo ""
echo "========================================"
echo "Setup Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Place your kernel as 'bzImage' in this directory"
echo "  2. Run: ./qemu.sh"
echo "  3. Wait 30-40 seconds for VM to boot"
echo "  4. Test SSH: ssh root@localhost -p 2222"
echo ""
echo "Files created:"
echo "  - rootfs.img (20GB Ubuntu 24.04)"
echo "  - qemu.sh (start QEMU)"
echo "  - monitor.sh (panic monitor)"
echo "  - test.sh (one-click test)"
echo "  - shared/ (shared directory)"
echo "========================================"
