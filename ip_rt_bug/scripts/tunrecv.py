#!/usr/bin/env python3
"""Create a tap interface and read packets to verify packet injection."""
import fcntl
import os
import struct
import time
import subprocess

TUNSETIFF = 0x400454ca
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

# Create tun interface via ip tuntap
subprocess.run(["ip", "tuntap", "add", "mode", "tap", "tunrecv"], check=False)
subprocess.run(["ip", "addr", "add", "10.0.0.1/24", "dev", "tunrecv"], check=False)
subprocess.run(["ip", "link", "set", "tunrecv", "up"], check=False)

fd = os.open("/dev/net/tun", os.O_RDWR)
ifr = b"tunrecv\x00" + b"\x00" * (40 - 8) + struct.pack("H", IFF_TAP | IFF_NO_PI)
# struct ifreq on x86_64 is 40 bytes; flags at offset 16
ifr = b"tunrecv\x00" + b"\x00" * 9 + struct.pack("H", IFF_TAP | IFF_NO_PI) + b"\x00" * 22
fcntl.ioctl(fd, TUNSETIFF, ifr)

print("tunrecv attached, listening...")
count = 0
for _ in range(50):
    try:
        pkt = os.read(fd, 2048)
        if pkt:
            count += 1
            print(f"received {len(pkt)} bytes: {pkt[:64].hex()}")
    except BlockingIOError:
        time.sleep(0.1)

print(f"total packets received: {count}")
