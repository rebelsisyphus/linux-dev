# Agent Guidelines for QEMU Testing Workspace

## Overview

This workspace contains pre-built QEMU artifacts for running virtual machines with custom kernels and root filesystems. The directory structure:

```
.
├── bzImage          # Pre-compiled kernel image
├── initramfs/       # Initial ram filesystem (contains busybox, init script)
├── initrd.img       # Compressed initramfs
├── rootfs.img       # 20GB ext4 root filesystem (Ubuntu-based)
├── makeimg.sh       # Build initramfs from initramfs/ directory
├── mkext.sh         # Create and populate rootfs.img with debootstrap
├── qemu.sh          # Run QEMU with the configured kernel and disk
└── mkbk/            # Backup/working directory (see mkbk/)
```

## Build Commands

### Build initramfs
```bash
./makeimg.sh
```
Creates `initrd.img` from the `initramfs/` directory structure.

### Create root filesystem
```bash
sudo ./mkext.sh
```
Creates a 20GB `rootfs.img` and populates it with Ubuntu Noble using debootstrap. Requires root privileges.

### Run QEMU VM
```bash
./qemu.sh
```
Launches QEMU with:
- Kernel: `bzImage`
- Disk: `rootfs.img` (ext4 on `/dev/sda`)
- Serial console on ttyS0
- Nographic mode (Ctrl+A X to exit)

### Kernel development iteration
```bash
# 1. Mount rootfs to install new kernel/modules
sudo mount -o loop rootfs.img /mnt/rootfs
sudo cp arch/x86/boot/bzImage /mnt/rootfs/boot/
sudo umount /mnt/rootfs

# 2. Run with new kernel
./qemu.sh
```

## Code Style

This workspace contains shell scripts and minimal code. Style guidelines:

### Shell Scripts
- Use `#!/bin/sh` for POSIX compatibility
- Use `set -e` and `set -u` for safer scripts
- Quote variables: `"$var"` not `$var`
- Use `$(...)` for command substitution, not backticks
- Indent with 4 spaces

### Error Handling
- Check exit codes: `command || exit 1`
- Use descriptive error messages to stderr
- Validate inputs before use

### Common Patterns
```sh
#!/bin/sh
set -euo pipefail

# Validate prerequisites
command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "Error: QEMU not installed" >&2
    exit 1
}
```

## Important Notes

1. **rootfs.img is 20GB** - large file, handle with care
2. **Mount operations require sudo** - scripts should check for root
3. **Loop devices** - ensure /mnt/rootfs exists before mounting
4. **Ctrl+A X** - standard QEMU hotkey to exit nographic mode
5. **Serial console** - VM output goes to terminal, not graphical window

## QEMU Command Reference

Key flags used in `qemu.sh`:
- `-kernel`: Boot from kernel image
- `-hda`: IDE disk image
- `-append`: Kernel command line
- `-nographic`: Disable graphical output, use serial
