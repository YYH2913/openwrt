#!/usr/bin/env python3

import ipaddress
import socket
import struct
import subprocess
import sys
import tempfile
import time


DST = bytes.fromhex("001122334455")
SRC = bytes.fromhex("020000000001")
SOL_PACKET = getattr(socket, "SOL_PACKET", 263)
PACKET_AUXDATA = 8
TP_STATUS_VLAN_VALID = 1 << 4
TP_STATUS_VLAN_TPID_VALID = 1 << 6


def run(*arguments):
    subprocess.run(arguments, check=True, stdout=subprocess.DEVNULL)


def install_direct_counter(nft_command):
    rule = """insert rule bridge airoha_ctc classification_lan1 \
@ll,96,16 { 0x8100, 0x88a8, 0x9100 } \
@ll,128,16 { 0x8100, 0x88a8, 0x9100 } \
@ll,160,16 == 0x0800 @ll,176,4 == 4 \
@ll,224,16 & 0x3fff == 0 @ll,248,8 == 6 @ll,180,4 == 5 \
ether daddr == 00:11:22:33:44:55 @ll,112,3 == 3 \
@ll,116,12 == 100 @ll,352,16 == 443 \
counter comment \"packet-test-direct\"
"""
    with tempfile.NamedTemporaryFile(mode="w", encoding="ascii") as rules:
        rules.write(rule)
        rules.flush()
        run(nft_command, "-f", rules.name)


def counter_packets(nft_command, comment):
    output = subprocess.check_output(
        [
            nft_command,
            "-j",
            "list",
            "chain",
            "bridge",
            "airoha_ctc",
            "classification_lan1",
        ],
        text=True,
    )
    import json

    for item in json.loads(output)["nftables"]:
        rule = item.get("rule")
        if not rule or rule.get("comment") != comment:
            continue
        for expression in rule["expr"]:
            if "counter" in expression:
                return expression["counter"]["packets"]
    raise RuntimeError(f"diagnostic counter {comment} was not found")


def direct_counter_packets(nft_command):
    return counter_packets(nft_command, "packet-test-direct")


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ipv4_tcp(source="192.0.2.1", destination="198.51.100.1"):
    tcp = struct.pack("!HHLLBBHHH", 12345, 443, 0, 0, 5 << 4, 2, 8192, 0, 0)
    header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        20 + len(tcp),
        1,
        0,
        64,
        6,
        0,
        socket.inet_aton(source),
        socket.inet_aton(destination),
    )
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return header + tcp


def ipv6_udp(source, destination):
    udp = struct.pack("!HHHH", 12345, 443, 8, 0)
    return (
        struct.pack("!IHBB", 6 << 28, len(udp), 17, 64)
        + ipaddress.IPv6Address(source).packed
        + ipaddress.IPv6Address(destination).packed
        + udp
    )


def ipv6_tcp(source, destination):
    tcp = struct.pack("!HHLLBBHHH", 12345, 443, 0, 0, 5 << 4, 2, 8192, 0, 0)
    return (
        struct.pack("!IHBB", 6 << 28, len(tcp), 6, 64)
        + ipaddress.IPv6Address(source).packed
        + ipaddress.IPv6Address(destination).packed
        + tcp
    )


def qinq(payload_type, payload, outer_type, pcp):
    outer_tci = (pcp << 13) | 100
    inner_tci = 200
    frame = (
        DST
        + SRC
        + struct.pack("!HHHHH", outer_type, outer_tci, 0x8100, inner_tci, payload_type)
        + payload
    )
    return frame.ljust(60, b"\0")


def pppoe_ipv4(payload):
    return struct.pack("!BBHHH", 0x11, 0, 1, len(payload) + 2, 0x0021) + payload


