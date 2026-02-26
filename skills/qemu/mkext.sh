sudo apt install debootstrap qemu-user-static
dd if=/dev/zero of=rootfs.img bs=1M count=20480  # 2GB 镜像
mkfs.ext4 rootfs.img
sudo mkdir /mnt/rootfs
sudo mount -o loop rootfs.img /mnt/rootfs
sudo debootstrap --arch=amd64 noble /mnt/rootfs https://mirrors.aliyun.com/ubuntu/
sudo chroot /mnt/rootfs passwd
sudo umount /mnt/rootfs
