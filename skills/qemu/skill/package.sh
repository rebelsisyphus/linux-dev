#!/bin/bash
# 打包QEMU测试环境skill

set -e

VERSION="1.0.0"
PACKAGE_NAME="qemu-test-env-skill-${VERSION}"

echo "Packaging QEMU Test Environment Skill..."
echo "Version: $VERSION"
echo ""

# 创建临时目录
TMP_DIR=$(mktemp -d)
PACKAGE_DIR="$TMP_DIR/$PACKAGE_NAME"

mkdir -p "$PACKAGE_DIR"

# 复制文件
cp -v qemu.sh monitor.sh test.sh mkext.sh setup-rootfs.sh install.sh "$PACKAGE_DIR/"
cp -v README.md skill.json SKILL_USAGE.md "$PACKAGE_DIR/"

# 创建压缩包
cd "$TMP_DIR"
tar -czf "${PACKAGE_NAME}.tar.gz" "$PACKAGE_NAME"

# 移动回当前目录
mv "${PACKAGE_NAME}.tar.gz" "$(dirname "$0")/"

# 清理
rm -rf "$TMP_DIR"

echo ""
echo "=========================================="
echo "Package created: ${PACKAGE_NAME}.tar.gz"
echo "=========================================="
echo ""
echo "To use this skill in a new directory:"
echo ""
echo "  # 方法1: 解压到新目录"
echo "  mkdir ~/my-qemu-project"
echo "  cd ~/my-qemu-project"
echo "  tar -xzf ${PACKAGE_NAME}.tar.gz"
echo "  mv ${PACKAGE_NAME}/* ."
echo "  sudo ./install.sh"
echo ""
echo "  # 方法2: 复制到opencode skills目录"
echo "  mkdir -p ~/.config/opencode/skills/qemu-test-env"
echo "  tar -xzf ${PACKAGE_NAME}.tar.gz -C ~/.config/opencode/skills/qemu-test-env/"
echo "  cd ~/my-qemu-project"
echo "  opencode /apply-skill qemu-test-env"
echo ""
echo "=========================================="