def receive_forwarded(receiver, sender, frame):
    sender.send(frame)
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            received, ancillary, _, _ = receiver.recvmsg(2048, 128)
        except TimeoutError:
            continue
        if received[:12] == frame[:12]:
            vlan = None
            for level, kind, data in ancillary:
                if level != SOL_PACKET or kind != PACKET_AUXDATA or len(data) < 20:
                    continue
                status, _, _, _, _, tci, tpid = struct.unpack_from(
                    "=IIIHHHH", data
                )
                if status & TP_STATUS_VLAN_VALID:
                    if not status & TP_STATUS_VLAN_TPID_VALID:
                        tpid = 0x8100
                    vlan = (tpid, tci)
                    break
            return received, vlan
    raise RuntimeError("test frame was not forwarded")


def forwarded_pcp(receiver, sender, frame):
    received, vlan = receive_forwarded(receiver, sender, frame)
    if vlan is not None:
        return vlan[1] >> 13
    if struct.unpack("!H", received[12:14])[0] not in (0x8100, 0x88A8, 0x9100):
        raise AssertionError("forwarded frame has no observable outer VLAN tag")
    return struct.unpack("!H", received[14:16])[0] >> 13


def expect_pcp(receiver, sender, frame, expected, label):
    actual = forwarded_pcp(receiver, sender, frame)
    if actual != expected:
        raise AssertionError(f"{label}: expected PCP {expected}, got {actual}")


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: ctc-classification-packets.py RULESET IP NFT ETHTOOL"
        )

    ruleset, ip_command, nft_command, ethtool_command = sys.argv[1:]

    run(ip_command, "link", "add", "br-test", "type", "bridge")
    run(ip_command, "link", "add", "lan1", "type", "veth", "peer", "name", "ctc-tx")
    run(ip_command, "link", "add", "lan2", "type", "veth", "peer", "name", "ctc-rx")
    run(ip_command, "link", "set", "lan1", "master", "br-test")
    run(ip_command, "link", "set", "lan2", "master", "br-test")
    run(ip_command, "link", "set", "br-test", "up")
    for interface in ("lan1", "lan2", "ctc-tx", "ctc-rx"):
        run(ip_command, "link", "set", interface, "up")
        run(
            ethtool_command,
            "-K",
            interface,
            "rxvlan",
            "off",
            "txvlan",
            "off",
            "rx-vlan-stag-hw-parse",
            "off",
            "tx-vlan-stag-hw-insert",
            "off",
        )
    run(nft_command, "-f", ruleset)
    install_direct_counter(nft_command)

    receiver = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
    sender = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
    receiver.setsockopt(SOL_PACKET, PACKET_AUXDATA, 1)
    receiver.bind(("ctc-rx", 0))
    sender.bind(("ctc-tx", 0))
    receiver.settimeout(0.1)

    direct = qinq(0x0800, ipv4_tcp(), 0x9100, 3)
    direct_pcp = forwarded_pcp(receiver, sender, direct)
    if direct_counter_packets(nft_command) != 1:
        raise AssertionError("0x9100 QinQ IPv4 did not match raw SDK fields")
    if direct_pcp != 5:
        raise AssertionError(f"0x9100 QinQ IPv4: expected PCP 5, got {direct_pcp}")

    pppoe = qinq(0x8864, pppoe_ipv4(ipv4_tcp()), 0x9100, 3)
    expect_pcp(receiver, sender, pppoe, 5, "QinQ PPPoE IPv4")

    source = "fe80::1"
    sdk_less_network_greater = qinq(
        0x86DD, ipv6_udp(source, "2101:0db7::"), 0x8100, 1
    )
    expect_pcp(
        receiver,
        sender,
        sdk_less_network_greater,
        6,
        "SDK little-endian IPv6 less-than",
    )

    sdk_greater_network_less = qinq(
        0x86DD, ipv6_udp(source, "1f01:0db9::"), 0x8100, 1
    )
    expect_pcp(
        receiver,
        sender,
        sdk_greater_network_less,
        1,
        "SDK little-endian IPv6 greater-than",
    )

    tcp = qinq(
        0x86DD,
        ipv6_tcp("2001:0db8::100", "3001:0db8::100"),
        0x88A8,
        1,
    )
    expect_pcp(receiver, sender, tcp, 7, "QinQ IPv6 TCP destination port")

    print("CTC QinQ, PPPoE, IPv6 TCP and SDK ordering packet tests passed")


if __name__ == "__main__":
    main()
