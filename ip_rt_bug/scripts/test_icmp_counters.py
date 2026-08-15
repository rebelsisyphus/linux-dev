#!/usr/bin/env python3
import fcntl, struct, os, time

os.system("ip netns add testns2 2>/dev/null")
os.system("ip netns exec testns2 bash -c 'echo 1 > /proc/sys/net/ipv4/ip_forward; echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter; echo 0 > /proc/sys/net/ipv4/conf/default/rp_filter'")

cmd = "ip netns exec testns2 cat /proc/net/snmp | grep Icmp:"
print("before:")
os.system(cmd)

os.system("ip netns exec testns2 bash -c 'ip tuntap add tun0 mode tap; ip tuntap add tun1 mode tap; ip addr add 10.0.0.1/24 dev tun0; ip addr add 10.0.1.1/24 dev tun1; ip link set tun0 up; ip link set tun1 up; ip route add 10.0.1.0/24 dev tun1'")

TUNSETIFF = 0x400454ca
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

fd = os.open("/dev/net/tun", os.O_RDWR)
ifr = b"tun0\x00" + b"\x00" * (16 - 5) + struct.pack("H", IFF_TAP | IFF_NO_PI) + b"\x00" * (40 - 16 - 2)
fcntl.ioctl(fd, TUNSETIFF, ifr)

pkt = bytearray(60)
pkt[0:6] = b"\xff\xff\xff\xff\xff\xff"
pkt[6:12] = b"\x02\x00\x00\x00\x00\x00"
pkt[12:14] = b"\x08\x00"

ip = bytearray(20)
ip[0] = 0x45
ip[1] = 0
ip[2:4] = (40).to_bytes(2, "big")
ip[4:6] = b"\x00\x00"
ip[6:8] = b"\x00\x00"
ip[8] = 1  # TTL
ip[9] = 6  # TCP
ip[10:12] = b"\x00\x00"
ip[12:16] = bytes([10, 0, 0, 3])
ip[16:20] = bytes([10, 0, 1, 2])

cs = 0
for i in range(0, 20, 2):
    cs += (ip[i] << 8) + ip[i + 1]
while cs > 0xffff:
    cs = (cs & 0xffff) + (cs >> 16)
ip[10:12] = (~cs & 0xffff).to_bytes(2, "big")

pkt[14:34] = ip
tcp = bytearray(20)
tcp[12] = 0x50  # doff=5
tcp[13] = 0x02  # SYN
pkt[34:54] = tcp

for _ in range(100):
    os.write(fd, bytes(pkt))

os.close(fd)
time.sleep(1)
print("after:")
os.system(cmd)
