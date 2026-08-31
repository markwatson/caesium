#!/usr/bin/env python3
"""
Measure NTP offset/delay to a server, optionally pinning the socket to a
specific interface so a VPN tunnel cannot capture the route.

theta = ((T2 - T1) + (T3 - T4)) / 2      offset (server - local)
delta =  (T4 - T1) - (T3 - T2)           round-trip delay
"""
import argparse, socket, statistics, struct, sys, time

NTP_UNIX_DELTA = 2208988800
IP_BOUND_IF = 25  # macOS <netinet/in.h>


def to_unix(ts):
    return ((ts >> 32) & 0xFFFFFFFF) - NTP_UNIX_DELTA + (ts & 0xFFFFFFFF) / 2**32


def query(host, port, iface, timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if iface:
        s.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF,
                     struct.pack("I", socket.if_nametoindex(iface)))
    s.settimeout(timeout)
    pkt = bytearray(48)
    pkt[0] = 0x1B  # LI=0 VN=3 Mode=3 (client)
    try:
        t1 = time.time()
        s.sendto(bytes(pkt), (host, port))
        data, _ = s.recvfrom(48)
        t4 = time.time()
    finally:
        s.close()
    li_vn_mode, stratum = data[0], data[1]
    t2 = to_unix(struct.unpack("!Q", data[32:40])[0])
    t3 = to_unix(struct.unpack("!Q", data[40:48])[0])
    return {"offset": ((t2 - t1) + (t3 - t4)) / 2,
            "delay": (t4 - t1) - (t3 - t2),
            "proc": t3 - t2,
            "stratum": stratum,
            "li": (li_vn_mode >> 6) & 0x3}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("-n", "--count", type=int, default=100)
    ap.add_argument("-i", "--interval", type=float, default=0.1)
    ap.add_argument("-I", "--iface", help="pin socket to interface, e.g. en9")
    ap.add_argument("-p", "--port", type=int, default=123)
    ap.add_argument("-t", "--timeout", type=float, default=2.0)
    a = ap.parse_args()

    rs, fails = [], 0
    for k in range(a.count):
        try:
            rs.append(query(a.host, a.port, a.iface, a.timeout))
        except Exception as e:
            fails += 1
            if fails <= 3:
                print(f"  query {k} failed: {e}", file=sys.stderr)
        if k + 1 < a.count:
            time.sleep(a.interval)
    if not rs:
        print("all queries failed"); return 1

    off = [r["offset"] for r in rs]
    dly = [r["delay"] for r in rs]
    best = min(rs, key=lambda r: r["delay"])  # min-delay sample: least asymmetry

    print(f"host        : {a.host}  iface={a.iface or 'default route'}")
    print(f"samples     : {len(rs)} ok, {fails} failed")
    print(f"stratum     : {rs[0]['stratum']}   LI={rs[0]['li']}")
    print(f"delay  min  : {min(dly)*1e3:.3f} ms   median {statistics.median(dly)*1e3:.3f} ms"
          f"   max {max(dly)*1e3:.3f} ms")
    print(f"offset median: {statistics.median(off)*1e3:+.3f} ms"
          f"   sd {statistics.pstdev(off)*1e3:.3f} ms")
    print(f"offset @ min-delay sample : {best['offset']*1e3:+.3f} ms  "
          f"(delay {best['delay']*1e3:.3f} ms)")
    print(f"server processing T3-T2   : {statistics.median([r['proc'] for r in rs])*1e6:.1f} us")
    print(f"|bias| <= delay/2         : {min(dly)/2*1e3:.3f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
